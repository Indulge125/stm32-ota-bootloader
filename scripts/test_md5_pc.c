/* PC 端 MD5 自检入口 —— 给 scripts/test_md5.py 用 gcc 编译运行。
 *
 * 除了跑 RFC 1321 的标准向量，还支持对一个真实文件按 1024 字节分片累加，
 * **这正是 MCU 上从 OneNET 分片下载固件时要走的那条路径**。
 * 用真实固件跑一遍，能验证"分片续算"这件事本身没写错 ——
 * 一次性算对、分片算错，是很常见的一类 bug。
 *
 * 这个文件不参与 Keil 工程，只在 PC 上编译。
 */
#include <stdio.h>
#include <stdlib.h>
#include "md5.h"

int main(int argc, char **argv)
{
	uint8_t r = MD5_SelfTest();

	printf("MD5_SELFTEST=%u\n", (unsigned)r);

	if(argc > 1)
	{
		FILE   *fp = fopen(argv[1], "rb");
		MD5_CTX ctx;
		uint8_t buf[4096];
		uint8_t d[MD5_DIGEST_SIZE];
		size_t  n;
		size_t  chunk = 1024;		/* 默认和 M4 的分片大小一致 */
		int     i;
		unsigned long total = 0;
		int     chunks = 0;

		/* 分片大小可指定 —— 只跑一个值测不出"跨分片续算"的边界 bug。
		 * Python 侧会对同一个文件跑 1024 / 256 / 7 三种，结果必须完全一样。 */
		if(argc > 2)
		{
			long v = strtol(argv[2], NULL, 10);
			if(v > 0 && v <= 4096) chunk = (size_t)v;
		}

		if(fp == NULL)
		{
			printf("FILE_OPEN_FAILED=%s\n", argv[1]);
			return 1;
		}

		MD5_Init(&ctx);
		while((n = fread(buf, 1, chunk, fp)) > 0)
		{
			MD5_Update(&ctx, buf, (uint32_t)n);
			total += n;
			chunks ++;
		}
		fclose(fp);
		MD5_Final(&ctx, d);

		printf("FILE_BYTES=%lu\n", total);
		printf("FILE_CHUNKS=%d\n", chunks);
		printf("FILE_CHUNKSIZE=%u\n", (unsigned)chunk);
		printf("FILE_MD5=");
		for(i = 0; i < MD5_DIGEST_SIZE; i ++) printf("%02x", d[i]);
		printf("\n");
	}

	return (r == 0) ? 0 : 1;
}
