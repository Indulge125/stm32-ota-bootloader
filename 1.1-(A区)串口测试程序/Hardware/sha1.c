#include "sha1.h"
#include <string.h>		/* strlen：只给下面的自检用 */

/* 循环左移。宏要加括号：一是优先级，二是避免实参是表达式时被求值多次。 */
#define SHA1_ROL(x, n)		( ((x) << (n)) | ((x) >> (32 - (n))) )

/* 一个 512 位块的压缩函数：80 轮。w[80] 占 320 字节栈空间，
 * 这是本模块最大的栈开销，调用它的任务栈要留够。 */
static void SHA1_Transform(uint32_t state[5], const uint8_t block[SHA1_BLOCK_SIZE])
{
	uint32_t w[80];
	uint32_t a, b, c, d, e, t;
	uint8_t i;

	/* 前 16 个字：大端拼装 */
	for(i = 0; i < 16; i ++)
	{
		w[i] = ((uint32_t)block[i*4 + 0] << 24)
		     | ((uint32_t)block[i*4 + 1] << 16)
		     | ((uint32_t)block[i*4 + 2] <<  8)
		     | ((uint32_t)block[i*4 + 3]);
	}
	/* 后 64 个字：由前 16 个字递推 */
	for(i = 16; i < 80; i ++)
	{
		w[i] = SHA1_ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	}

	a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

	for(i = 0; i < 80; i ++)
	{
		uint32_t f, k;
		if(i < 20)		{ f = (b & c) | ((~b) & d);			k = 0x5A827999; }
		else if(i < 40)	{ f = b ^ c ^ d;					k = 0x6ED9EBA1; }
		else if(i < 60)	{ f = (b & c) | (b & d) | (c & d);	k = 0x8F1BBCDC; }
		else			{ f = b ^ c ^ d;					k = 0xCA62C1D6; }

		t = SHA1_ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void SHA1_Init(SHA1_CTX *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count    = 0;
}

void SHA1_Update(SHA1_CTX *ctx, const uint8_t *data, uint32_t len)
{
	/* idx 用 count 反推，而不是单独存一个成员 —— 少一个状态就少一处可能不同步 */
	uint32_t idx = ctx->count % SHA1_BLOCK_SIZE;

	ctx->count += len;

	while(len --)
	{
		ctx->buffer[idx ++] = *data ++;
		if(idx == SHA1_BLOCK_SIZE)
		{
			SHA1_Transform(ctx->state, ctx->buffer);
			idx = 0;
		}
	}
}

void SHA1_Final(SHA1_CTX *ctx, uint8_t digest[SHA1_DIGEST_SIZE])
{
	uint32_t bits = ctx->count << 3;		/* 比特数，必须在补位之前算 */
	uint8_t  pad  = 0x80;					/* 先补一个 1，再补 0 */
	uint8_t  zero = 0x00;
	uint8_t  lenb[8];
	uint8_t  i;

	SHA1_Update(ctx, &pad, 1);

	/* 补 0 到离 64 字节边界还剩 8 字节（留给长度字段） */
	while((ctx->count % SHA1_BLOCK_SIZE) != 56)
	{
		SHA1_Update(ctx, &zero, 1);
	}

	/* 长度按大端写 8 字节。本实现只支持 4GB 以内的输入，
	 * 高 4 字节恒为 0 —— 固件才 13KB，够用。 */
	for(i = 0; i < 4; i ++) lenb[i]     = 0;
	lenb[4] = (uint8_t)(bits >> 24);
	lenb[5] = (uint8_t)(bits >> 16);
	lenb[6] = (uint8_t)(bits >>  8);
	lenb[7] = (uint8_t)(bits);

	SHA1_Update(ctx, lenb, 8);

	/* 输出大端 */
	for(i = 0; i < 5; i ++)
	{
		digest[i*4 + 0] = (uint8_t)(ctx->state[i] >> 24);
		digest[i*4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i*4 + 2] = (uint8_t)(ctx->state[i] >>  8);
		digest[i*4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

void HMAC_SHA1(const uint8_t *key, uint32_t key_len,
               const uint8_t *data, uint32_t data_len,
               uint8_t out[SHA1_DIGEST_SIZE])
{
	uint8_t  k[SHA1_BLOCK_SIZE];
	uint8_t  ipad[SHA1_BLOCK_SIZE];
	uint8_t  opad[SHA1_BLOCK_SIZE];
	uint8_t  inner[SHA1_DIGEST_SIZE];
	SHA1_CTX ctx;
	uint16_t i;

	/* key 先规整到 64 字节：太长就先摘要一次，太短就补 0 */
	for(i = 0; i < SHA1_BLOCK_SIZE; i ++) k[i] = 0;
	if(key_len > SHA1_BLOCK_SIZE)
	{
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, key, key_len);
		SHA1_Final(&ctx, k);
	}
	else
	{
		for(i = 0; i < key_len; i ++) k[i] = key[i];
	}

	for(i = 0; i < SHA1_BLOCK_SIZE; i ++)
	{
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5C;
	}

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, ipad, SHA1_BLOCK_SIZE);
	SHA1_Update(&ctx, data, data_len);
	SHA1_Final(&ctx, inner);

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, opad, SHA1_BLOCK_SIZE);
	SHA1_Update(&ctx, inner, SHA1_DIGEST_SIZE);
	SHA1_Final(&ctx, out);
}

/* ------------------------------------------------------------------
 * 自检
 * 向量来源：SHA1 用 FIPS 180-1 常见测试向量，HMAC 用 RFC 2202。
 * 和 scripts/onenet_token_vectors.json 里的值是同一批，两端对账用。
 *
 * 为什么要把向量放进固件而不是只在 PC 上测：
 *   同一个 SHA1 源码，在 x86 上对、在 Cortex-M3 上不一定对
 *   （char 有无符号、移位宽度、对齐都可能有差异）。
 *   上电跑一遍自检，能把"编译环境差异"和"算法写错"分开。
 * ------------------------------------------------------------------ */

/* 把 20 字节摘要和 40 个字符的十六进制串比较，省掉一个十六进制解析器 */
static uint8_t SHA1_HexEqual(const uint8_t *digest, const char *hex)
{
	uint8_t i, hi, lo, b;

	for(i = 0; i < SHA1_DIGEST_SIZE; i ++)
	{
		hi = (uint8_t)hex[i*2 + 0];
		lo = (uint8_t)hex[i*2 + 1];
		hi = (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
		lo = (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
		b  = (uint8_t)((hi << 4) | lo);
		if(b != digest[i]) return 0;
	}
	return 1;
}

static uint8_t SHA1_RunCase(const char *msg, const char *expect_hex)
{
	SHA1_CTX ctx;
	uint8_t  d[SHA1_DIGEST_SIZE];

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, (const uint8_t *)msg, (uint32_t)strlen(msg));
	SHA1_Final(&ctx, d);

	return SHA1_HexEqual(d, expect_hex);
}

static uint8_t SHA1_RunHmacCase(const uint8_t *key, uint32_t key_len,
                                const char *msg, const char *expect_hex)
{
	uint8_t d[SHA1_DIGEST_SIZE];
	HMAC_SHA1(key, key_len, (const uint8_t *)msg, (uint32_t)strlen(msg), d);
	return SHA1_HexEqual(d, expect_hex);
}

uint8_t SHA1_SelfTest(void)
{
	uint8_t k20[20];
	uint8_t i;
	uint8_t n = 0;

	/* --- SHA1 本体 --- */
	n ++; if(!SHA1_RunCase("", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))		return n;
	n ++; if(!SHA1_RunCase("abc", "a9993e364706816aba3e25717850c26c9cd0d89d"))	return n;
	n ++; if(!SHA1_RunCase("123456789", "f7c3bc1d808e04732adf679965ccc34ca7ae3441")) return n;

	/* --- HMAC-SHA1 (RFC 2202) --- */
	for(i = 0; i < 20; i ++) k20[i] = 0x0B;		/* key = 20 个 0x0b */
	n ++; if(!SHA1_RunHmacCase(k20, 20, "Hi There",
			"b617318655057264e28bc0b6fb378c8ef146be00"))						return n;

	n ++; if(!SHA1_RunHmacCase((const uint8_t *)"Jefe", 4, "what do ya want for nothing?",
			"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"))						return n;

	n ++; if(!SHA1_RunHmacCase((const uint8_t *)"key", 3,
			"The quick brown fox jumps over the lazy dog",
			"de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9"))						return n;

	return 0;
}
