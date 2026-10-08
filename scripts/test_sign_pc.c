/* PC 端自检入口 —— 给 scripts/test_sign.py 用 gcc 编译运行。
 *
 * 为什么要在 PC 上先跑：
 *   同一份 SHA1 源码，x86 上对不代表 Cortex-M3 上对（char 有无符号、
 *   移位宽度、字节序都可能不同）。先在 PC 上把算法跑绿，板子上再出问题
 *   就只剩"编译环境"和"硬件"两个方向，排查面小一半。
 *
 * 这个文件不参与 Keil 工程，只在 PC 上编译。
 */
#include <stdio.h>
#include <string.h>
#include "sha1.h"
#include "base64.h"
#include "onenet_token.h"

int main(void)
{
	uint8_t r;
	char    auth[ONENET_AUTH_MAX];
	char    buf[ONENET_AUTH_MAX];
	uint8_t i;

	/* --- 分层自检，逐层报结果，便于定位是哪一层挂了 --- */

	r = SHA1_SelfTest();
	printf("SHA1_SELFTEST=%u\n", (unsigned)r);

	r = Base64_SelfTest();
	printf("BASE64_SELFTEST=%u\n", (unsigned)r);

	r = OneNet_TokenSelfTest();
	printf("TOKEN_SELFTEST=%u\n", (unsigned)r);

	/* --- 打印合成用例的完整 Authorization，供 Python 侧逐字节对账 --- */

	r = OneNet_BuildAuthForDevice("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",
	                              "Y6O759jbtX", "upgrade",
	                              1767225600u, auth, sizeof(auth));
	if(r == 0) printf("AUTH_SYNTHETIC=%s\n", auth);
	else       printf("AUTH_SYNTHETIC=<build failed r=%u>\n", (unsigned)r);

	/* --- 再跑一个"长密钥"用例：key 超过 64 字节时 HMAC 要先摘要一次。
	 *     这条路径平时走不到，但写错了不会有人发现，专门测一下。 --- */
	{
		char longkey[128];
		memset(longkey, 'A', 100);
		longkey[100] = '\0';
		r = OneNet_BuildAuthForDevice(longkey, "PID", "DN", 1767225600u, buf, sizeof(buf));
		printf("LONGKEY_BUILD_RC=%u\n", (unsigned)r);
		if(r == 0) printf("AUTH_LONGKEY=%s\n", buf);
	}

	/* --- 边界：极小缓冲区应当被拒绝而不是溢出 --- */
	r = OneNet_BuildAuthForDevice("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",
	                              "Y6O759jbtX", "upgrade", 1767225600u, buf, 16);
	printf("TINY_BUFFER_RC=%u\n", (unsigned)r);

	/* 汇总 */
	r = SHA1_SelfTest() | Base64_SelfTest() | OneNet_TokenSelfTest();
	printf("ALL_PASS=%u\n", (unsigned)(r == 0));
	(void)i;
	return (r == 0) ? 0 : 1;
}
