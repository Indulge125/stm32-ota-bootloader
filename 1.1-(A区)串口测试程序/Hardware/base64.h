#ifndef __BASE64_H
#define __BASE64_H

#include "stm32f10x.h"

/* Base64 编解码 —— OneNET Token 签名要用，两个方向各有用途：
 *
 *   解码：控制台给出的 accessKey 是 base64 字符串，
 *         算 HMAC 之前必须先解成原始字节当密钥。
 *   编码：算出来的 HMAC 摘要（20 字节）要编成 base64 才能放进 Authorization 头。
 *
 * 缓冲区长度约定（调用方负责）：
 *   编码：out 至少 ((in_len + 2) / 3) * 4 + 1 字节（末位留给 '\0'）
 *         例：13KB 固件 → 17KB 输出，所以真的要对固件编码时别无脑用
 *   解码：out 至少 (in_len / 4) * 3 字节
 *
 * 编码：本文件是 UTF-8 带 BOM，保留 BOM（原因见 sha1.h 注释）。
 */

/* 编码。返回写入的字符数（不含 '\0'） */
uint16_t Base64_Encode(const uint8_t *in, uint32_t in_len, char *out);

/* 解码。返回解出的字节数；遇到非法字符返回 0。
 * 不做严格的填充校验——'=' 之后不再产出字节即视为结束。 */
uint32_t Base64_Decode(const char *in, uint32_t in_len, uint8_t *out);

/* 自检：RFC 4648 标准向量。全过返回 0，否则返回第一个失败的用例编号。 */
uint8_t Base64_SelfTest(void);

#endif
