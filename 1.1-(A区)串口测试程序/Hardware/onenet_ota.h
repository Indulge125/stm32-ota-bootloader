#ifndef __ONENET_OTA_H
#define __ONENET_OTA_H

#include "stm32f10x.h"

/* --------------------------------------------------------------------------
 * 应用版本号 —— **由构建配置注入，不要改这里的源码**
 * --------------------------------------------------------------------------
 * 这个字符串是平台判定"要不要升级"的唯一依据：
 *   POST /version 上报它，GET /check 拿它当 version 参数，
 *   平台还拿它和升级包的「目标版本」逐字比对。
 * 所以它必须和 OneNET 控制台上填的「目标版本」**逐字一致** ——
 * "1.1.0" 和 "v1.1.0" 是两个不同的版本。写歪的表现是"平台永远不下发任务"：
 * 不报错，只是静默地什么都不发生，最难查。
 *
 * ── 怎么切换版本：注入**纯数字代号** ────────────────────────────────────
 * 不要再手改这个文件。同一个工程要编出两个版本：
 *   1.0.0 —— 烧进设备，当"待升级的旧版本"
 *   1.1.0 —— 传到 OneNET，当"升级包"
 * 改源码就意味着一遍遍"改了编、编完改回来"，容易漏，也会让源码被
 * 临时改动污染。**版本号是构建参数，不是源码内容** —— 这是工程化的基本分工。
 *
 * Keil 里的做法（一个工程两个 Target）：
 *   Target 下拉框（Build 按钮右边）→ Manage Project Items → New(Copy)
 *     建 A-1.0.0 和 A-1.1.0 两个 Target
 *   对每个 Target：Options for Target → C/C++ → Define 填
 *       USE_STDPERIPH_DRIVER,APP_VERSION_ID=100
 *       USE_STDPERIPH_DRIVER,APP_VERSION_ID=110
 *   建议再给两个 Target 设**不同的输出目录**（Options → Output →
 *     Select Folder for Objects），否则 Rebuild 会互相覆盖 .bin。
 *   之后：下拉框切 Target → Rebuild → 得到对应版本的 .bin。
 *
 * 命令行同理：armcc -DAPP_VERSION_ID=110 ...
 * 代号用「主*100 + 次*10 + 修」这种好认的形式：100 = 1.0.0，110 = 1.1.0。
 *
 * ⚠️ 为什么注入的是数字而不是字符串（本想直接写 APP_VERSION="1.1.0" 更直观）：
 *   实测走不通，是**两个叠加的 Windows 老坑**——
 *     ① Keil 生成 -DAPP_VERSION="1.1.0"，但 Windows 的参数解析把引号当
 *        分组符**剥掉**，armcc 实际收到 -DAPP_VERSION=1.1.0。于是宏被定义成
 *        记号 1.1.0，编译器把它当 double，源码里 "A-OTA v" APP_VERSION 直接
 *        散架 —— 报一堆 `#18: expected a ")"` / `#167: argument of type "double"`。
 *     ② armcc 的 -D **不接受逗号分隔多个宏**（报 `#992: invalid macro
 *        definition`），而 Keil 的 Define 框恰恰是用逗号分隔多个符号的。
 *   改成纯数字，这两个坑一起绕开。
 *
 * 加新版本只需在下面加一个 #elif —— **版本号的权威清单留在这一个头文件里**，
 * 构建只负责"选"其中一个。这比"每次都从命令行传全字符串"好维护，
 * 也更接近生产环境里 version.h 的做法。
 *
 * 兜底值取 100（1.0.0）：没传代号时行为保持不变、不会静默刷成错版本。
 * 传了不认识的值会直接 #error 编译失败 —— 那也比静默编错版本强。 */
#ifndef APP_VERSION_ID
#define APP_VERSION_ID	100
#endif

#if   APP_VERSION_ID == 100
#define APP_VERSION		"1.0.0"
#elif APP_VERSION_ID == 110
#define APP_VERSION		"1.1.0"
#else
#error "未知的 APP_VERSION_ID —— 请在这段 #if 里加一个分支，并检查 Keil 的 Define 设置"
#endif

/* ==========================================================================
 * OneNET OTA 流程编排
 * ==========================================================================
 *
 * 把 【检测任务 → 分片下载 → 校验 → 置标志 → 复位】 串成一次调用。
 * 底下两层：onenet_http.c 负责 HTTP，onenet_token.c 负责签名，
 * 再下面 4G.c 负责 ESP8266、W25Q64.c 负责外部 Flash、m24c02.c 负责掉电标志。
 *
 * 为什么 A 区只负责"下载 + 置标志"，搬运交给 B 区 BootLoader：
 *   A 区正在运行自己的代码，没法擦掉自己脚下的 Flash 再写新的（写到一半就崩了）。
 *   所以 A 区只把新固件放进 W25Q64、打个标志、复位；
 *   由 BootLoader 在"没人运行 A 区"的时机去擦写和跳转。
 *   这也是为什么 ota_layout.h 必须是两个工程共享的契约。
 *
 * 升级成功的确认不需要设备做任何特殊动作：
 *   新固件起来后照常走 OTA_Run()，第一步 POST /version 就会把新版本号告诉平台，
 *   /check 随即返回 12013 "task succ"，平台自动把任务标记为完成。
 *   （实测确认，见 git 提交 b99792a 的说明。）
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 可选的运行状态回调
 * --------------------------------------------------------------------------
 * OTA 流程会经过"查任务 → 下载 → 校验 → 置标志"几个阶段，每个阶段要好几秒。
 * 串口日志够详细，但现场调试时**抬头看 OLED 比翻串口方便**。
 *
 * 用回调而不是让 onenet_ota.c 直接调 OLED，是为了**不把网络模块和显示模块
 * 绑死** —— 这个模块以后可能被别的工程复用，那边可能没有 OLED，
 * 或者要显示到别的地方。不注册回调就只走串口日志，一切照旧。
 *
 *   line : 建议显示在第几行（1~4）
 *   text : ASCII 短字符串 —— OLED 用 8x16 字库，只有 ASCII，别塞中文
 *
 * 回调里只做显示，别放耗时操作（它跑在 OTA 主流程里）。 */
typedef void (*OTA_StatusCb)(uint8_t line, const char *text);
void OTA_SetStatusCb(OTA_StatusCb cb);

/* 返回码。失败码按"卡在哪一层"区分，方便一看就知道往哪儿查。 */
#define OTA_R_OK            0   /* 无待办：没有任务，或设备已经是目标版本 */
#define OTA_R_APPLYING      1   /* 已下载校验并置好标志 —— 正常走不到这里（已复位） */
#define OTA_R_FAIL_VERSION  2   /* 上报版本号失败 */
#define OTA_R_FAIL_CHECK    3   /* 检测升级任务失败（响应解析不出，或 code 异常） */
#define OTA_R_FAIL_ERASE    4   /* W25Q64 擦除失败 */
#define OTA_R_FAIL_DOWNLOAD 5   /* 下载失败：网络错、Ota-Errno 非 0、长度不足 */
#define OTA_R_FAIL_MD5      6   /* 固件 MD5 与平台给的对不上（网络传输出错） */
#define OTA_R_FAIL_STATUS   7   /* 上报进度/状态失败 */
#define OTA_R_FAIL_VERIFY   8   /* W25Q64 回读校验失败：数据没真正写进外部 Flash */
#define OTA_R_FAIL_FLAG     9   /* AT24C02 标志回读校验失败：置标志没生效 */

/* 跑一次完整的 OTA 检查。
 *
 * 无任务时很快返回 OTA_R_OK；
 * 有任务时依次完成下载、校验、置标志、复位 —— 复位之后不会再返回。
 *
 * 调用建议：放在 main 的初始化之后跑一次即可，不必放进主循环轮询 ——
 * 整个流程（含 13 次 HTTP 往返）要十几秒，塞进循环会拖累业务。
 * 真要做周期性检查，也应该由业务自己决定节奏（比如每 10 分钟一次）。 */
uint8_t OTA_Run(void);

#endif
