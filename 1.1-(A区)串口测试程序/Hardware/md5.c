#include "md5.h"
#include <string.h>		/* strlen：只给下面的自检用 */

/* 循环左移。宏要加括号：一是优先级，二是避免实参是表达式时被求值多次。 */
#define MD5_ROL(x, n)		( ((x) << (n)) | ((x) >> (32 - (n))) )

/* RFC 1321 里四个轮函数。写成位运算形式（而不是照抄教科书的分支写法）
 * 是为了让编译器能直接用位指令，省几个周期 —— 13KB 固件要过 200 多块。 */
#define MD5_F(x, y, z)		( (z) ^ ((x) & ((y) ^ (z))) )
#define MD5_G(x, y, z)		( (y) ^ ((z) & ((x) ^ (y))) )
#define MD5_H(x, y, z)		( (x) ^ (y) ^ (z) )
#define MD5_I(x, y, z)		( (y) ^ ((x) | (~(z))) )

/* 一趟。用 do{}while(0) 包起来，保证它是一条语句 ——
 * 直接写成多语句宏会在 if/else 那种没有花括号的地方出问题。 */
#define MD5_STEP(f, a, b, c, d, x, t, s)	\
	do {									\
		(a) += f((b), (c), (d)) + (x) + (t);\
		(a) = MD5_ROL((a), (s));		\
		(a) += (b);							\
	} while(0)

static void MD5_Transform(uint32_t state[4], const uint8_t block[MD5_BLOCK_SIZE])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t x[16];
	uint8_t  i;

	/* ⚠️ MD5 是**小端**的，SHA1 是大端 —— 这里写反了摘要全错，
	 * 而且自检向量会立刻发现（所以那几个向量必须留着）。 */
	for(i = 0; i < 16; i ++)
	{
		x[i] = (uint32_t)block[i*4 + 0]
		     | ((uint32_t)block[i*4 + 1] <<  8)
		     | ((uint32_t)block[i*4 + 2] << 16)
		     | ((uint32_t)block[i*4 + 3] << 24);
	}

	/* Round 1 */
	MD5_STEP(MD5_F, a,b,c,d, x[ 0], 0xd76aa478,  7);
	MD5_STEP(MD5_F, d,a,b,c, x[ 1], 0xe8c7b756, 12);
	MD5_STEP(MD5_F, c,d,a,b, x[ 2], 0x242070db, 17);
	MD5_STEP(MD5_F, b,c,d,a, x[ 3], 0xc1bdceee, 22);
	MD5_STEP(MD5_F, a,b,c,d, x[ 4], 0xf57c0faf,  7);
	MD5_STEP(MD5_F, d,a,b,c, x[ 5], 0x4787c62a, 12);
	MD5_STEP(MD5_F, c,d,a,b, x[ 6], 0xa8304613, 17);
	MD5_STEP(MD5_F, b,c,d,a, x[ 7], 0xfd469501, 22);
	MD5_STEP(MD5_F, a,b,c,d, x[ 8], 0x698098d8,  7);
	MD5_STEP(MD5_F, d,a,b,c, x[ 9], 0x8b44f7af, 12);
	MD5_STEP(MD5_F, c,d,a,b, x[10], 0xffff5bb1, 17);
	MD5_STEP(MD5_F, b,c,d,a, x[11], 0x895cd7be, 22);
	MD5_STEP(MD5_F, a,b,c,d, x[12], 0x6b901122,  7);
	MD5_STEP(MD5_F, d,a,b,c, x[13], 0xfd987193, 12);
	MD5_STEP(MD5_F, c,d,a,b, x[14], 0xa679438e, 17);
	MD5_STEP(MD5_F, b,c,d,a, x[15], 0x49b40821, 22);

	/* Round 2 */
	MD5_STEP(MD5_G, a,b,c,d, x[ 1], 0xf61e2562,  5);
	MD5_STEP(MD5_G, d,a,b,c, x[ 6], 0xc040b340,  9);
	MD5_STEP(MD5_G, c,d,a,b, x[11], 0x265e5a51, 14);
	MD5_STEP(MD5_G, b,c,d,a, x[ 0], 0xe9b6c7aa, 20);
	MD5_STEP(MD5_G, a,b,c,d, x[ 5], 0xd62f105d,  5);
	MD5_STEP(MD5_G, d,a,b,c, x[10], 0x02441453,  9);
	MD5_STEP(MD5_G, c,d,a,b, x[15], 0xd8a1e681, 14);
	MD5_STEP(MD5_G, b,c,d,a, x[ 4], 0xe7d3fbc8, 20);
	MD5_STEP(MD5_G, a,b,c,d, x[ 9], 0x21e1cde6,  5);
	MD5_STEP(MD5_G, d,a,b,c, x[14], 0xc33707d6,  9);
	MD5_STEP(MD5_G, c,d,a,b, x[ 3], 0xf4d50d87, 14);
	MD5_STEP(MD5_G, b,c,d,a, x[ 8], 0x455a14ed, 20);
	MD5_STEP(MD5_G, a,b,c,d, x[13], 0xa9e3e905,  5);
	MD5_STEP(MD5_G, d,a,b,c, x[ 2], 0xfcefa3f8,  9);
	MD5_STEP(MD5_G, c,d,a,b, x[ 7], 0x676f02d9, 14);
	MD5_STEP(MD5_G, b,c,d,a, x[12], 0x8d2a4c8a, 20);

	/* Round 3 */
	MD5_STEP(MD5_H, a,b,c,d, x[ 5], 0xfffa3942,  4);
	MD5_STEP(MD5_H, d,a,b,c, x[ 8], 0x8771f681, 11);
	MD5_STEP(MD5_H, c,d,a,b, x[11], 0x6d9d6122, 16);
	MD5_STEP(MD5_H, b,c,d,a, x[14], 0xfde5380c, 23);
	MD5_STEP(MD5_H, a,b,c,d, x[ 1], 0xa4beea44,  4);
	MD5_STEP(MD5_H, d,a,b,c, x[ 4], 0x4bdecfa9, 11);
	MD5_STEP(MD5_H, c,d,a,b, x[ 7], 0xf6bb4b60, 16);
	MD5_STEP(MD5_H, b,c,d,a, x[10], 0xbebfbc70, 23);
	MD5_STEP(MD5_H, a,b,c,d, x[13], 0x289b7ec6,  4);
	MD5_STEP(MD5_H, d,a,b,c, x[ 0], 0xeaa127fa, 11);
	MD5_STEP(MD5_H, c,d,a,b, x[ 3], 0xd4ef3085, 16);
	MD5_STEP(MD5_H, b,c,d,a, x[ 6], 0x04881d05, 23);
	MD5_STEP(MD5_H, a,b,c,d, x[ 9], 0xd9d4d039,  4);
	MD5_STEP(MD5_H, d,a,b,c, x[12], 0xe6db99e5, 11);
	MD5_STEP(MD5_H, c,d,a,b, x[15], 0x1fa27cf8, 16);
	MD5_STEP(MD5_H, b,c,d,a, x[ 2], 0xc4ac5665, 23);

	/* Round 4 */
	MD5_STEP(MD5_I, a,b,c,d, x[ 0], 0xf4292244,  6);
	MD5_STEP(MD5_I, d,a,b,c, x[ 7], 0x432aff97, 10);
	MD5_STEP(MD5_I, c,d,a,b, x[14], 0xab9423a7, 15);
	MD5_STEP(MD5_I, b,c,d,a, x[ 5], 0xfc93a039, 21);
	MD5_STEP(MD5_I, a,b,c,d, x[12], 0x655b59c3,  6);
	MD5_STEP(MD5_I, d,a,b,c, x[ 3], 0x8f0ccc92, 10);
	MD5_STEP(MD5_I, c,d,a,b, x[10], 0xffeff47d, 15);
	MD5_STEP(MD5_I, b,c,d,a, x[ 1], 0x85845dd1, 21);
	MD5_STEP(MD5_I, a,b,c,d, x[ 8], 0x6fa87e4f,  6);
	MD5_STEP(MD5_I, d,a,b,c, x[15], 0xfe2ce6e0, 10);
	MD5_STEP(MD5_I, c,d,a,b, x[ 6], 0xa3014314, 15);
	MD5_STEP(MD5_I, b,c,d,a, x[13], 0x4e0811a1, 21);
	MD5_STEP(MD5_I, a,b,c,d, x[ 4], 0xf7537e82,  6);
	MD5_STEP(MD5_I, d,a,b,c, x[11], 0xbd3af235, 10);
	MD5_STEP(MD5_I, c,d,a,b, x[ 2], 0x2ad7d2bb, 15);
	MD5_STEP(MD5_I, b,c,d,a, x[ 9], 0xeb86d391, 21);

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void MD5_Init(MD5_CTX *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
	ctx->count    = 0;
}

void MD5_Update(MD5_CTX *ctx, const uint8_t *data, uint32_t len)
{
	/* idx 由 count 反推，少存一个状态就少一处可能不同步的地方 */
	uint32_t idx = ctx->count % MD5_BLOCK_SIZE;

	ctx->count += len;

	while(len --)
	{
		ctx->buffer[idx ++] = *data ++;
		if(idx == MD5_BLOCK_SIZE)
		{
			MD5_Transform(ctx->state, ctx->buffer);
			idx = 0;
		}
	}
}

void MD5_Final(MD5_CTX *ctx, uint8_t digest[MD5_DIGEST_SIZE])
{
	uint32_t bits = ctx->count << 3;		/* 比特数，必须在补位之前算 */
	uint8_t  pad  = 0x80;
	uint8_t  zero = 0x00;
	uint8_t  lenb[8];
	uint8_t  i;

	MD5_Update(ctx, &pad, 1);

	/* 补 0 到离 64 字节边界还剩 8 字节（留给长度字段） */
	while((ctx->count % MD5_BLOCK_SIZE) != 56)
	{
		MD5_Update(ctx, &zero, 1);
	}

	/* ⚠️ 长度也是**小端**的 —— 和 SHA1 的大端相反。
	 * 这里写反了，短输入会碰巧对（长度小，字节都在低位），
	 * 长输入才错位暴露 —— 所以自检向量里必须有一条 80 字节的长输入。 */
	lenb[0] = (uint8_t)(bits);
	lenb[1] = (uint8_t)(bits >>  8);
	lenb[2] = (uint8_t)(bits >> 16);
	lenb[3] = (uint8_t)(bits >> 24);
	for(i = 4; i < 8; i ++) lenb[i] = 0;	/* 本实现只支持 4GB 以内输入 */

	MD5_Update(ctx, lenb, 8);

	/* 摘要也是小端输出 */
	for(i = 0; i < 4; i ++)
	{
		digest[i*4 + 0] = (uint8_t)(ctx->state[i]);
		digest[i*4 + 1] = (uint8_t)(ctx->state[i] >>  8);
		digest[i*4 + 2] = (uint8_t)(ctx->state[i] >> 16);
		digest[i*4 + 3] = (uint8_t)(ctx->state[i] >> 24);
	}
}

/* ------------------------------------------------------------------
 * 自检
 * 向量来源：RFC 1321 附录 A.5 的标准测试套件。
 * 第 4 条（80 字节长输入）是专门留的 —— 如果长度字段的字节序写反了，
 * 只有足够长的输入才会暴露，短输入会碰巧通过。
 * ------------------------------------------------------------------ */

static uint8_t MD5_HexEqual(const uint8_t *digest, const char *hex)
{
	uint8_t i, hi, lo, b;

	for(i = 0; i < MD5_DIGEST_SIZE; i ++)
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

static uint8_t MD5_RunCase(const char *msg, const char *expect_hex)
{
	MD5_CTX ctx;
	uint8_t d[MD5_DIGEST_SIZE];

	MD5_Init(&ctx);
	MD5_Update(&ctx, (const uint8_t *)msg, (uint32_t)strlen(msg));
	MD5_Final(&ctx, d);

	return MD5_HexEqual(d, expect_hex);
}

uint8_t MD5_SelfTest(void)
{
	uint8_t n = 0;

	n ++; if(!MD5_RunCase("", "d41d8cd98f00b204e9800998ecf8427e"))					return n;
	n ++; if(!MD5_RunCase("a", "0cc175b9c0f1b6a831c399e269772661"))					return n;
	n ++; if(!MD5_RunCase("abc", "900150983cd24fb0d6963f7d28e17f72"))				return n;
	n ++; if(!MD5_RunCase("message digest", "f96b697d7cb7938d525a2f31aaf161d0"))		return n;
	n ++; if(!MD5_RunCase("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b")) return n;

	/* 80 字节长输入 —— 字节序写反时只有这条会挂 */
	n ++; if(!MD5_RunCase(
			"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
			"57edf4a22be3c955ac49da2e2107b67a"))									return n;

	return 0;
}
