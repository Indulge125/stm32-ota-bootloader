#include "onenet_http.h"
#include "onenet_token.h"
#include "onenet_cfg.h"
#include "4G.h"
#include "Delay.h"
#include "usart.h"
#include <string.h>

/* ==========================================================================
 * Token 的过期时间戳（et）
 * ==========================================================================
 *
 * MCU 上没有时钟，算不出"当前时间"，而 OneNET 要求 et 是一个 Unix 时间戳。
 * 实测（2026-10-08）OneNET 的约束是：
 *
 *     now  <=  et  <=  now + 10 年
 *
 * 超出后者的报错是 "token expire is too long, max expire limit 10 years"；
 * 低于前者的报错是 "request has expired:expire=...,now=..."。
 *
 * 所以硬编码一个**落在窗口内**的值：
 *     et = 2082758400 = 2036-01-01 00:00:00 UTC
 *     有效窗口 = [2026-01-01, 2036-01-01]，约 9 年
 * （实测 2036-01-01 通过、2038-01-01 被拒，边界就在 10 年处。）
 *
 * 这是**刻意的取舍**：换来"完全不需要对时逻辑"。代价是 token 长期有效，
 * 安全性差 —— 演示/学习项目可接受。
 *
 * 以后要做正式产品，正确的做法是：从响应头的 Date 字段学真实时间
 * （服务器一定会回 Date），此后把 et 设成 now + 1 小时。
 * 现在留个记号，别以为这里是随手写的数。 */
#define ONENET_TOKEN_ET         2082758400u

/* --------------------------------------------------------------------------
 * 静态状态
 * -------------------------------------------------------------------------- */
static char     s_req[HTTP_REQ_BUF];    /* 请求报文 */
static char     s_hdr[HTTP_HDR_BUF];    /* 响应头累积缓冲 */

static uint16_t s_hdr_len;              /* s_hdr 里已累积的字节数 */
static uint16_t s_body_off;             /* s_hdr 里 body 的起始偏移（0 = 还没解析出） */
static uint16_t s_body_avail;           /* 随响应头一起收进来的 body 字节数 */
static uint16_t s_body_sent;            /* 其中已经交给调用者的字节数 */

static uint8_t  s_open;                 /* TCP 是否还开着 */
static uint8_t  s_wifi_ready;           /* WiFi 是否已经连上（连一次就够，不必每请求重连） */

/* ==========================================================================
 * 简单的字符串拼接器
 * ==========================================================================
 * 和 onenet_token.c 里那份是同一套路子，但那边的是 static，不跨文件暴露。
 * 这里重新写一份而不是把 token 模块的内部工具提成公共 API ——
 * 这两处拼的都是各自模块的报文，硬凑一个公共层反而增加耦合。
 * 代价是约 30 行重复；这几个函数不会变，重复的风险可以接受。
 *
 * 不用 sprintf 家族：ARMCC 的 printf 引擎链进来要 1KB 上下，
 * A 区 Flash 还要留给 OTA 逻辑。 */
typedef struct
{
	char     *buf;
	uint16_t  cap;      /* 含结尾 '\0' 的总容量 */
	uint16_t  len;
}StrBuf;

static uint8_t SB_AppendN(StrBuf *sb, const char *s, uint16_t n)
{
	uint16_t i;
	if((uint16_t)(sb->len + n + 1) > sb->cap) return 0;
	for(i = 0; i < n; i ++) sb->buf[sb->len ++] = s[i];
	sb->buf[sb->len] = '\0';
	return 1;
}

static uint8_t SB_Append(StrBuf *sb, const char *s)
{
	return SB_AppendN(sb, s, (uint16_t)strlen(s));
}

static uint8_t SB_AppendU32(StrBuf *sb, uint32_t v)
{
	char    tmp[10];
	uint8_t n = 0;
	if(v == 0) tmp[n ++] = '0';
	while(v > 0) { tmp[n ++] = (char)('0' + (v % 10)); v /= 10; }
	if((uint16_t)(sb->len + n + 1) > sb->cap) return 0;
	while(n > 0) { n --; sb->buf[sb->len ++] = tmp[n]; }
	sb->buf[sb->len] = '\0';
	return 1;
}

/* ==========================================================================
 * 诊断输出
 * ========================================================================== */

/* 打十六进制。失败时最有用的东西就是"到底收到了什么字节" ——
 * 只说一句"超时"等于把线索丢掉。len 会截断，避免刷屏。 */
static void DumpHex(const uint8_t *p, uint16_t len)
{
	uint16_t i;
	uint16_t n = (len > 64) ? 64 : len;

	for(i = 0; i < n; i ++)
	{
		U1_printf("%02X ", p[i]);
		if((i & 15) == 15) U1_printf("\r\n");
	}
	if(len > n) U1_printf("\r\n...(共 %u 字节，只打前 64)", (unsigned)len);
	U1_printf("\r\n");
}

/* ==========================================================================
 * 响应头解析
 * ========================================================================== */

/* 在 s_hdr 里找 "\r\n\r\n"，返回它之后的位置（body 起点）；找不到返回 0。
 * 从下标 3 开始扫，所以一进来就能命中"头结束紧接着就是空头"的极端情况。 */
static uint16_t FindHdrEnd(void)
{
	uint16_t i;
	if(s_hdr_len < 4) return 0;
	for(i = 3; i < s_hdr_len; i ++)
	{
		if(s_hdr[i-3] == '\r' && s_hdr[i-2] == '\n' &&
           s_hdr[i-1] == '\r' && s_hdr[i]   == '\n')
		{
			return (uint16_t)(i + 1);
		}
	}
	return 0;
}

/* 找名为 name 的头（**大小写不敏感**，HTTP 头名不区分大小写），
 * 把它的值的 [起, 止) 区间写进 *vs / *ve。
 * 返回 1 = 找到。值不含结尾的 '\r'，也不含冒号后的空格。 */
static uint8_t HeaderFind(const char *name, uint16_t *vs, uint16_t *ve)
{
	uint16_t nlen = (uint16_t)strlen(name);
	uint16_t i = 0;

	/* 跳过状态行 */
	while(i < s_hdr_len && s_hdr[i] != '\n') i ++;
	if(i < s_hdr_len) i ++;

	while(i < s_hdr_len)
	{
		uint16_t ls = i, le, k, v;

		while(i < s_hdr_len && s_hdr[i] != '\n') i ++;
		le = i;                                  /* 该行不含 '\n' 的结束位置 */
		if(i < s_hdr_len) i ++;

		/* 行尾可能带 '\r'，比较时排除掉 */
		{
			uint16_t le2 = le;
			if(le2 > ls && s_hdr[le2-1] == '\r') le2 --;

			if((uint16_t)(le2 - ls) <= nlen) continue;

			for(k = 0; k < nlen; k ++)
			{
				char a = s_hdr[ls + k], b = name[k];
				if(a >= 'A' && a <= 'Z') a = (char)(a + 32);
				if(b >= 'A' && b <= 'Z') b = (char)(b + 32);
				if(a != b) break;
			}
			if(k != nlen) continue;                  /* 名字不一致 */
			if(s_hdr[ls + nlen] != ':') continue;    /* 必须紧跟冒号，避免前缀误命中 */

			v = (uint16_t)(ls + nlen + 1);
			while(v < le2 && (s_hdr[v] == ' ' || s_hdr[v] == '\t')) v ++;

			*vs = v; *ve = le2;
			return 1;
		}
	}
	return 0;
}

/* 取头的十进制值。返回 1 = 找到且值全是数字 */
static uint8_t HeaderGetU32(const char *name, uint32_t *out)
{
	uint16_t vs, ve, i;
	uint32_t acc = 0;
	uint8_t  got = 0;

	if(!HeaderFind(name, &vs, &ve)) return 0;
	for(i = vs; i < ve; i ++)
	{
		if(s_hdr[i] < '0' || s_hdr[i] > '9') break;
		acc = acc * 10 + (uint32_t)(s_hdr[i] - '0');
		got = 1;
	}
	if(!got) return 0;
	*out = acc;
	return 1;
}

/* 取 Content-Range 的总长度："bytes 0-1023/13028" 里的 13028。
 * 单独写一个是因为它带 'bytes ' 前缀和 '/' 分隔，不是纯数字。 */
static uint8_t HeaderGetRangeTotal(uint32_t *out)
{
	uint16_t vs, ve, i;
	uint32_t acc = 0;
	uint8_t  got = 0;

	if(!HeaderFind("Content-Range", &vs, &ve)) return 0;

	/* 找 '/' */
	for(i = vs; i < ve; i ++) if(s_hdr[i] == '/') break;
	if(i >= ve) return 0;
	i ++;

	for(; i < ve; i ++)
	{
		if(s_hdr[i] < '0' || s_hdr[i] > '9') break;
		acc = acc * 10 + (uint32_t)(s_hdr[i] - '0');
		got = 1;
	}
	if(!got) return 0;
	*out = acc;
	return 1;
}

/* 解析状态行和各个头。返回 HTTP_OK 或 HTTP_ERR_FORMAT */
static uint8_t ParseHeaders(HTTP_Resp *resp)
{
	uint16_t i = 0;
	uint32_t v;

	/* 状态行形如 "HTTP/1.1 200 OK" */
	if(s_hdr_len < 12 ||
       s_hdr[0] != 'H' || s_hdr[1] != 'T' || s_hdr[2] != 'T' || s_hdr[3] != 'P')
	{
		G4_SetVerbose(1);
		U1_printf("[HTTP] 响应不像 HTTP，开头 16 字节：\r\n");
		DumpHex((const uint8_t *)s_hdr, (s_hdr_len < 16) ? s_hdr_len : 16);
		return HTTP_ERR_FORMAT;
	}

	/* 跳过 "HTTP/1.1"，取第一段连续数字作为状态码 */
	while(i < s_hdr_len && s_hdr[i] != ' ') i ++;
	while(i < s_hdr_len && s_hdr[i] == ' ') i ++;
	while(i < s_hdr_len && s_hdr[i] >= '0' && s_hdr[i] <= '9')
	{
		resp->status = (uint16_t)(resp->status * 10 + (s_hdr[i] - '0'));
		i ++;
	}

	if(HeaderGetU32("Content-Length", &v))  resp->content_len   = v;
	if(HeaderGetU32("Ota-Errno",      &v))  resp->ota_errno     = (int32_t)v;
	if(HeaderGetRangeTotal(&v))             resp->content_total = v;

	return HTTP_OK;
}

/* ==========================================================================
 * 请求报文拼装
 * ========================================================================== */

uint8_t HTTP_DevPath(char *out, uint16_t cap, const char *tail)
{
	StrBuf sb;
	sb.buf = out; sb.cap = cap; sb.len = 0; out[0] = '\0';

	if(!SB_Append(&sb, "/fuse-ota/"))            return 2;
	if(!SB_Append(&sb, ONENET_PRO_ID))           return 2;
	if(!SB_Append(&sb, "/"))                     return 2;
	if(!SB_Append(&sb, ONENET_DEV_NAME))         return 2;
	if(!SB_Append(&sb, "/"))                     return 2;
	if(!SB_Append(&sb, tail))                    return 2;
	return 0;
}

static uint8_t BuildRequest(const char *method, const char *path,
							const char *body, uint16_t body_len,
							const char *range)
{
	char     auth[ONENET_AUTH_MAX];
	StrBuf   sb;
	uint8_t  r;

	/* 每次请求都重算 Token —— et 是固定的，但重算比缓存更不容易出错，
     * 代价只是几百个周期。 */
	r = OneNet_BuildAuthForDevice(ONENET_ACCESS_KEY, ONENET_PRO_ID, ONENET_DEV_NAME,
                                  ONENET_TOKEN_ET, auth, sizeof(auth));
	if(r != 0)
	{
		U1_printf("[HTTP] Token 构造失败 rc=%u\r\n", (unsigned)r);
		return 1;
	}

	sb.buf = s_req; sb.cap = HTTP_REQ_BUF; sb.len = 0; s_req[0] = '\0';

	if(!SB_Append(&sb, method))                             return 2;
	if(!SB_Append(&sb, " "))                                return 2;
	if(!SB_Append(&sb, path))                               return 2;
	if(!SB_Append(&sb, " HTTP/1.1\r\n"))                    return 2;
	if(!SB_Append(&sb, "Host: " ONENET_API_HOST "\r\n"))    return 2;
	if(!SB_Append(&sb, "Authorization: "))                  return 2;
	if(!SB_Append(&sb, auth))                               return 2;
	if(!SB_Append(&sb, "\r\n"))                             return 2;

	if(body_len > 0)
	{
		if(!SB_Append(&sb, "Content-Type: application/json\r\n")) return 2;
		if(!SB_Append(&sb, "Content-Length: "))             return 2;
		if(!SB_AppendU32(&sb, body_len))                    return 2;
		if(!SB_Append(&sb, "\r\n"))                         return 2;
	}
	if(range)
	{
		if(!SB_Append(&sb, "Range: "))                      return 2;
		if(!SB_Append(&sb, range))                          return 2;
		if(!SB_Append(&sb, "\r\n"))                         return 2;
	}
	if(!SB_Append(&sb, "Connection: close\r\n\r\n"))        return 2;

	/* body 用带长度的追加，不用 SB_Append —— JSON 里不该有 '\0'，
     * 但按长度走更稳妥，也免得以后换二进制 body 时踩坑。 */
	if(body_len > 0)
	{
		if(!SB_AppendN(&sb, body, body_len))                return 2;
	}
	return 0;
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

/* 连一次 WiFi 就够，不必每个请求重连。
 * 返回 0 成功。 */
static uint8_t EnsureWiFi(void)
{
	if(s_wifi_ready) return 0;

	G4_Init(G4_BAUD_DEFAULT);

	/* ⚠️ 必须先切到 Station 模式再 CWJAP。
     * ESP-AT 上电默认是 SoftAP，此时发 AT+CWJAP 会**秒回 ERROR**。
     * 这里踩过一次：只调 JoinAP 不调 SetStation，现象是"连 WiFi 失败"，
     * 而如果 AT 日志被关掉，几乎不可能想到卡在"模式没切"这一步。
     * 4G.c 里 G4_Connect() 的顺序就是 SetStation → JoinAP → TcpConnect，照它来。 */
	if(G4_SetStation() != G4_OK)
	{
		U1_printf("[HTTP] 切 Station 模式失败（AT+CWMODE=1）—— "
                  "模组没应答？查 USB 供电、PA2/PA3 接线、PB2 跳线\r\n");
		return 1;
	}

	if(G4_JoinAP(WIFI_SSID, WIFI_PASS) != G4_OK)
	{
		U1_printf("[HTTP] 连 WiFi 失败 —— 查 wifi_cfg.h 里的 SSID/密码，"
                  "以及模组是否在范围内\r\n");
		return 1;
	}
	s_wifi_ready = 1;
	return 0;
}

uint8_t HTTP_Start(const char *method, const char *path,
                   const char *body, uint16_t body_len,
                   const char *range, HTTP_Resp *resp)
{
	uint32_t waited;
	uint8_t  r;

	/* 先把输出清干净，任何提前返回都不会留下上次的脏值 */
	resp->status        = 0;
	resp->ota_errno     = -1;
	resp->content_len   = 0xFFFFFFFFu;
	resp->content_total = 0xFFFFFFFFu;

	s_hdr_len = 0; s_body_off = 0; s_body_avail = 0; s_body_sent = 0;
	s_open = 0;

	if(BuildRequest(method, path, body, body_len, range) != 0)
	{
		U1_printf("[HTTP] 请求报文超过 %d 字节，装不下\r\n", HTTP_REQ_BUF);
		return HTTP_ERR_FORMAT;
	}

	/* verbose 不在这里动，交给调用者。
     * 理由：AT 日志是"连不上"时唯一能看出原因的东西，调试期必须开着。
     * 真正需要闭嘴的是 M4 的固件下载循环（一次 dump 能吃掉整个接收缓冲），
     * 那由下载逻辑自己在循环前后开关 —— 不要在这里替它决定。
     *
     * 下面错误路径里的 G4_SetVerbose(1) 是**兜底**：确保诊断信息一定打得出来，
     * 即使调用者之前把它关了。 */
	if(EnsureWiFi() != 0) { G4_SetVerbose(1); return HTTP_ERR_LINK; }

	if(G4_TcpConnect(ONENET_API_HOST, ONENET_API_PORT) != G4_OK)
	{
		G4_SetVerbose(1);
		U1_printf("[HTTP] 连不上 %s:%d —— 查网络、DNS、模组固件是否正常\r\n",
                  ONENET_API_HOST, ONENET_API_PORT);
		return HTTP_ERR_LINK;
	}
	s_open = 1;
	G4_PayloadReset();

	if(G4_TcpSend((const uint8_t *)s_req, (uint16_t)strlen(s_req)) != G4_OK)
	{
		G4_SetVerbose(1);
		U1_printf("[HTTP] 发送失败，请求报文 %u 字节\r\n",
                  (unsigned)strlen(s_req));
		HTTP_End();
		return HTTP_ERR_LINK;
	}

	/* --- 等响应头：累积到出现 "\r\n\r\n" --- */
	waited = 0;
	while(waited < HTTP_HDR_TIMEOUT_MS)
	{
		uint16_t room = (uint16_t)(HTTP_HDR_BUF - s_hdr_len);
		uint16_t n;

		if(room == 0)
		{
			G4_SetVerbose(1);
			U1_printf("[HTTP] 响应头超过 %d 字节还没结束 —— 已经收到的：\r\n", HTTP_HDR_BUF);
			DumpHex((const uint8_t *)s_hdr, s_hdr_len);
			HTTP_End();
			return HTTP_ERR_OVERFLOW;
		}

		n = G4_PayloadRead((uint8_t *)(s_hdr + s_hdr_len), room);
		if(n > 0)
		{
			s_hdr_len += n;

			s_body_off = FindHdrEnd();
			if(s_body_off > 0)
			{
				/* 头结束。注意：这次读进来的可能还有 body 的前几个字节，
                 * 它们已经在 s_hdr 里了 —— 记下来，让 ReadBody 先交出去，
                 * 否则这几个字节就丢了（固件开头几个字节错位，CRC 必挂）。 */
				s_body_avail = (uint16_t)(s_hdr_len - s_body_off);
				s_body_sent  = 0;

				G4_SetVerbose(1);
				r = ParseHeaders(resp);
				if(r != HTTP_OK) { HTTP_End(); return r; }

				U1_printf("[HTTP] %u %s -> %u，Ota-Errno=%d，Content-Length=%u\r\n",
                          (unsigned)resp->status,
                          (resp->status == 200) ? "OK" : "?",
                          (unsigned)resp->status,
                          (int)resp->ota_errno,
                          (unsigned)resp->content_len);

				/* verbose 保持开着：调用者取 body 时能看到串口回显，
                 * 但 body 走的是 G4_PayloadRead，不受 verbose 影响。 */
				return HTTP_OK;
			}
			/* 没到 '\r\n\r\n' 就继续收，别 Delay —— 数据还在路上 */
		}
		else
		{
			/* 模组还没吐出载荷。检查一下是不是接收缓冲爆了 ——
             * 爆了就是静默丢字节，光等下去只会等到超时，查不出原因。 */
			if(G4_RxOverflow())
			{
				G4_SetVerbose(1);
				U1_printf("[HTTP] 模组接收缓冲溢出过（丢字节了），已收到 %u 字节：\r\n",
                          (unsigned)s_hdr_len);
				DumpHex((const uint8_t *)s_hdr, s_hdr_len);
				HTTP_End();
				return HTTP_ERR_OVERFLOW;
			}
			Delay_ms(10);
			waited += 10;
		}
	}

	G4_SetVerbose(1);
	U1_printf("[HTTP] 等响应头超时 %ums，已收到 %u 字节：\r\n",
              (unsigned)waited, (unsigned)s_hdr_len);
	DumpHex((const uint8_t *)s_hdr, s_hdr_len);
	HTTP_End();
	return HTTP_ERR_TIMEOUT;
}

uint16_t HTTP_ReadBody(uint8_t *buf, uint16_t max)
{
	uint16_t n;

	/* 先把"跟着响应头一起收进来"的那几个字节交出去。
     * 顺序不能颠倒：这些字节在流里排在前面。 */
	if(s_body_sent < s_body_avail)
	{
		n = (uint16_t)(s_body_avail - s_body_sent);
		if(n > max) n = max;
		memcpy(buf, s_hdr + s_body_off + s_body_sent, n);
		s_body_sent += n;
		return n;
	}

	if(!s_open) return 0;

	return G4_PayloadRead(buf, max);
}

void HTTP_End(void)
{
	if(s_open)
	{
		/* ⚠️ 这里的 AT+CIPCLOSE 十有八九会回 ERROR，**那是正常的，不是故障**。
		 *
		 * 因为每个请求都带 Connection: close，服务器响应完就主动关了连接。
		 * 此时再发 CIPCLOSE，模组发现"没有活动连接可关"，就回 ERROR。
		 * 但 G4_TcpClose() 内部把它当失败，会打一行 [FAIL] 模组回ERROR/FAIL ——
		 * 那行日志会让人以为请求出问题了，是个纯粹的假警报。
		 *
		 * 所以先在 verbose 关掉的状态下关连接，把这条噪音压掉，再恢复。
		 * （真正的连接失败在 HTTP_Start 里早就被拦下了，走到这儿就是正常收尾。） */
		G4_SetVerbose(0);
		G4_TcpClose();
		G4_SetVerbose(1);
		s_open = 0;
	}
}
