#include "onenet_token.h"
#include "sha1.h"
#include "base64.h"
#include <string.h>

/* Token 格式里的两个固定字段。改这两个值等于换协议版本，不要随手改。 */
#define TOK_VERSION		"2022-05-01"
#define TOK_METHOD		"sha1"

/* ------------------------------------------------------------------
 * 不用 sprintf 家族的原因：
 *   A 区 Flash 余量紧张，而 ARMCC 的 printf 引擎（含浮点/长整型支持）
 *   链进来要 1KB 上下，这里只需要"整数转十进制"和"URL 转义"两件小事，
 *   手写更划算。
 * ------------------------------------------------------------------ */

typedef struct
{
	char     *buf;
	uint16_t  cap;		/* 缓冲区总容量，含结尾 '\0' */
	uint16_t  len;		/* 当前已写长度 */
}StrBuf;

static uint8_t SB_Append(StrBuf *sb, const char *s)
{
	while(*s)
	{
		if((uint16_t)(sb->len + 1) >= sb->cap) return 0;
		sb->buf[sb->len ++] = *s ++;
	}
	sb->buf[sb->len] = '\0';
	return 1;
}

static uint8_t SB_AppendU32(StrBuf *sb, uint32_t v)
{
	char    tmp[10];
	uint8_t n = 0;

	if(v == 0)
	{
		tmp[n ++] = '0';
	}
	else
	{
		while(v > 0)
		{
			tmp[n ++] = (char)('0' + (v % 10));
			v /= 10;
		}
	}
	/* tmp 里是逆序的，倒着写出去 */
	while(n > 0)
	{
		n --;
		if((uint16_t)(sb->len + 1) >= sb->cap) return 0;
		sb->buf[sb->len ++] = tmp[n];
	}
	sb->buf[sb->len] = '\0';
	return 1;
}

/* URL 转义（RFC 3986）：unreserved 字符原样，其余转 %XX（大写十六进制）。
 * 为什么必须要这一步：
 *   res 里的 '/' 和 base64 签名里的 '+' '/' '=' 都必须转义。
 *   不转义的话，服务端解析出的 res 和参与签名的 res 就不是同一个字符串，
 *   表现是一直返回 10403 auth failed，且看不出是哪一步错的。
 * 转义集合与 Python urllib.parse.quote(s, safe="") 保持一致 —— PC 侧对标
 * 脚本用同一个定义，两边才能逐字节对上。 */
static uint8_t SB_AppendUrlEnc(StrBuf *sb, const char *s)
{
	static const char HEX[] = "0123456789ABCDEF";

	while(*s)
	{
		uint8_t c = (uint8_t)(*s);

		if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		   (c >= '0' && c <= '9') ||
		   c == '-' || c == '_' || c == '.' || c == '~')
		{
			if((uint16_t)(sb->len + 1) >= sb->cap) return 0;
			sb->buf[sb->len ++] = (char)c;
		}
		else
		{
			if((uint16_t)(sb->len + 3) >= sb->cap) return 0;
			sb->buf[sb->len ++] = '%';
			sb->buf[sb->len ++] = HEX[c >> 4];
			sb->buf[sb->len ++] = HEX[c & 0x0F];
		}
		s ++;
	}
	sb->buf[sb->len] = '\0';
	return 1;
}

uint8_t OneNet_BuildAuth(const char *access_key_b64,
                         const char *res,
                         uint32_t et,
                         char *out, uint16_t out_size)
{
	uint8_t  key[64];					/* 解码后的密钥。OneNET 的 key 是 32 字节，留够 */
	uint32_t key_len;
	uint8_t  hmac[SHA1_DIGEST_SIZE];
	char     sign_b64[32];				/* 20 字节 → 28 个 base64 字符 + '\0' */
	char     sfs[ONENET_SFS_MAX];
	StrBuf   sb;

	if(access_key_b64 == 0 || res == 0 || out == 0 || out_size == 0) return 2;

	/* --- 1) accessKey 是 base64 的，先解成原始字节当 HMAC 密钥 --- */
	key_len = Base64_Decode(access_key_b64, (uint32_t)strlen(access_key_b64), key);
	if(key_len == 0) return 1;

	/* --- 2) 拼签名原文 --- */
	/* 顺序必须是 et / method / res / version —— 按参数名字母序排列。
	 * 这是 OneNET 的约定，和 Authorization 里出现的先后无关。
	 * 写反了不会报语法错，只会一直 10403，所以这里单独说明一次。 */
	sb.buf = sfs; sb.cap = ONENET_SFS_MAX; sb.len = 0; sfs[0] = '\0';
	if(!SB_AppendU32(&sb, et))						return 2;
	if(!SB_Append(&sb, "\n" TOK_METHOD "\n"))		return 2;
	if(!SB_Append(&sb, res))						return 2;
	if(!SB_Append(&sb, "\n" TOK_VERSION))			return 2;

	/* --- 3) HMAC-SHA1 再 base64 --- */
	HMAC_SHA1(key, key_len, (const uint8_t *)sfs, (uint32_t)sb.len, hmac);
	Base64_Encode(hmac, SHA1_DIGEST_SIZE, sign_b64);

	/* --- 4) 拼 Authorization 头，res 与 sign 都要转义 --- */
	sb.buf = out; sb.cap = out_size; sb.len = 0; out[0] = '\0';
	if(!SB_Append(&sb, "version=" TOK_VERSION "&res="))	return 2;
	if(!SB_AppendUrlEnc(&sb, res))						return 2;
	if(!SB_Append(&sb, "&et="))							return 2;
	if(!SB_AppendU32(&sb, et))							return 2;
	if(!SB_Append(&sb, "&method=" TOK_METHOD "&sign="))	return 2;
	if(!SB_AppendUrlEnc(&sb, sign_b64))					return 2;

	return 0;
}

uint8_t OneNet_BuildAuthForDevice(const char *access_key_b64,
                                  const char *pro_id,
                                  const char *dev_name,
                                  uint32_t et,
                                  char *out, uint16_t out_size)
{
	/* 实测确认：res 必须用「设备级」资源。
	 * 只写到 products/{pid} 会被拒（invalid authorization），
	 * 写成 userid/xxx 也会被拒（invalid resource）。 */
	char    res[128];
	StrBuf  sb;

	sb.buf = res; sb.cap = sizeof(res); sb.len = 0; res[0] = '\0';
	if(!SB_Append(&sb, "products/"))		return 2;
	if(!SB_Append(&sb, pro_id))				return 2;
	if(!SB_Append(&sb, "/devices/"))		return 2;
	if(!SB_Append(&sb, dev_name))			return 2;

	return OneNet_BuildAuth(access_key_b64, res, et, out, out_size);
}

/* ------------------------------------------------------------------
 * 自检
 * 用合成密钥（32 字节 0x00..0x1F 的 base64），刻意不用真实 accessKey，
 * 这样对照值可以安全进仓库（见 scripts/onenet_token_vectors.json）。
 *
 * 为什么上电要跑这个：
 *   Token 算错的表现是服务端笼统地回一句 "auth failed"，
 *   不会告诉你是 base64 错了、HMAC 错了还是转义错了。
 *   上电先自检一次，能把"签名算法"和"网络/配置问题"彻底分开。
 * ------------------------------------------------------------------ */
uint8_t OneNet_TokenSelfTest(void)
{
	static const char SYNTH_KEY[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
	static const char EXPECT[]    =
		"version=2022-05-01"
		"&res=products%2FY6O759jbtX%2Fdevices%2Fupgrade"
		"&et=1767225600"
		"&method=sha1"
		"&sign=hUq2AGdEdN8EdT5jBcq1m%2FjjzEM%3D";

	char    buf[ONENET_AUTH_MAX];
	uint8_t r;

	/* 顺带把两个基础模块也自检了 —— 它们不对，Token 一定不对，
	 * 但如果分开测，能直接看出是哪一层的问题。 */
	r = SHA1_SelfTest();
	if(r != 0) return r;					/* 1..6  SHA1/HMAC 用例编号 */

	r = Base64_SelfTest();
	if(r != 0) return 10 + r;				/* 11..19 base64 用例编号 */

	r = OneNet_BuildAuthForDevice(SYNTH_KEY, "Y6O759jbtX", "upgrade",
	                              1767225600u, buf, sizeof(buf));
	if(r != 0) return 20 + r;				/* 21..23 构造失败 */

	if(strcmp(buf, EXPECT) != 0) return 30;	/* 30  结果对不上 */

	return 0;
}
