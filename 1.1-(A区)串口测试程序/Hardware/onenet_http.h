#ifndef __ONENET_HTTP_H
#define __ONENET_HTTP_H

#include "stm32f10x.h"

/* ==========================================================================
 * OneNET OTA —— 设备侧 HTTP 客户端
 * ==========================================================================
 *
 * 全部 6 个 OTA 接口（上报版本 / 检测任务 / 下载固件 / 上报状态 / 检测状态 /
 * 查询版本）都走这一层。签名由 onenet_token.c 提供，网络由 4G.c 提供。
 *
 * 三个刻意的设计取舍，都写在下面，改动前先读一遍：
 *
 * ── 1) 为什么用明文 HTTP 而不是 HTTPS ──────────────────────────────────
 *   ESP8266 的 AT 固件做 TLS 很麻烦：要证书、握手慢、RAM 吃紧。
 *   实测 iot-api.heclouds.com 的 **80 端口明文可用**（不会强制跳 HTTPS），
 *   所以直接走明文 TCP，整个 TLS 栈都不需要（官方 SDK 里那块 wolfssl 更是
 *   没法塞进 64KB Flash 的 F103C8T6）。
 *   代价：数据不加密。演示/学习可接受，**真实产品必须走 HTTPS**。
 *
 * ── 2) 为什么每个请求都带 Connection: close ────────────────────────────
 *   让服务器响应完就关连接，body 的结束由"连接关闭"来界定 ——
 *   这样就**绕开了 chunked 传输编码**，否则还得再写一个 chunked 解析器。
 *   代价：每个请求都要重新建 TCP（几十毫秒），OTA 场景完全可接受。
 *
 * ── 3) 为什么 body 必须流式读，不能整体缓存 ─────────────────────────────
 *   RAM 只有 20KB，4G.c 的接收缓冲已经占了 2048，而固件有 13KB。
 *   整体缓存必爆。所以 ReadBody() 一次只交出一块，由调用者边收边处理
 *   （写 W25Q64、累加 MD5）。
 *
 * ── 编码：UTF-8 带 BOM。ARMCC5 无 BOM 时中文会被误解析并报 #870-D。
 * ========================================================================== */

/* OneNET 的 API 域名与端口。80 是明文，443 需要 TLS（本方案不用）。 */
#define ONENET_API_HOST         "iot-api.heclouds.com"
#define ONENET_API_PORT         80

/* 响应头累积缓冲。
 * 实测 OneNET 的响应头（含 Content-Disposition / Ota-Errno / Content-Range）
 * 约 400 字节；下载接口最长。768 留了接近一倍余量。
 * 真溢出会返回 HTTP_ERR_OVERFLOW 并打印已收到的原始字节，**不会静默丢字节**。 */
#define HTTP_HDR_BUF            768

/* 请求报文缓冲：方法 + path + Host + Authorization(~170) + 若干头 + body。
 * 实测最长约 330 字节，512 有余量。 */
#define HTTP_REQ_BUF            512

/* 等响应头的总超时。服务器在收到请求后通常 100ms 内响应，
 * 但首次请求要等 DNS + TCP 握手，留足。 */
#define HTTP_HDR_TIMEOUT_MS     8000

/* 返回码 */
#define HTTP_OK                 0   /* 头部解析完成。**业务成败看 resp->status 和 ota_errno** */
#define HTTP_ERR_LINK           1   /* 连不上 / 发不出（AT 或 TCP 层失败） */
#define HTTP_ERR_TIMEOUT        2   /* 等响应头超时 */
#define HTTP_ERR_OVERFLOW       3   /* 响应头超过 HTTP_HDR_BUF */
#define HTTP_ERR_FORMAT         4   /* 响应不像 HTTP，或请求报文拼装失败 */

/* 响应头里我们关心的字段 */
typedef struct
{
    uint16_t status;            /* HTTP 状态码：200 / 206 / 4xx / 5xx；0 = 没解析出来 */
    int32_t  ota_errno;         /* Ota-Errno 头；**-1 表示响应里没有这个头**。
                                 * 下载接口必须看它：文件不存在/任务过期时
                                 * HTTP 状态码可能仍是 200，只有它说实话。 */
    uint32_t content_len;       /* Content-Length；0xFFFFFFFF = 响应里没有 */
    uint32_t content_total;     /* Content-Range 里的总长度（"bytes 0-1023/13028" 的 13028）；
                                 * 0xFFFFFFFF = 没有这个头 */
}HTTP_Resp;

/* --------------------------------------------------------------------------
 * 发起一次请求，并把响应头解析完。
 *
 *   method   : "GET" / "POST"
 *   path     : 完整路径，如 "/fuse-ota/Y6O759jbtX/upgrade/version"
 *              （用 HTTP_DevPath() 拼，别手写前缀）
 *   body     : POST 的 body，可为 NULL
 *   body_len : body 长度；0 表示不发 body（GET 与无 body 的 POST 都传 0）
 *   range    : Range 头内容如 "0-1023"，可为 NULL（不给这个头）
 *   resp     : 输出，响应头解析结果
 *
 * 返回 HTTP_OK 表示"拿到了并解析完响应头"，**不代表业务成功** ——
 * 业务结果要看 resp->status（200/206/4xx/5xx）和 resp->ota_errno。
 *
 * 成功后可以反复调 HTTP_ReadBody() 取 body，取完调 HTTP_End() 收尾。
 * 任何非 HTTP_OK 的返回都**已经内部收尾**（TCP 已关、verbose 已恢复），
 * 调用者不需要再调 HTTP_End()。
 * -------------------------------------------------------------------------- */
uint8_t HTTP_Start(const char *method, const char *path,
                   const char *body, uint16_t body_len,
                   const char *range, HTTP_Resp *resp);

/* --------------------------------------------------------------------------
 * 流式取 body。返回本次交出的字节数；返回 0 表示"暂时没有"。
 *
 * ⚠️ 0 有歧义：可能是"还没到"，也可能是"已经收完了"。用 Connection: close
 *    时没有可靠的分帧信号，所以**由调用者按已知的长度来判结束** ——
 *    长度来自 /check 返回的 size，或 resp->content_len。
 *    调用者循环：收够长度就停，或超时放弃。
 * -------------------------------------------------------------------------- */
uint16_t HTTP_ReadBody(uint8_t *buf, uint16_t max);

/* 收尾：关 TCP、恢复 4G.c 的 verbose 开关。可重复调用。 */
void HTTP_End(void);

/* 拼设备级路径：/fuse-ota/{产品ID}/{设备名}/{tail}
 *   tail 例："version"、"check?type=2&version=1.0.0"、"1516460/download"
 * 返回 0 成功、2 缓冲区不够。产品ID/设备名从 onenet_cfg.h 取。 */
uint8_t HTTP_DevPath(char *out, uint16_t cap, const char *tail);

#endif
