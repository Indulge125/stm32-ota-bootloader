#include "stm32f10x.h"
#include "4G.h"
#include "Delay.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ================= 实现说明 =================
 * 模组：ESP8266（乐鑫官方 AT 固件），挂 USART2。
 * 传输层：USART2 + 空闲中断 + 线性接收缓冲。
 * 比 USART1 的 DMA+环形槽位简单 —— AT 应答是短文本，
 * 一次空闲中断收一帧足够；后续做固件分包下载再考虑加 DMA。
 *
 * 复位时序：PB2 输出低脉冲（约 200ms）后释放，等模组开机。
 * ESP8266 多数模块 RST 低有效；若复位无效先核对手册里的极性。
 */

static uint8_t  s_rxBuf[G4_RX_SIZE];
static volatile uint16_t s_rxLen = 0;
static volatile uint8_t  s_rxOverflow = 0;
static uint8_t  s_verbose = 1;		/* 默认打开 AT 收发日志，方便联调 */

void G4_SetVerbose(uint8_t on)
{
	s_verbose = on;
}

/* 把接收缓冲打到串口1；空缓冲打 (无数据) */
static void G4_LogRx(void)
{
	uint16_t i;

	if(s_verbose == 0)
	{
		return;
	}
	if(s_rxLen == 0)
	{
		U1_printf("[RX] (无数据)\r\n");
		return;
	}
	U1_printf("[RX] ");
	for(i = 0; i < s_rxLen; i ++)
	{
		if(s_rxBuf[i] == '\r')
		{
			U1_printf("\\r");
		}
		else if(s_rxBuf[i] == '\n')
		{
			U1_printf("\\n");
		}
		else if(s_rxBuf[i] < 0x20 || s_rxBuf[i] > 0x7E)
		{
			U1_printf("\\x%02X", s_rxBuf[i]);		/* 不可见字节用十六进制 */
		}
		else
		{
			U1_printf("%c", s_rxBuf[i]);
		}
	}
	U1_printf("\r\n");
}

static void G4_LogRet(uint8_t ret)
{
	if(s_verbose == 0)
	{
		return;
	}
	switch(ret)
	{
		case G4_OK:          U1_printf("[OK]\r\n"); break;
		case G4_ERR_TIMEOUT: U1_printf("[FAIL] 超时无OK\r\n"); break;
		case G4_ERR_RESP:    U1_printf("[FAIL] 模组回ERROR/FAIL\r\n"); break;
		case G4_ERR_PARAM:   U1_printf("[FAIL] 参数错误\r\n"); break;
		default:             U1_printf("[FAIL] ret=%d\r\n", ret); break;
	}
}

/* ---------- 硬件初始化 ---------- */

/* 初始化 USART2 + PB2 复位脚
 * bandrate 一般 115200（ESP8266 AT 默认）
 * 开 RXNE + 空闲中断，收字节进线性缓冲 s_rxBuf */
void G4_Init(uint32_t bandrate)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	/* ⚠️ 为什么不用 USART2：
	 * STM32F103C8T6 是 LQFP48 封装，USART2 被锁死在 PB10/PB11 ——
	 * 而那两个脚是 AT24C02 的软件 I2C（SCL/SDA），不能动。
	 * USART2 的重映射脚 PC10/PC11、PD8/PD9 在 48 脚封装上不存在，换不了。
	 * → ESP8266 改用 USART2：PA2 = TX，PA3 = RX（当前分配里这两脚是空的）。 */
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

	/* PB2 = 模组复位脚，推挽输出，默认释放（高）
	 * 注意：PB2 就是 BOOT1，复位时被采样。板上 BOOT1 跳线必须处于 0 位，
	 * 否则复用成 GPIO 会导致启动模式异常。若不放心可把 G4_RST_PIN 换到 PB0/PB12。 */
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin   = G4_RST_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(G4_RST_PORT, &GPIO_InitStructure);
	GPIO_SetBits(G4_RST_PORT, G4_RST_PIN);

	/* PA2 = USART2_TX，复用推挽 */
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin   = G4_TX_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(G4_TX_PORT, &GPIO_InitStructure);

	/* PA3 = USART2_RX，上拉输入 */
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin   = G4_RX_PIN;
	GPIO_Init(G4_RX_PORT, &GPIO_InitStructure);

	USART_DeInit(USART2);

	USART_InitStructure.USART_BaudRate            = bandrate;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
	USART_InitStructure.USART_Parity              = USART_Parity_No;
	USART_InitStructure.USART_StopBits            = USART_StopBits_1;
	USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
	USART_Init(USART2, &USART_InitStructure);

	/* 空闲中断：一帧 AT 应答结束后标记就绪 */
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
	USART_ITConfig(USART2, USART_IT_IDLE, ENABLE);

	NVIC_InitStructure.NVIC_IRQChannel                   = USART2_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 2;
	NVIC_Init(&NVIC_InitStructure);

	G4_ClearRx();
	USART_Cmd(USART2, ENABLE);
}

/* 硬件复位模组：低脉冲 200ms，再给开机启动留 2s
 * 每次 BootLoader 上电都会调，保证模组和 MCU 从已知状态开始 */
void G4_Reset(void)
{
	GPIO_ResetBits(G4_RST_PORT, G4_RST_PIN);
	Delay_ms(200);
	GPIO_SetBits(G4_RST_PORT, G4_RST_PIN);
	Delay_ms(2000);
	G4_ClearRx();
}

/* ---------- 收发原语 ----------
 * 缓冲是「线性 + 人工清零」：发命令前 ClearRx，轮询 WaitResp 看内容。
 * 简单够用；若以后要做固件大数据流，再换 DMA + 环形缓冲。 */

void G4_ClearRx(void)
{
	s_rxLen = 0;
	s_rxOverflow = 0;
	memset(s_rxBuf, 0, G4_RX_SIZE);
}

/* ================= 【4a 联调用】+IPD 接收测试 =================
 * AT 模式下模组收到 TCP 数据会吐出：  +IPD,<长度>:<原始字节>
 * 长度是十进制文本，冒号之后就是要按字节数取走的载荷。
 *
 * 本函数在接收缓冲里找 +IPD，解析长度，等载荷到齐后打印出来。
 * ⚠️ 只适合小段数据（G4_RX_SIZE = 512）；收真实固件必须改成
 *    边收边写 W25Q64 的流式处理，不能靠这个缓冲兜。
 */
uint8_t G4_RxTest(uint32_t timeout_ms)
{
	uint8_t  *b;
	uint16_t  n, i, j, k;
	uint32_t  len;
	uint32_t  waited = 0;

	G4_ClearRx();

	while(waited < timeout_ms)
	{
		b = (uint8_t *)G4_RxBuf();
		n = G4_RxLen();

		/* 在缓冲里找 "+IPD," */
		for(i = 0; (uint32_t)i + 5 <= n; i ++)
		{
			if(memcmp(&b[i], "+IPD,", 5) != 0)
			{
				continue;
			}

			/* 解析十进制长度，直到 ':' */
			len = 0;
			j = i + 5;
			while((j < n) && (b[j] >= '0') && (b[j] <= '9'))
			{
				len = len * 10 + (uint32_t)(b[j] - '0');
				j ++;
			}
			if((j >= n) || (b[j] != ':'))
			{
				continue;					/* 长度还没收全，继续等 */
			}
			j ++;							/* 跳过 ':' */

			if((uint32_t)(n - j) < len)
			{
				continue;					/* 载荷还没到齐，继续等 */
			}

			/* 载荷到齐了 */
			U1_printf("[IPD] 收到 %u 字节: ", (unsigned int)len);
			for(k = 0; k < len; k ++)
			{
				if((b[j + k] >= 0x20) && (b[j + k] < 0x7F))
				{
					U1_printf("%c", b[j + k]);
				}
				else
				{
					U1_printf("\\x%02X", b[j + k]);		/* 非可见字节用十六进制 */
				}
			}
			U1_printf("\r\n");
			return G4_OK;
		}

		Delay_ms(20);
		waited += 20;
	}

	U1_printf("[IPD] 超时：%ums 内没收到 +IPD\r\n", (unsigned int)timeout_ms);
	return G4_ERR_TIMEOUT;
}

uint16_t G4_RxLen(void)
{
	return s_rxLen;
}

const uint8_t *G4_RxBuf(void)
{
	return s_rxBuf;
}

/* 原样发送字符串（不补换行） */
void G4_SendString(const char *str)
{
	const uint8_t *p = (const uint8_t *)str;

	while(*p != '\0')
	{
		while(USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
		USART_SendData(USART2, *p);
		p ++;
	}
	while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}

/* 发送一条 AT 命令（自动补 \r\n —— 模组以 CR/LF 结尾认命令） */
void G4_SendCmd(const char *cmd)
{
	G4_SendString(cmd);
	G4_SendString("\r\n");
}

/* 在接收缓冲里找子串。找不到返回 NULL */
static const char *G4_Find(const char *hay, const char *needle)
{
	return strstr(hay, needle);
}

/* 等待应答：每 1ms 查一次接收缓冲
 * expect 非空则等该子串；缓冲里出现 ERROR/FAIL 立刻失败返回
 * 超时返回 G4_ERR_TIMEOUT，不阻塞比 timeout 更久 */
uint8_t G4_WaitResp(const char *expect, uint32_t timeout_ms)
{
	uint32_t i;

	for(i = 0; i < timeout_ms; i ++)
	{
		/* 先判错误应答，有错立刻返回，不干等超时 */
		if(G4_Find((const char *)s_rxBuf, "ERROR") != 0 ||
		   G4_Find((const char *)s_rxBuf, "FAIL") != 0)
		{
			return G4_ERR_RESP;
		}
		if(expect != 0 && G4_Find((const char *)s_rxBuf, expect) != 0)
		{
			return G4_OK;
		}
		Delay_ms(1);
	}
	return G4_ERR_TIMEOUT;
}

/* 发一条 AT 并等 OK。先清缓冲，避免串到上一次的残留应答
 * verbose 开启时会打 [TX]/[RX]/[OK|FAIL]，格式：
 *   [TX] AT+CWJAP="ssid","pass"
 *   [RX] WIFI CONNECTED\nWIFI GOT IP\nOK
 *   [OK] */
uint8_t G4_Cmd(const char *cmd, uint32_t timeout_ms)
{
	uint8_t ret;

	if(s_verbose)
	{
		U1_printf("[TX] %s\r\n", cmd);
	}
	G4_ClearRx();
	G4_SendCmd(cmd);
	ret = G4_WaitResp("OK", timeout_ms);
	G4_LogRx();
	G4_LogRet(ret);
	return ret;
}

/* ---------- 业务封装（官方 ESP8266 AT） ---------- */

uint8_t G4_AT_Test(void)
{
	uint8_t retry;
	uint32_t baud;
	static const uint32_t kTryBaud[] = {115200, 9600, 74880};
	uint8_t bi;

	/* 依次试常见波特率：官方 AT=115200，部分模块=9600，ROM 启动日志=74880 */
	for(bi = 0; bi < 3; bi ++)
	{
		baud = kTryBaud[bi];
		USART_Cmd(USART2, DISABLE);
		USART_InitTypeDef us;
		us.USART_BaudRate            = baud;
		us.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
		us.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
		us.USART_Parity              = USART_Parity_No;
		us.USART_StopBits            = USART_StopBits_1;
		us.USART_WordLength          = USART_WordLength_8b;
		USART_Init(USART2, &us);
		USART_Cmd(USART2, ENABLE);

		if(s_verbose)
		{
			U1_printf("[BAUD] 尝试 %u bps\r\n", (unsigned int)baud);
		}

		for(retry = 1; retry <= 3; retry ++)
		{
			if(s_verbose)
			{
				U1_printf("[TRY] AT 测试第 %d/3 次\r\n", retry);
			}
			if(G4_Cmd("AT", 500) == G4_OK)
			{
				U1_printf("[BAUD] 波特率 %u 可用\r\n", (unsigned int)baud);
				return G4_OK;
			}
			Delay_ms(100);
		}
	}
	return G4_ERR_TIMEOUT;
}

/* Station 模式 */
uint8_t G4_SetStation(void)
{
	return G4_Cmd("AT+CWMODE=1", 1000);
}

/* AT+CWJAP="ssid","pass" —— 加入热点。入网耗时，超时给 15s */
uint8_t G4_JoinAP(const char *ssid, const char *pass)
{
	char cmd[G4_CMD_MAX];

	if(ssid == 0 || ssid[0] == '\0' || strlen(ssid) > 32)
	{
		return G4_ERR_PARAM;
	}
	if(pass == 0)
	{
		pass = "";
	}
	if(strlen(pass) > 64)
	{
		return G4_ERR_PARAM;
	}

	sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
	return G4_Cmd(cmd, 15000);
}

/* AT+CIPSTART="TCP","host",port —— 单连接模式下建立 TCP */
uint8_t G4_TcpConnect(const char *host, uint16_t port)
{
	char cmd[G4_CMD_MAX];

	if(host == 0 || host[0] == '\0' || strlen(host) > 64)
	{
		return G4_ERR_PARAM;
	}

	/* 先确保单连接；已是单连接时模组回 ERROR，忽略即可 */
	G4_Cmd("AT+CIPMUX=0", 1000);

	sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%u", host, (unsigned int)port);
	/* 域名解析 + 建连，超时给 10s */
	G4_ClearRx();
	/* ⚠️ CIPSTART 走 G4_SendCmd，而 G4_SendCmd 不打日志。
	 * 这条命令的结果是 CONNECT 还是 ERROR、还是超时，必须看得见 ——
	 * 否则"连接成功"是真是假没法判断（曾经因为日志里看不到这条，
	 * 误判成"命令根本没发出去"）。所以这里手动补 [TX]/[RX]。 */
	if(s_verbose)
	{
		U1_printf("[TX] %s\r\n", cmd);
	}
	G4_SendCmd(cmd);
	/* 不同固件回 "OK" / "CONNECT" / "ALREADY CONNECTED"，任一都算通 */
	if(G4_WaitResp("CONNECT", 10000) == G4_OK ||
	   G4_WaitResp("ALREADY", 500) == G4_OK ||
	   G4_WaitResp("OK", 500) == G4_OK)
	{
		if(s_verbose)
		{
			G4_LogRx();
		}
		return G4_OK;
	}
	if(s_verbose)
	{
		G4_LogRx();
		G4_LogRet(G4_ERR_TIMEOUT);
	}
	return G4_ERR_TIMEOUT;
}

uint8_t G4_TcpClose(void)
{
	return G4_Cmd("AT+CIPCLOSE", 2000);
}

/* 连 WiFi + 建 TCP，对应「设置服务器信息后发起连接」 */
uint8_t G4_Connect(const char *ssid, const char *pass,
                   const char *host, uint16_t port)
{
	uint8_t ret;

	ret = G4_SetStation();
	if(ret != G4_OK)
	{
		return ret;
	}
	ret = G4_JoinAP(ssid, pass);
	if(ret != G4_OK)
	{
		return ret;
	}
	return G4_TcpConnect(host, port);
}

/* ---------- USART2 中断 ---------- */

void USART2_IRQHandler(void)
{
	uint8_t byte;

	if(USART_GetITStatus(USART2, USART_IT_RXNE) == SET)
	{
		byte = (uint8_t)USART_ReceiveData(USART2);
		if(s_rxLen < G4_RX_SIZE - 1)
		{
			s_rxBuf[s_rxLen ++] = byte;
			s_rxBuf[s_rxLen] = '\0';		/* 始终保持可当 C 字符串用 */
		}
		else
		{
			s_rxOverflow = 1;
		}
	}

	if(USART_GetITStatus(USART2, USART_IT_IDLE) == SET)
	{
		/* 清空闲标志：先读 SR 再读 DR（与 USART1 相同） */
		USART_GetFlagStatus(USART2, USART_FLAG_IDLE);
		USART_ReceiveData(USART2);
		/* 一帧结束，此处不做额外处理；
		 * G4_WaitResp 会轮询缓冲区内容 */
	}
}
