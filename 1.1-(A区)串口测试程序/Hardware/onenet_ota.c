#include "onenet_ota.h"
#include "onenet_http.h"
#include "onenet_cfg.h"
#include "ota_layout.h"
#include "md5.h"
#include "W25Q64.h"
#include "m24c02.h"
#include "4G.h"
#include "Delay.h"
#include "usart.h"
#include <string.h>

/* ==========================================================================
 * 参数
 * ========================================================================== */

/* 每片下载 1024 字节。
 *   取小了：请求次数爆炸（256 字节要 51 次，每次都要重建 TCP）
 *   取大了：RAM 吃紧（A 区总共只有 20KB）
 *   1024 同时也是 W25Q64 的 4 个页，写起来是 4 次整页写，不跨页。 */
#define OTA_CHUNK_SIZE      1024

/* 小响应（/version、/check）的 body 缓冲。实测这些响应 90~190 字节。 */
#define OTA_JSON_BUF        384

/* 单片下载的接收超时。服务器一小片通常 100ms 内发完。 */
#define OTA_CHUNK_TIMEOUT   3000

/* 上报进度的最小间隔（百分比）。太密会拖慢下载 —— 每次上报都是一整轮
 * TCP 建连 + 请求 + 响应。 */
#define OTA_PROGRESS_STEP   10

static char    s_json[OTA_JSON_BUF];        /* /version、/check 的响应 */
static uint8_t s_chunk[OTA_CHUNK_SIZE];     /* 单片固件 */

/* ==========================================================================
 * 小工具
 * ========================================================================== */

/* uint32 -> 十进制字符串，返回长度。不用 sprintf 是为了省 1KB 左右的 Flash。 */
static uint8_t U32ToStr(uint32_t v, char *out)
{
	char    tmp[10];
	uint8_t n = 0, i;

	if(v == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
	while(v > 0) { tmp[n ++] = (char)('0' + (v % 10)); v /= 10; }
	for(i = 0; i < n; i ++) out[i] = tmp[n - 1 - i];
	out[n] = '\0';
	return n;
}

/* 从 JSON 文本里取 "key": <十进制数>。
 *
 * ⚠️ 为什么不用 cJSON：整个响应用到的字段就 4 个（code/tid/size/md5），
 *    引一个通用 JSON 库要多占 3~4KB Flash，还带 malloc ——
 *    在一个 RAM 只有 20KB、且不允许动态分配失败的设备上不划算。
 *    响应格式是 OneNET 固定的，按字段名直接找足够稳。
 *
 * 为什么要拼上引号再找：直接找 "tid" 会被 "request_id" 里的片段干扰不了，
 * 但找 "size" 可能命中别的字段名后缀。加引号能严格匹配字段名。 */
static uint8_t JsonU32(const char *json, const char *key, uint32_t *out)
{
	char        pat[24];
	const char *p;
	uint32_t    acc = 0;
	uint8_t     got = 0;

	if(strlen(key) + 3 > sizeof(pat)) return 0;
	pat[0] = '"';
	strcpy(pat + 1, key);
	strcat(pat, "\":");
	p = strstr(json, pat);
	if(p == 0) return 0;

	p += strlen(pat);
	while(*p == ' ') p ++;                          /* 容忍 "key": 12 这种写法 */
	while(*p >= '0' && *p <= '9')
	{
		acc = acc * 10 + (uint32_t)(*p - '0');
		p ++;
		got = 1;
	}
	if(!got) return 0;
	*out = acc;
	return 1;
}

/* 从 JSON 文本里取 "key":"<字符串>" 的值（不含引号） */
static uint8_t JsonStr(const char *json, const char *key, char *out, uint16_t cap)
{
	char        pat[32];
	const char *p, *q;
	uint16_t    n;

	if(strlen(key) + 4 > sizeof(pat)) return 0;
	pat[0] = '"';
	strcpy(pat + 1, key);
	strcat(pat, "\":\"");
	p = strstr(json, pat);
	if(p == 0) return 0;

	p += strlen(pat);
	q = strchr(p, '"');
	if(q == 0) return 0;

	n = (uint16_t)(q - p);
	if((uint16_t)(n + 1) > cap) return 0;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

/* 把整个 body 收进 buf 并补 '\0'，最多收 content_len 字节。
 * 返回 1 成功。P 号参数 timeout 是"一直没有新数据"的容忍时间。 */
static uint8_t ReadBodyAll(char *buf, uint16_t cap, uint32_t content_len, uint32_t timeout_ms)
{
	uint32_t got = 0, idle = 0;
	uint16_t want;

	if(content_len == 0xFFFFFFFFu) content_len = (uint32_t)(cap - 1);
	if(content_len > (uint32_t)(cap - 1)) content_len = (uint32_t)(cap - 1);

	while(got < content_len)
	{
		want = (uint16_t)(content_len - got);
		if(want > (uint16_t)(cap - 1 - got)) want = (uint16_t)(cap - 1 - got);

		{
			uint16_t k = HTTP_ReadBody((uint8_t *)(buf + got), want);
			if(k > 0) { got += k; idle = 0; continue; }
		}
		if(idle >= timeout_ms) break;
		Delay_ms(10);
		idle += 10;
	}
	buf[got] = '\0';
	return (got == content_len) ? 1 : 0;
}

/* ==========================================================================
 * 各步骤
 * ========================================================================== */

/* ① 上报当前版本。返回值：OTA_R_OK / OTA_R_FAIL_VERSION */
static uint8_t StepReportVersion(void)
{
	char      path[96];
	char      body[80];
	HTTP_Resp resp;
	uint8_t   rc;

	if(HTTP_DevPath(path, sizeof(path), "version") != 0) return OTA_R_FAIL_VERSION;

	/* s_version 是平台拿去和升级包「目标版本」比对的字段；
	 * f_version 是模组版本，本项目 ESP8266 的固件不参与 OTA，填占位值即可
	 * （但两个字段都必须出现，缺一个平台会报参数错误）。 */
	strcpy(body, "{\"s_version\":\"" APP_VERSION "\",\"f_version\":\"1.0.0\"}");

	rc = HTTP_Start("POST", path, body, (uint16_t)strlen(body), 0, &resp);
	if(rc != HTTP_OK)
	{
		U1_printf("[OTA] 上报版本：HTTP 层失败 rc=%u\r\n", (unsigned)rc);
		return OTA_R_FAIL_VERSION;
	}
	if(resp.status != 200)
	{
		U1_printf("[OTA] 上报版本：HTTP %u\r\n", (unsigned)resp.status);
		HTTP_End();
		return OTA_R_FAIL_VERSION;
	}

	if(!ReadBodyAll(s_json, sizeof(s_json), resp.content_len, 3000))
	{
		U1_printf("[OTA] 上报版本：收 body 超时\r\n");
		HTTP_End();
		return OTA_R_FAIL_VERSION;
	}
	HTTP_End();

	if(strstr(s_json, "\"code\":0") == 0)
	{
		U1_printf("[OTA] 上报版本被拒：%s\r\n", s_json);
		return OTA_R_FAIL_VERSION;
	}
	U1_printf("[OTA] 版本已上报：s_version=%s\r\n", APP_VERSION);
	return OTA_R_OK;
}

/* ② 检测升级任务。
 *    *has_task = 0 表示没有待办（含"平台说任务已完成"）；
 *    有任务时把 tid / size / md5 / target 带出来。 */
static uint8_t StepCheckTask(uint32_t *tid, uint32_t *size, char *md5, uint16_t md5cap,
                             char *target, uint16_t tcap, uint8_t *has_task)
{
	char      path[128], tail[96];
	HTTP_Resp resp;
	uint8_t   rc;
	uint32_t  code = 0;

	*has_task = 0;

	/* path = /fuse-ota/{pid}/{dn}/check?type=2&version=<当前版本>
	 * type=2 是 SOTA（MCU 软件）；type=1 是 FOTA（模组固件），不是我们要的。 */
	strcpy(tail, "check?type=2&version=");
	strncat(tail, APP_VERSION, sizeof(tail) - strlen(tail) - 1);
	if(HTTP_DevPath(path, sizeof(path), tail) != 0) return OTA_R_FAIL_CHECK;

	rc = HTTP_Start("GET", path, 0, 0, 0, &resp);
	if(rc != HTTP_OK)
	{
		U1_printf("[OTA] 检测任务：HTTP 层失败 rc=%u\r\n", (unsigned)rc);
		return OTA_R_FAIL_CHECK;
	}
	if(!ReadBodyAll(s_json, sizeof(s_json), resp.content_len, 3000))
	{
		U1_printf("[OTA] 检测任务：收 body 超时\r\n");
		HTTP_End();
		return OTA_R_FAIL_CHECK;
	}
	HTTP_End();

	if(!JsonU32(s_json, "code", &code))
	{
		U1_printf("[OTA] 检测任务：响应里没有 code：%s\r\n", s_json);
		return OTA_R_FAIL_CHECK;
	}

	/* 实测的三种 code：
	 *   0     有任务
	 *   12012 not exist     —— 没有针对这个版本的任务
	 *   12013 task succ     —— 任务已完成（设备版本已等于目标版本）
	 * 后两种都算"无待办"，不是故障。 */
	if(code != 0)
	{
		U1_printf("[OTA] 无待升级任务（平台返回 code=%u）\r\n", (unsigned)code);
		return OTA_R_OK;
	}

	if(!JsonU32(s_json, "tid",  tid) ||
	   !JsonU32(s_json, "size", size) ||
	   !JsonStr(s_json, "md5", md5, md5cap))
	{
		U1_printf("[OTA] 检测任务：字段缺失：%s\r\n", s_json);
		return OTA_R_FAIL_CHECK;
	}
	if(!JsonStr(s_json, "target", target, tcap)) target[0] = '\0';

	/* 平台理论上不会给"目标版本 == 当前版本"的任务，但真给了也不该傻下 13KB。
	 * 这一层防御很便宜，省得以后出怪事。 */
	if(target[0] != '\0' && strcmp(target, APP_VERSION) == 0)
	{
		U1_printf("[OTA] 平台给的目标版本与当前版本相同（%s），跳过\r\n", target);
		return OTA_R_OK;
	}

	*has_task = 1;
	U1_printf("[OTA] 有任务：tid=%u  target=%s  size=%u  md5=%s\r\n",
	          (unsigned)*tid, target, (unsigned)*size, md5);
	return OTA_R_OK;
}

/* 上报进度(0~100)或状态码(>100)。失败**不中断**下载 —— 见调用处说明。 */
static uint8_t StepReportStatus(uint32_t tid, uint32_t step)
{
	char      tail[64], tmp[12];
	char      path[128], body[32];
	HTTP_Resp resp;
	uint8_t   rc;

	U32ToStr(tid, tail);
	strcat(tail, "/status");
	if(HTTP_DevPath(path, sizeof(path), tail) != 0) return OTA_R_FAIL_STATUS;

	strcpy(body, "{\"step\":");
	U32ToStr(step, tmp);
	strcat(body, tmp);
	strcat(body, "}");

	rc = HTTP_Start("POST", path, body, (uint16_t)strlen(body), 0, &resp);
	if(rc != HTTP_OK) return OTA_R_FAIL_STATUS;

	/* 这里不读 body：状态上报的响应很短，而且我们只关心有没有被拒。
	 * 直接收尾，省一次读取和一次超时判断。 */
	HTTP_End();
	return (resp.status == 200) ? OTA_R_OK : OTA_R_FAIL_STATUS;
}

/* ③ 分片下载到 W25Q64，同时累加 MD5。返回 OTA_R_* */
static uint8_t StepDownload(uint32_t tid, uint32_t size, const char *expect_md5)
{
	char        tail[64], tmp[12], range[32];
	char        path[128];
	uint32_t    off = 0;
	uint32_t    last_pct = 0;
	MD5_CTX     ctx;
	uint8_t     digest[MD5_DIGEST_SIZE];
	char        got_md5[40];
	uint8_t     i, rc;

	/* --- 擦除 --- */
	/* W25Q64 的 Page Program 只能把 1 写成 0，要先把目标区擦回 0xFF。
	 * 块 0 是 64KB，而固件约 13KB —— 擦一整块而不是逐个 4KB 扇区，
	 * 是因为擦除指令本来就慢，少发几条反而快，而且块 0 本来就专供 OTA。 */
	U1_printf("[OTA] 擦除 W25Q64 块 %d ...\r\n", OTA_W25Q64_BLOCK);
	W25Q64_Erase64K(OTA_W25Q64_BLOCK);

	MD5_Init(&ctx);

	/* --- 下载循环 --- */
	/* ⚠️ 从这里开始关掉 AT 日志。
	 * G4_SetVerbose(1) 会把整个接收缓冲 dump 出来，收固件时一次 dump 就能
	 * 把缓冲吃掉、导致丢字节。调试连接问题时才需要它，下载时必须闭嘴。 */
	G4_SetVerbose(0);

	while(off < size)
	{
		uint32_t n = size - off;
		uint32_t pct;
		HTTP_Resp resp;
		uint16_t  k;

		if(n > OTA_CHUNK_SIZE) n = OTA_CHUNK_SIZE;

		/* Range 是**闭区间**："0-1023" 表示前 1024 个字节。
		 * 写成 "0-1024" 会多要一个字节 —— 平台允许，但下一片的起点会错位。 */
		U32ToStr(off, range);
		strcat(range, "-");
		U32ToStr(off + n - 1, tmp);
		strcat(range, tmp);

		U32ToStr(tid, tail);
		strcat(tail, "/download");
		if(HTTP_DevPath(path, sizeof(path), tail) != 0) { G4_SetVerbose(1); return OTA_R_FAIL_DOWNLOAD; }

		rc = HTTP_Start("GET", path, 0, 0, range, &resp);
		if(rc != HTTP_OK)
		{
			G4_SetVerbose(1);
			U1_printf("[OTA] 下载第 %u 片：HTTP 层失败 rc=%u\r\n",
			          (unsigned)(off / OTA_CHUNK_SIZE), (unsigned)rc);
			return OTA_R_FAIL_DOWNLOAD;
		}

		/* ⚠️ 两个判据都要看，而且顺序有讲究：
		 *   Ota-Errno 是 OneNET 自己的错误码，**放在响应头里**。
		 *   文件不存在 / 任务过期 / 鉴权失败时，HTTP 状态码可能仍是 200，
		 *   只有 Ota-Errno 说实话。先查它，报错信息才准确。 */
		if(resp.ota_errno != 0)
		{
			G4_SetVerbose(1);
			U1_printf("[OTA] 下载被拒：Ota-Errno=%d（1设备不存在 2文件不存在 "
			          "3对象存储无资源 4大小不一致 5任务过期 6鉴权失败）\r\n",
			          (int)resp.ota_errno);
			HTTP_End();
			return OTA_R_FAIL_DOWNLOAD;
		}
		if(resp.status != 206 && resp.status != 200)
		{
			G4_SetVerbose(1);
			U1_printf("[OTA] 下载第 %u 片：HTTP %u\r\n",
			          (unsigned)(off / OTA_CHUNK_SIZE), (unsigned)resp.status);
			HTTP_End();
			return OTA_R_FAIL_DOWNLOAD;
		}

		/* 收满 n 字节。HTTP_ReadBody 返回 0 只表示"暂时没有"，
		 * 不代表结束 —— 必须自己按长度收，收不满就算超时失败。 */
		{
			uint32_t got = 0, idle = 0;
			while(got < n)
			{
				k = HTTP_ReadBody(s_chunk + got, (uint16_t)(n - got));
				if(k > 0) { got += k; idle = 0; continue; }
				if(idle >= OTA_CHUNK_TIMEOUT) break;
				Delay_ms(10);
				idle += 10;
			}
			if(got != n)
			{
				G4_SetVerbose(1);
				U1_printf("[OTA] 第 %u 片只收到 %u/%u 字节就断了\r\n",
				          (unsigned)(off / OTA_CHUNK_SIZE), (unsigned)got, (unsigned)n);
				HTTP_End();
				return OTA_R_FAIL_DOWNLOAD;
			}
		}
		HTTP_End();

		/* ⚠️ 重新关掉 AT 日志。
		 * HTTP_End() 内部为了压掉 CIPCLOSE 的假 ERROR，会把 verbose 关掉再恢复成 1，
		 * 所以每片收尾之后它又开了。下一片的 G4_TcpConnect 就会 dump 一次接收缓冲 ——
		 * 不在这里重新关掉，13 片下来丢字节是迟早的事。 */
		G4_SetVerbose(0);

		/* --- 写 W25Q64 ---
		 * Page Program 一次最多 256 字节，且**不能跨页边界**（跨了会绕回页首，
		 * 把已经写好的数据盖掉 —— 这是 W25Q64 最经典的坑）。
		 * 下面按"距本页末尾还剩多少"来切，天然不会跨页。 */
		{
			uint32_t done = 0;
			while(done < n)
			{
				uint32_t room = 256 - ((off + done) % 256);
				uint16_t piece = (uint16_t)((n - done < room) ? (n - done) : room);
				W25Q64_PageProgram((off + done) / 256, s_chunk + done, piece);
				done += piece;
			}
		}

		MD5_Update(&ctx, s_chunk, n);
		off += n;

		/* --- 报进度 --- */
		pct = (uint32_t)(off * 100 / size);
		if(pct >= last_pct + OTA_PROGRESS_STEP || off >= size)
		{
			/* 上报失败**不中断下载**：
			 * 这一路只是给控制台看进度用的，不参与正确性判定（正确性由 MD5 保证）。
			 * 为一次网络抖就把已经下了 90% 的固件丢掉，代价远大于收益。
			 * 任务若真的失效了，后面每一片都会失败，MD5 和 Ota-Errno 会兜住。 */
			if(StepReportStatus(tid, pct) != OTA_R_OK)
			{
				U1_printf("[OTA] （进度上报失败，继续下载）\r\n");
			}
			last_pct = pct;
		}
	}

	G4_SetVerbose(1);

	/* --- 校验 --- */
	MD5_Final(&ctx, digest);
	for(i = 0; i < MD5_DIGEST_SIZE; i ++)
	{
		static const char HEX[] = "0123456789abcdef";
		got_md5[i*2 + 0] = HEX[digest[i] >> 4];
		got_md5[i*2 + 1] = HEX[digest[i] & 0x0F];
	}
	got_md5[MD5_DIGEST_SIZE*2] = '\0';

	U1_printf("[OTA] 下载完成 %u 字节\r\n", (unsigned)off);
	U1_printf("[OTA] MD5 本地 %s\r\n", got_md5);
	U1_printf("[OTA] MD5 平台 %s\r\n", expect_md5);

	if(strcmp(got_md5, expect_md5) != 0)
	{
		U1_printf("[OTA] ✗ MD5 不一致 —— 放弃本次升级（不置标志，A 区保持原样）\r\n");
		return OTA_R_FAIL_MD5;
	}
	U1_printf("[OTA] ✓ MD5 一致\r\n");
	return OTA_R_OK;
}

/* ④ 置标志并复位，交给 BootLoader 搬运 */
static void StepApply(uint32_t size)
{
	/* 顺序很重要：先把长度和标志都写进结构体，再一次性写进 AT24C02。
	 * 如果分开写两次，中间掉电可能留下"标志置了但长度是旧的"的状态 ——
	 * BootLoader 会照着旧长度搬一段残缺的固件进 A 区。 */
	OTA_Info.OTA_Flag = OTA_SET_FLAG;
	OTA_Info.FileLen[OTA_W25Q64_BLOCK] = size;
	AT24C02_WriteOTAInfo();

	U1_printf("[OTA] 标志已写入 AT24C02：OTA_Flag=0x%08X  FileLen[%d]=%u\r\n",
	          (unsigned)OTA_SET_FLAG, OTA_W25Q64_BLOCK, (unsigned)size);
	U1_printf("[OTA] 即将复位，由 BootLoader 搬运到 A 区\r\n");

	/* 给串口一点时间把上面几行发完再复位。不延时的话，
	 * 最关键的"标志写好了"这行往往来不及发出去，看起来像是卡死在下载。 */
	Delay_ms(300);
	NVIC_SystemReset();
}

/* ==========================================================================
 * 主流程
 * ========================================================================== */

uint8_t OTA_Run(void)
{
	uint32_t tid = 0, size = 0;
	char     md5[40], target[40];
	uint8_t  has_task = 0;
	uint8_t  r;

	U1_printf("\r\n===== OneNET OTA =====\r\n");

	/* 第 ① 步不是可有可无的：它同时向平台"声明设备当前版本"。
	 * 升级成功之后新固件第一次跑，就是靠这一步让平台把任务标记为完成。 */
	r = StepReportVersion();
	if(r != OTA_R_OK) { U1_printf("===== OTA 结束 rc=%u =====\r\n\r\n", (unsigned)r); return r; }

	r = StepCheckTask(&tid, &size, md5, sizeof(md5), target, sizeof(target), &has_task);
	if(r != OTA_R_OK || !has_task)
	{
		U1_printf("===== OTA 结束 rc=%u =====\r\n\r\n", (unsigned)r);
		return r;
	}

	/* 防御：避免平台给了个离谱的 size 让我们写爆 W25Q64 的块 0。
	 * 一块 64KB，固件远小于它；但"远小于"是假设，不是保证。 */
	if(size == 0 || size > 64u * 1024u)
	{
		U1_printf("[OTA] 固件长度 %u 不合理（块 0 是 64KB），放弃\r\n", (unsigned)size);
		U1_printf("===== OTA 结束 rc=%u =====\r\n\r\n", (unsigned)OTA_R_FAIL_CHECK);
		return OTA_R_FAIL_CHECK;
	}

	r = StepDownload(tid, size, md5);
	if(r != OTA_R_OK)
	{
		U1_printf("===== OTA 结束 rc=%u =====\r\n\r\n", (unsigned)r);
		return r;
	}

	/* 告诉平台"下载完成" —— 平台会把设备状态从「下载中」转成「升级中」。
	 * 之后再报 201 就多余了（文档明确说 100 和 101 等价，不用再传 101）。 */
	if(StepReportStatus(tid, 100) != OTA_R_OK)
	{
		U1_printf("[OTA] （下载完成状态上报失败，仍继续搬运）\r\n");
	}

	StepApply(size);            /* 里面会复位，不会返回 */
	return OTA_R_APPLYING;
}
