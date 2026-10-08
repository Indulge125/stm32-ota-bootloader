#ifndef __SHA1_H
#define __SHA1_H

#include "stm32f10x.h"

/* SHA1 / HMAC-SHA1 —— OneNET 的 Authorization 签名要用。
 *
 * 为什么会出现在 MCU 上：
 *   OneNET 设备侧 API 的鉴权头是
 *     sign = base64( HMAC-SHA1( base64decode(accessKey), sfs ) )
 *     sfs  = et + "\n" + method + "\n" + res + "\n" + version
 *   这个签名必须在设备本地算（accessKey 不下发），所以 MCU 得自带 SHA1。
 *
 * 为什么写成流式（Init/Update/Final）而不是一次性：
 *   sfs 是三段拼出来的，长度不固定；而且以后如果要对分片下载的固件算摘要，
 *   分片是逐块来的，只有流式才能续算。一次性接口做不到。
 *
 * 编码：本文件是 UTF-8 带 BOM。ARMCC5 默认按系统 ANSI 代码页解码源文件，
 *   无 BOM 时中文字符串字面量会被误解析并报 #870-D，务必保留 BOM。
 */

#define SHA1_DIGEST_SIZE   20
#define SHA1_BLOCK_SIZE    64

typedef struct
{
	uint32_t state[5];                  /* 中间状态 a~e */
	uint32_t count;                     /* 已喂入的字节数（够用：固件远小于 4GB） */
	uint8_t  buffer[SHA1_BLOCK_SIZE];   /* 未满一块的残留数据 */
}SHA1_CTX;

void SHA1_Init(SHA1_CTX *ctx);
void SHA1_Update(SHA1_CTX *ctx, const uint8_t *data, uint32_t len);
void SHA1_Final(SHA1_CTX *ctx, uint8_t digest[SHA1_DIGEST_SIZE]);

/* HMAC-SHA1：key 超过 64 字节时先做一次 SHA1 压缩，这是 RFC 2104 的规定 */
void HMAC_SHA1(const uint8_t *key, uint32_t key_len,
               const uint8_t *data, uint32_t data_len,
               uint8_t out[SHA1_DIGEST_SIZE]);

/* 自检：跑已知向量，全过返回 0，否则返回第一个失败的用例编号（从 1 起）。
 * 上电时可调用；PC 端 gcc 测试也复用它，保证两端跑的是同一份判定逻辑。 */
uint8_t SHA1_SelfTest(void);

#endif
