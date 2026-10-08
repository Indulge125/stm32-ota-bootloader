#ifndef __MD5_H
#define __MD5_H

#include "stm32f10x.h"

/* MD5 —— 校验从 OneNET 下载回来的固件完整性。
 *
 * 为什么是 MD5 而不是复用 BootLoader 的 CRC16：
 *   平台在 /check 的响应里给的是 **md5** 字段（不是 CRC）。
 *   要跟云端给的值对上，就只能算 MD5。自己另算 CRC 没有意义 ——
 *   那样等于放弃了"和云端对账"这个校验点。
 *
 * 为什么写成流式（Init/Update/Final）：
 *   固件是分片下载的（每片 1024 字节），必须边收边算。
 *   等收全了再算需要一整块 13KB 缓冲，RAM 不够。
 *
 * 编码：UTF-8 带 BOM，保留（原因见 sha1.h）。
 */

#define MD5_DIGEST_SIZE		16
#define MD5_BLOCK_SIZE		64

typedef struct
{
	uint32_t state[4];					/* A / B / C / D */
	uint32_t count;						/* 已喂入的字节数（固件远小于 4GB） */
	uint8_t  buffer[MD5_BLOCK_SIZE];	/* 未满一块的残留 */
}MD5_CTX;

void MD5_Init(MD5_CTX *ctx);
void MD5_Update(MD5_CTX *ctx, const uint8_t *data, uint32_t len);
void MD5_Final(MD5_CTX *ctx, uint8_t digest[MD5_DIGEST_SIZE]);

/* 自检：跑 RFC 1321 的已知向量，全过返回 0，否则返回第一个失败的用例编号。
 * 上电可调用；PC 侧 gcc 测试复用同一份判定逻辑。 */
uint8_t MD5_SelfTest(void);

#endif
