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

/* ================= 流式消费接口（4b-2） ================= */

/* 从接收缓冲头部丢弃 n 字节。流式解析靠它把已处理的字节移出去，
 * 否则缓冲很快堆满、新数据进不来（表现为"传着传着就不动了"）。
 *
 * s_rxLen 由 USART2 中断更新，搬移期间必须关中断，否则：
 *   中断在搬移中途插进来 -> s_rxLen 变了 -> 搬到一半的数据错位。
 * 搬 512 字节约几微秒，而 115200 下 87us 才来一个字节 —— 关得住，不丢字节。 */
void G4_RxDrop(uint16_t n)
{
	uint16_t i;
	if(n == 0) return;

	NVIC_DisableIRQ(USART2_IRQn);
	if(n >= s_rxLen)
	{
		s_rxLen = 0;
		NVIC_EnableIRQ(USART2_IRQn);
		return;
	}
	for(i = 0; i + n < s_rxLen; i ++)
	{
		s_rxBuf[i] = s_rxBuf[i + n];
	}
	s_rxLen -= n;
	NVIC_EnableIRQ(USART2_IRQn);
}

/* +IPD 解析状态：0=在找信封 2=正在收载荷 */
static uint8_t  s_ipdState = 0;
static uint32_t s_ipdRemain = 0;

void G4_PayloadReset(void)
{
	s_ipdState  = 0;
	s_ipdRemain = 0;
	G4_ClearRx();
}

/* 批量取载荷：一次最多取 max 个字节，返回实际取到的个数。
 *
 * 为什么需要它：G4_PayloadGet 每取 1 字节就调一次 G4_RxDrop(1)，
 * 而 G4_RxDrop 要从头部搬移 s_rxLen-1 个字节 —— 于是"缓冲越满、单字节越贵"，
 * 满缓冲时单字节约 100us，而 115200 下 86.8us 就来一个字节，排空追不上到达，
 * 一直填到溢出。4b-2b 实测：收 13000 字节到 11208 就溢出了，溢出标志被置 1。
 *
 * 这里改成"取一批、搬一次"：256 字节一批时，搬移总量降到逐字节版的约 1/400。
 *
 * 关键点：只有在"两个信封之间"才去找 "+IPD,"；一旦进入载荷就严格按长度数，
 * 总共取 s_ipdRemain 个字节。这样载荷里出现 "+IPD" 也不会被误当成信封。 */
uint16_t G4_PayloadRead(uint8_t *dst, uint16_t max)
{
	uint8_t  *b;
	uint16_t  n, i, j, take;
	uint32_t  len;

	if(max == 0) return 0;

	for(;;)
	{
		n = G4_RxLen();
		if(n == 0) return 0;

		/* --- 正在收载荷：一次取走一批，只搬移一次缓冲 --- */
		if(s_ipdState == 2)
		{
			b = (uint8_t *)G4_RxBuf();
			take = (s_ipdRemain > (uint32_t)n) ? n : (uint16_t)s_ipdRemain;
			if(take > max) take = max;
			if(take == 0) return 0;
			for(i = 0; i < take; i ++)
			{
				dst[i] = b[i];
			}
			G4_RxDrop(take);				/* 整批只搬这一次 */
			s_ipdRemain -= take;
			if(s_ipdRemain == 0)
			{
				s_ipdState = 0;			/* 本包装完，回到找信封 */
			}
			return take;
		}

		/* --- 找信封 "+IPD," --- */
		b = (uint8_t *)G4_RxBuf();
		for(i = 0; (uint32_t)i + 5 <= n; i ++)
		{
			if(memcmp(&b[i], "+IPD,", 5) == 0) break;
		}
		if((uint32_t)i + 5 > n)
		{
			/* 没找到完整信封：丢掉绝大部分，只留最后 4 字节（可能是半个 "+IPD"） */
			if(n > 4) G4_RxDrop(n - 4);
			return 0;
		}
		if(i > 0)
		{
			G4_RxDrop(i);				/* 丢掉信封之前的杂字节 */
		}

		/* --- 解析十进制长度，直到 ':' --- */
		b = (uint8_t *)G4_RxBuf();
		n = G4_RxLen();
		len = 0;
		j = 5;
		while((j < n) && (b[j] >= '0') && (b[j] <= '9'))
		{
			len = len * 10 + (uint32_t)(b[j] - '0');
			j ++;
		}
		if(j >= n)
		{
			return 0;					/* 长度还没收全，等下一批 */
		}
		if(b[j] != ':')
		{
			G4_RxDrop(5);				/* 格式不对，丢掉 "+IPD," 重来 */
			return 0;
		}
		G4_RxDrop(j + 1);				/* 丢掉 "+IPD,<len>:" 整个信封 */

		if(len == 0)
		{
			continue;					/* 空信封，回去继续找下一个 */
		}
		s_ipdRemain = len;
		s_ipdState  = 2;
		/* 不 return —— 立刻回到循环开头把这批载荷取走，
		 * 少一次空转，也少一轮调用方的 Delay */
	}
}

/* 取下一个纯载荷字节（自动剥掉 +IPD,<len>: 信封）。
 * 返回 1=取到，0=暂时没有数据。
 *
 * 现在只是批量版的薄封装 —— 信封状态机只保留 G4_PayloadRead 一份实现，
 * 免得两处逻辑漂移（这个项目已经因为"两处必须同步"被咬过）。
 * 收大块数据（如固件）请直接用 G4_PayloadRead，别用这个逐字节版。 */
uint8_t G4_PayloadGet(uint8_t *out)
{
	return (G4_PayloadRead(out, 1) == 1) ? 1 : 0;
}


/* ================= 【4b-2a】流式 +IPD 接收测试 =================
 * 20 秒内把所有载荷字节累加，结束时打印摘要：
 *   总字节数 + 前 32 字节 + 后 32 字节
 *
 * 为什么不打印全部：调试串口 9600bps，14KB 的十六进制要打 45 秒，
 * 而且打印会把接收循环卡住 -> 缓冲溢出 -> 丢数据。摘要足够判断对错。
 */
uint8_t G4_RxTest(uint32_t timeout_ms)
{
	uint8_t  byte;
	uint8_t  first[32];
	uint8_t  last[32];
	uint16_t fcnt = 0;
	uint16_t i;
	uint32_t total = 0;
	uint32_t waited = 0;
	uint32_t quiet  = 0;			/* 连续没收到数据的时长 */

	G4_PayloadReset();

	while((waited < timeout_ms) && (quiet < 3000))
	{
		if(G4_PayloadGet(&byte))
		{
			if(fcnt < 32)
			{
				first[fcnt ++] = byte;	/* 记开头 */
			}
			/* 环形记最后 32 字节 */
			last[total % 32] = byte;
			total ++;
			quiet = 0;
		}
		else
		{
			Delay_ms(5);
			waited += 5;
			quiet  += 5;
		}
	}

	if(total == 0)
	{
		U1_printf("[IPD] 一个载荷字节都没收到\r\n");
		return G4_ERR_TIMEOUT;
	}

	U1_printf("[IPD] 共收到 %u 字节\r\n", (unsigned int)total);

	U1_printf("  开头 %u 字节: ", (unsigned int)fcnt);
	for(i = 0; i < fcnt; i ++)
	{
		U1_printf("%02X ", first[i]);
	}
	U1_printf("\r\n");

	U1_printf("  结尾 (按收到顺序): ");
	if(total <= 32)
	{
		for(i = 0; i < (uint16_t)total; i ++)
		{
			U1_printf("%02X ", last[i]);
		}
	}
	else
	{
		/* last[] 是环形缓冲，从 total%32 处开始才是正确的先后顺序 */
		for(i = 0; i < 32; i ++)
		{
			U1_printf("%02X ", last[(total + i) % 32]);
		}
	}
	U1_printf("\r\n");

	return G4_OK;
}

uint16_t G4_RxLen(void)
{
	return s_rxLen;
}

const uint8_t *G4_RxBuf(void)
{
	return s_rxBuf;
}

/* 接收缓冲溢出标志：USART2 中断里缓冲满时置位并丢弃该字节。
 * 以前只写不读 —— 溢出是**完全静默**的，只表现为"数据莫名少了一截"，
 * 排查时很容易误判成网络丢包。4b-2b 收完固件读一次，
 * 把"静默损坏"变成"有据可查"。
 * 由 G4_ClearRx() 清（发命令前 / G4_PayloadReset 时）。 */
uint8_t G4_RxOverflow(void)
{
	return s_rxOverflow;
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

/* 向服务器发原始字节。AT 流程：
 *   AT+CIPSEND=<len>  →  模组回 "OK" 和 ">" 提示符
 *   <len 个原始字节>
 *   模组回 "SEND OK"
 *
 * 为什么单独写而不复用 G4_SendString：
 *   G4_SendString 是给文本用的（依赖 \0 结尾），这里必须是定长字节流，
 *   固件里可能出现 0x00。
 */
uint8_t G4_TcpSend(const uint8_t *data, uint16_t len)
{
	char     cmd[32];
	uint16_t i;

	if((data == 0) || (len == 0))
	{
		return G4_ERR_PARAM;
	}

	sprintf(cmd, "AT+CIPSEND=%u", (unsigned int)len);

	G4_ClearRx();
	if(s_verbose)
	{
		U1_printf("[TX] %s\r\n", cmd);
	}
	G4_SendCmd(cmd);

	/* 等 ">" 提示符：模组表示"可以开始灌数据了" */
	if(G4_WaitResp(">", 3000) != G4_OK)
	{
		if(s_verbose)
		{
			G4_LogRx();
			G4_LogRet(G4_ERR_TIMEOUT);
		}
		return G4_ERR_TIMEOUT;
	}

	/* 灌原始字节 */
	for(i = 0; i < len; i ++)
	{
		while(USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
		USART_SendData(USART2, data[i]);
	}
	while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);

	/* 等 SEND OK */
	G4_ClearRx();
	if(G4_WaitResp("SEND OK", 5000) == G4_OK)
	{
		if(s_verbose)
		{
			U1_printf("[OK] 已发送 %u 字节\r\n", (unsigned int)len);
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
