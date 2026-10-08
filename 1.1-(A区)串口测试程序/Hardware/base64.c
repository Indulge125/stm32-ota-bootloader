#include "base64.h"
#include <string.h>		/* strcmp：只给下面的自检用 */

static const char B64_TAB[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 字符 → 6 位值。非法字符返回 -1（用 int8_t 才能表达"负"） */
static int8_t Base64_Val(char c)
{
	if(c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
	if(c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
	if(c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
	if(c == '+')             return 62;
	if(c == '/')             return 63;
	return -1;
}

uint16_t Base64_Encode(const uint8_t *in, uint32_t in_len, char *out)
{
	uint32_t i = 0;
	uint16_t n = 0;

	/* 每 3 字节一批。循环条件用 i+2 < in_len，
	 * 这样只剩 1 或 2 字节时自然落到下面的补 '=' 分支 */
	while((i + 2) < in_len)
	{
		out[n ++] = B64_TAB[ in[i] >> 2 ];
		out[n ++] = B64_TAB[ ((in[i] & 0x03) << 4) | (in[i+1] >> 4) ];
		out[n ++] = B64_TAB[ ((in[i+1] & 0x0F) << 2) | (in[i+2] >> 6) ];
		out[n ++] = B64_TAB[ in[i+2] & 0x3F ];
		i += 3;
	}

	if((in_len - i) == 1)			/* 剩 1 字节 → 2 个字符 + "==" */
	{
		out[n ++] = B64_TAB[ in[i] >> 2 ];
		out[n ++] = B64_TAB[ (in[i] & 0x03) << 4 ];
		out[n ++] = '=';
		out[n ++] = '=';
	}
	else if((in_len - i) == 2)		/* 剩 2 字节 → 3 个字符 + "=" */
	{
		out[n ++] = B64_TAB[ in[i] >> 2 ];
		out[n ++] = B64_TAB[ ((in[i] & 0x03) << 4) | (in[i+1] >> 4) ];
		out[n ++] = B64_TAB[ (in[i+1] & 0x0F) << 2 ];
		out[n ++] = '=';
	}

	out[n] = '\0';
	return n;
}

uint32_t Base64_Decode(const char *in, uint32_t in_len, uint8_t *out)
{
	uint32_t i = 0, n = 0;
	int8_t v0, v1, v2, v3;

	while((i + 3) < in_len)
	{
		v0 = Base64_Val(in[i]);
		v1 = Base64_Val(in[i + 1]);
		if(v0 < 0 || v1 < 0) return 0;				/* 前两个字符必须合法 */

		out[n ++] = (uint8_t)((v0 << 2) | (v1 >> 4));

		v2 = Base64_Val(in[i + 2]);					/* 可能是 '=' */
		if(v2 < 0) { i += 4; continue; }

		out[n ++] = (uint8_t)(((v1 & 0x0F) << 4) | (v2 >> 2));

		v3 = Base64_Val(in[i + 3]);					/* 可能是 '=' */
		if(v3 < 0) { i += 4; continue; }

		out[n ++] = (uint8_t)(((v2 & 0x03) << 6) | v3);

		i += 4;
	}
	return n;
}

/* ------------------------------------------------------------------
 * 自检
 * 向量来源：RFC 4648 §10 的标准测试向量（"f"/"fo"/"foo"/"foob"/"fooba"/"foobar"）。
 * 和 scripts/onenet_token_vectors.json 里的 base64 部分对账。
 * ------------------------------------------------------------------ */

static uint8_t Base64_EncCase(const char *plain, const char *expect)
{
	char buf[32];
	Base64_Encode((const uint8_t *)plain, (uint32_t)strlen(plain), buf);
	return (strcmp(buf, expect) == 0) ? 1 : 0;
}

static uint8_t Base64_DecCase(const char *b64, const char *expect)
{
	uint8_t buf[32];
	uint32_t n;
	memset(buf, 0, sizeof(buf));
	n = Base64_Decode(b64, (uint32_t)strlen(b64), buf);
	if(n != strlen(expect)) return 0;
	return (memcmp(buf, expect, n) == 0) ? 1 : 0;
}

uint8_t Base64_SelfTest(void)
{
	uint8_t n = 0;

	n ++; if(!Base64_EncCase("f",      "Zg=="))			return n;
	n ++; if(!Base64_EncCase("fo",     "Zm8="))			return n;
	n ++; if(!Base64_EncCase("foo",    "Zm9v"))			return n;
	n ++; if(!Base64_EncCase("foob",   "Zm9vYg=="))		return n;
	n ++; if(!Base64_EncCase("fooba",  "Zm9vYmE="))		return n;
	n ++; if(!Base64_EncCase("foobar", "Zm9vYmFy"))		return n;

	n ++; if(!Base64_DecCase("Zg==",     "f"))			return n;
	n ++; if(!Base64_DecCase("Zm9v",     "foo"))		return n;
	n ++; if(!Base64_DecCase("Zm9vYmFy", "foobar"))		return n;

	return 0;
}
