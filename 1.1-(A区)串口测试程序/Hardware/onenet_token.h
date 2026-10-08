#ifndef __ONENET_TOKEN_H
#define __ONENET_TOKEN_H

#include "stm32f10x.h"

/* OneNET 鉴权头（Authorization）构造 —— 每次 HTTP 请求都要带。
 *
 * 格式（已用真实设备实测通过）：
 *   Authorization: version=2022-05-01
 *                & res=products%2F{产品ID}%2Fdevices%2F{设备名}
 *                & et={过期时间戳}
 *                & method=sha1
 *                & sign={base64(HMAC-SHA1(base64decode(accessKey), sfs))}
 *
 *   sfs = et + "\n" + method + "\n" + res + "\n" + version      <- 按参数名字母序！
 *
 * 三个容易写错的地方：
 *   1. sfs 的顺序是 et / method / res / version（字母序），不是 URL 里的顺序。
 *      写反了不会报错，只会一直 10403 auth failed。
 *   2. res 里的 '/' 和 base64 里的 '+' '/' '=' 都必须 URL 转义。
 *      不转义的话，服务端解析出的 res 与参与签名的 res 就不是同一个串了。
 *   3. et 是会过期的。长期在线要重算 —— 见 OneNet_AuthExpired()。
 *
 * Flash 代价：不依赖 sprintf 家族，手写十进制转换和 URL 编码，
 * 比链进 printf 省下约 1KB。
 */

/* Authorization 字符串的长度上限（够 192 字节的 res + 44 字节签名） */
#define ONENET_AUTH_MAX		256
/* sfs（签名原文）的长度上限 */
#define ONENET_SFS_MAX		192

/* 构造 Authorization。
 *   access_key_b64 : 控制台给的那串 base64 密钥
 *   res            : 未转义的资源串，如 "products/XXX/devices/YYY"
 *   et             : 过期时间戳（秒）。传 0 表示用当前时间 + 一年
 *   out/out_size   : 输出缓冲，建议 ONENET_AUTH_MAX
 * 返回 0 成功；1 base64 解码失败；2 缓冲区不够；3 时间没初始化
 */
uint8_t OneNet_BuildAuth(const char *access_key_b64,
                         const char *res,
                         uint32_t et,
                         char *out, uint16_t out_size);

/* 便捷封装：按 产品ID + 设备名 拼 res 再调上面那个 */
uint8_t OneNet_BuildAuthForDevice(const char *access_key_b64,
                                  const char *pro_id,
                                  const char *dev_name,
                                  uint32_t et,
                                  char *out, uint16_t out_size);

/* 自检：用合成密钥跑完整 Token 对照，全过返回 0。
 * 对照值来自 scripts/onenet_token_vectors.json */
uint8_t OneNet_TokenSelfTest(void);

#endif
