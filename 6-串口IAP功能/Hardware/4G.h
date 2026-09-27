#ifndef __4G_H
#define __4G_H

#include "stm32f10x.h"

/* ================= ESP8266 引脚（挂在 USART2） =================
 * PB2           ESP_RST  输出，低脉冲复位模组（PB2 = BOOT1，板上跳线须置 0）
 * PA2           USART2_TX → ESP8266 RXD
 * PA3           USART2_RX ← ESP8266 TXD
 *
 * ⚠️ 为什么不是 USART3：
 *   STM32F103C8T6（LQFP48）的 USART3 锁死在 PB10/PB11，而那是 AT24C02 的软件 I2C。
 *   USART3 重映射脚 PC10/PC11、PD8/PD9 在 48 脚封装上不存在 —— 换不了。
 *   所以改用 USART2，PA2/PA3 在当前引脚分配里是空的。
 *
 * 模组固件：乐鑫官方 AT 指令集（AT+CWMODE / AT+CWJAP / AT+CIPSTART ...）
 * 默认波特率 115200-8-N-1。
 */
#define G4_RST_PORT         GPIOB
#define G4_RST_PIN          GPIO_Pin_2

#define G4_TX_PORT          GPIOA
#define G4_TX_PIN           GPIO_Pin_2
#define G4_RX_PORT          GPIOA
#define G4_RX_PIN           GPIO_Pin_3

#define G4_BAUD_DEFAULT     115200

#define G4_RX_SIZE          512			/* 接收缓冲区（AT 应答 + 小批量数据） */
#define G4_CMD_MAX          160			/* 单条 AT 命令最大长度 */

/* AT 交互结果 */
#define G4_OK               0
#define G4_ERR_TIMEOUT      1
#define G4_ERR_RESP         2			/* 模组回了 ERROR / FAIL */
#define G4_ERR_PARAM        3

void G4_Init(uint32_t bandrate);
void G4_Reset(void);

void G4_SendString(const char *str);
void G4_SendCmd(const char *cmd);		/* 自动补 \r\n */
void G4_ClearRx(void);

/* 等待应答：在缓冲区里找 expect 子串，超时返回 G4_ERR_TIMEOUT
 * 若缓冲区出现 ERROR/FAIL 则尽快返回 G4_ERR_RESP */
uint8_t G4_WaitResp(const char *expect, uint32_t timeout_ms);

/* 发一条 AT 并等 OK */
uint8_t G4_Cmd(const char *cmd, uint32_t timeout_ms);

/* 调试日志开关（默认开）：
 * 打开后每条 AT 会往串口1打 [TX]/[RX]/[OK]/[FAIL] */
void G4_SetVerbose(uint8_t on);

/* ---------- 业务封装（官方 ESP8266 AT） ---------- */
uint8_t G4_AT_Test(void);									/* AT → OK */
uint8_t G4_SetStation(void);								/* AT+CWMODE=1 */
uint8_t G4_JoinAP(const char *ssid, const char *pass);		/* AT+CWJAP="ssid","pass" */
uint8_t G4_TcpConnect(const char *host, uint16_t port);		/* AT+CIPSTART="TCP",host,port */
uint8_t G4_TcpClose(void);									/* AT+CIPCLOSE */

/* 连上 WiFi 并建立 TCP 连接（对应命令行设置服务器后发起连接） */
uint8_t G4_Connect(const char *ssid, const char *pass,
                   const char *host, uint16_t port);

uint16_t G4_RxLen(void);
/* 【4a 联调用】等待一段 +IPD 数据并打印出来（长度 + 内容）。
 * 返回 G4_OK 表示收到，G4_ERR_TIMEOUT 表示超时。
 * 只适合小段数据（接收缓冲 512 字节）；收大固件要走流式处理。 */
uint8_t G4_RxTest(uint32_t timeout_ms);
const uint8_t *G4_RxBuf(void);

void USART2_IRQHandler(void);

#endif
