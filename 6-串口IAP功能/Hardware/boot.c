#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "4G.h"
#include "wifi_cfg.h"
#include "MyFLASH.h"
#include "main.h"
#include "iic.h"
#include "m24c02.h"
#include "boot.h"
#include "W25Q64.h"

load_a load_A;

/* ================= 【4b-2b-1】内网 OTA：收固件写 W25Q64 =================
 * 协议：
 *   MCU   → 服务器   "OTA_REQ\n"
 *   服务器 → MCU     "OTA <真实长度> <CRC16>\n" + <真实长度 字节固件>
 *
 * 落盘位置：W25Q64 **块 0**，搬运口径的长度写进 OTA_Info.FileLen[0]。
 *   ⚠ 是块 0 / FileLen[0]，不是块 1 ——
 *     BootLoader_Branch() 的 OTA 分支置的就是 W25Q64_BlockNum = 0，
 *     main.c 的搬运读的也是 FileLen[0]、从 0*64KB 读。
 *
 * 本步不置 OTA_Flag（那是 4b-2b-2）。这里只把固件落到 W25Q64，
 * 然后置 UpData_A_Flag 交给主循环搬运 —— 和真实 OTA 走同一条搬运代码。
 *
 * 三道校验，任一不过都不触发搬运：
 *   1. 长度：!= 0 且 <= A 区容量（28672）
 *   2. CRC：边收边算，与协议头声明的比
 *   3. 回读 CRC：写完从 W25Q64 读回重算 —— 这一条才证明"真的落盘写对了"
 *      （W25Q64_PageProgram 无返回值、无错误上报，页写失败是静默的）
 *
 * 下面的顺序不能改，理由见每一步的注释：
 *   擦除必须在发请求之前（64KB 块擦 0.2~2 秒，而接收缓冲只装得下 44ms 的数据）；
 *   接收循环里绝不打印（一行 40 字符 @9600 要 42ms，一次就吃掉整个缓冲）。
 */
#define OTA_HDR_MAX    32		/* "OTA <10位> <8位>\n" 最长 25，留余量 */
#define OTA_HDR_MS     5000		/* 等协议头的静默超时 */
#define OTA_TOTAL_MS   60000	/* 收数据的总超时 */
#define OTA_QUIET_MS   2000		/* 连续没收到字节就放弃（服务器块间延时 50ms） */

uint8_t OTA_NetDownload(void)
{
	uint8_t   b;
	uint8_t   hdr[OTA_HDR_MAX];
	uint8_t   hlen = 0;
	uint8_t   hdrDone = 0;
	uint8_t   head[16];		/* 头 16 字节，失败时打印用 */
	uint8_t   last[16];		/* 尾 16 字节，环形 */
	uint32_t  len = 0;
	uint32_t  crcExp = 0;
	uint16_t  crcGot = 0x0000;
	uint32_t  got = 0;
	uint32_t  page = 0;
	uint16_t  inPage = 0;
	uint32_t  waited = 0;
	uint32_t  hdrWait = 0;
	uint32_t  quiet = 0;
	uint32_t  i;
	uint16_t  rlen;
	uint32_t  limit = (uint32_t)MyFlash_A_Page_Num * MyFlash_Page_Size;

	/* ⚠ 下载期间本函数要同步跑好几秒，主循环不会去消费串口事件环 ——
	 * 这段窗口里往调试串口敲的字符会排进环里，等下载结束才被当命令执行
	 * （敲个 1 就是"擦除A区"）。
	 *
	 * 这里**故意不去改环指针**。试过"把 In 拉平到 Out"，但行不通：
	 * URxDataIn 指向正在填充的槽位、它的 start 已被中断设成下一次 DMA 的目标，
	 * 而 main.c 处理完事件后无条件 URxDataOut++ —— 硬拉平会让那个 ++ 落到
	 * 一个陈旧槽位上，其 end 是上轮残留，datalen 会算成垃圾值，反而更危险。
	 * 命令 2/5 的 Xmodem 下载十几秒也有同样的窗口（usart.h 开头记过同类问题），
	 * 一直靠"下载期间别敲"约束。这里同样明确提示。 */
	U1_printf("[OTA] 下载期间请不要往调试串口敲字符\r\n");

	U1_printf("[OTA] 连接服务器 ...\r\n");
	G4_Init(G4_BAUD_DEFAULT);
	if(G4_TcpConnect(SERVER_HOST, SERVER_PORT) != G4_OK)
	{
		U1_printf("[OTA] 连不上服务器 —— 查穿透是否在线、端口映射、[8] 是否已连 WiFi\r\n");
		return 0;
	}

	U1_printf("[OTA] 擦除 W25Q64 块 0（整块 64KB，可能要 1~2 秒）...\r\n");
	W25Q64_Erase64K(0);		/* 必须在发请求之前，见文件头说明 */

	G4_SetVerbose(0);		/* 从这儿开始闭嘴：verbose 一次 dump 能吃掉整个接收缓冲 */
	G4_PayloadReset();
	G4_TcpSend((const uint8_t *)"OTA_REQ\n", 8);
	/* 上面这句的返回值不当判据：G4_TcpSend 结尾会 G4_ClearRx() 再等 SEND OK，
	 * 而 G4_WaitResp 是"先判 ERROR/FAIL 再判期望串"、且 strstr 遇 0x00 截断。
	 * 固件里出现 "ERROR"/"FAIL" 字样时会误报。真正的判据只有长度 + CRC。 */

	/* ---- 收协议头 ---- */
	U1_printf("[OTA] 等待协议头 ...\r\n");
	while(hdrWait < OTA_HDR_MS)
	{
		if(G4_PayloadGet(&b))
		{
			hdrWait = 0;			/* 有进展就重新计时 */
			if(b == '\n') { hdrDone = 1; break; }
			if(hlen >= OTA_HDR_MAX - 1) break;	/* 头太长，肯定不对 */
			hdr[hlen ++] = b;
		}
		else
		{
			Delay_ms(1);
			hdrWait ++;
		}
	}
	hdr[hlen] = '\0';		/* sscanf 只能吃 NUL 结尾的串 */
	if(!hdrDone)
	{
		U1_printf("[OTA] 等协议头超时（%d 秒）\r\n", OTA_HDR_MS / 1000);
		G4_SetVerbose(1);
		return 0;
	}
	U1_printf("[OTA] 头: %s\r\n", hdr);
	if(sscanf((char *)hdr, "OTA %u %x", &len, &crcExp) != 2)
	{
		U1_printf("[OTA] 协议头格式不对\r\n");
		G4_SetVerbose(1);
		return 0;
	}
	U1_printf("[OTA] 长度 %u，期望 CRC %04X\r\n", (unsigned int)len, (unsigned int)crcExp);

	/* 这两条必须挡在写入和搬运之前：
	 *   长度 0  -> 搬运会擦完 A 区却一个字节都不写回去；
	 *   超容量  -> 搬运循环写穿 0x0800FFFF，而 i 是 uint8_t 还会回绕重写。
	 * 失败路径只清 UpData_A_Flag、OTA_Flag 仍在，会变成每次复位都重试的死循环。 */
	if(len == 0)
	{
		U1_printf("[OTA] 长度是 0，拒绝\r\n");
		G4_SetVerbose(1);
		return 0;
	}
	if(len > limit)
	{
		U1_printf("[OTA] 长度 %u 超出 A 区容量 %u，拒绝\r\n",
		          (unsigned int)len, (unsigned int)limit);
		G4_SetVerbose(1);
		return 0;
	}

	/* ---- 收数据：边收边写，每 256 字节一页 ---------- */
	U1_printf("[OTA] 开始接收 %u 字节 ...\r\n", (unsigned int)len);
	while(got < len)
	{
		if(G4_PayloadGet(&b))
		{
			quiet = 0;
			UpDataA.UpDataBuff[inPage ++] = b;
			crcGot = Xmodem_CRC16_Update(crcGot, &b, 1);
			if(got < 16) head[got] = b;
			last[got % 16] = b;
			got ++;
			if(inPage == 256)
			{
				W25Q64_PageProgram(page, UpDataA.UpDataBuff, 256);
				page ++;
				inPage = 0;
			}
		}
		else
		{
			Delay_ms(1);
			waited ++;
			quiet  ++;
			if(quiet >= OTA_QUIET_MS)
			{
				U1_printf("[OTA] 连续 %d 毫秒没收到数据，放弃（已收 %u / %u 字节）\r\n",
				          OTA_QUIET_MS, (unsigned int)got, (unsigned int)len);
				G4_SetVerbose(1);
				return 0;
			}
			if(waited >= OTA_TOTAL_MS)
			{
				U1_printf("[OTA] 总超时（%d 秒），已收 %u / %u 字节\r\n",
				          OTA_TOTAL_MS / 1000, (unsigned int)got, (unsigned int)len);
				G4_SetVerbose(1);
				return 0;
			}
		}
	}

	/* 尾页不足 256 字节：补 0xFF 填满整页再写。
	 * 必须补 0xFF —— 这样 [len, 向上取整) 那几字节在 W25Q64 里就是 0xFF，
	 * 搬运时写进已擦除的 A 区是 no-op，不会置 PGERR。
	 * 若图省事直接把缓冲里的残留写下去，回读 CRC 也会跟着错。 */
	if(inPage > 0)
	{
		while(inPage < 256) UpDataA.UpDataBuff[inPage ++] = 0xFF;
		W25Q64_PageProgram(page, UpDataA.UpDataBuff, 256);
	}

	/* ---- 校验 1：长度 + 传输 CRC ---- */
	if(got != len)
	{
		U1_printf("[OTA] 字节数不符：收到 %u，协议头声明 %u\r\n",
		          (unsigned int)got, (unsigned int)len);
		G4_SetVerbose(1);
		return 0;
	}
	if(crcGot != (uint16_t)crcExp)
	{
		U1_printf("[OTA] CRC 不符：收到 %04X，期望 %04X\r\n",
		          (unsigned int)crcGot, (unsigned int)crcExp);
		G4_SetVerbose(1);
		return 0;
	}
	U1_printf("[OTA] 收完 %u 字节，传输 CRC %04X 通过\r\n",
	          (unsigned int)len, (unsigned int)crcGot);
	U1_printf("[OTA] 接收缓冲溢出标志：%u（应为 0）\r\n", (unsigned int)G4_RxOverflow());

	/* ---- 校验 2：从 W25Q64 回读重算 ----
	 * 这一步才证明"真的落盘写对了"。PageProgram 没有返回值、
	 * WaitBusy 超时后也是静默返回，页写失败只靠收下来的字节是查不出来的。 */
	U1_printf("[OTA] 回读 W25Q64 校验中 ...\r\n");
	crcGot = 0x0000;
	for(i = 0; i < len; i += MyFlash_Page_Size)
	{
		rlen = (uint16_t)(((len - i) > MyFlash_Page_Size) ? MyFlash_Page_Size : (len - i));
		W25Q64_ReadData(i, UpDataA.UpDataBuff, rlen);
		crcGot = Xmodem_CRC16_Update(crcGot, UpDataA.UpDataBuff, rlen);
	}
	if(crcGot != (uint16_t)crcExp)
	{
		U1_printf("[OTA] 回读 CRC 不符：%04X，期望 %04X —— 写入有问题，不搬运\r\n",
		          (unsigned int)crcGot, (unsigned int)crcExp);
		G4_SetVerbose(1);
		return 0;
	}
	U1_printf("[OTA] 回读 CRC %04X 通过 —— 确实写进 W25Q64 了\r\n", (unsigned int)crcGot);

	/* ---- 打印头尾各 16 字节（只打摘要：U1_printf 的缓冲 2048 字节且不查长度）---- */
	U1_printf("[OTA] 头 16: ");
	for(i = 0; i < 16; i ++) U1_printf("%02X ", head[i]);
	U1_printf("\r\n[OTA] 尾 16: ");
	for(i = 0; i < 16; i ++) U1_printf("%02X ", last[(got + i) % 16]);
	U1_printf("\r\n");

	/* ---- 全部通过：交给主循环搬运 ----
	 * FileLen[0] 是"搬运口径"的长度，必须向上取整到 4 的倍数：
	 *   · main.c 的搬运要求 FileLen % 4 == 0；
	 *   · MyFlash_WriteFlash 内部 while(num){...; num -= 4;}，
	 *     num 不是 4 的倍数会无符号回绕、一路写穿 FLASH。
	 * 注意它是搬运长度，**不是固件真实大小** —— 别拿它当固件大小上报。 */
	OTA_Info.FileLen[0] = (len + 3) & ~((uint32_t)3);
	UpDataA.W25Q64_BlockNum = 0;
	BootStaFlag |= UpData_A_Flag;

	U1_printf("[OTA] 就绪：FileLen[0] = %u（固件真实长度 %u）\r\n",
	          (unsigned int)OTA_Info.FileLen[0], (unsigned int)len);
	U1_printf("[OTA] 交给搬运 ...\r\n");

	G4_SetVerbose(1);
	return 1;
}

/* BootLoader分支判断 */
void BootLoader_Branch(void)
{
	if(BootLoader_Enter(50) == 0)						//不进入命令行，才去判断OTA_Flag
	{
		if(OTA_Info.OTA_Flag == OTA_SET_FLAG)			//判断OTA_Flag是不是OTA_SET_FLAG定义的值，是的话进入if
		{
			U1_printf("OTA更新\r\n");					//串口1输出信息
			BootStaFlag |= UpData_A_Flag;				//置位标志位，表明需要更新A区
			UpDataA.W25Q64_BlockNum = 0;				//W25Q64_BlockNum等于0，表明是OTA要更新A区
		}
		else											//判断OTA_Flag是不是OTA_SET_FLAG定义的值，不是的话进入else
		{
			U1_printf("OTA无更新，跳转A区\r\n");			//串口1输出信息
			LOAD_A(MyFlash_A_Start_Address);			//跳转到A区
		}
	}
	U1_printf("进入BootLoader命令行\r\n");
	BootLoader_Info();									//串口输出命令行信息
}

/* 判断是否进入BootLoader命令行 */
uint8_t BootLoader_Enter(uint8_t timeout)
{
	U1_printf("%dms内，输入小写字母 w ,进入BootLoader命令行\r\n", timeout * 100);
	while(timeout -- )
	{
		Delay_ms(100);
		if(USART1_RxBuff[0] == 'w')
		{
			return 1;								//进入命令行
		}
	}
	return 0;										//不进入命令行
}

void BootLoader_Info(void)
{
	U1_printf("\r\n");	
	U1_printf("[1]擦除A区\r\n");	
	U1_printf("[2]串口IAP下载A区程序\r\n");	
	U1_printf("[3]设置OTA版本号\r\n");	
	U1_printf("[4]查询OTA版本号\r\n");	
	U1_printf("[5]向外部FLASH下载程序\r\n");	
	U1_printf("[6]使用外部FLASH内程序\r\n");	
	U1_printf("[7]重启\r\n");
	U1_printf("[0]ESP8266 AT 自测\r\n");
	U1_printf("[8]连WiFi\r\n");
	U1_printf("[9]连服务器\r\n");
	U1_printf("[t]接收测试(4b-2a)\r\n");
	U1_printf("[o]内网OTA下载(4b-2b)\r\n");	
}

/* BootLoader处理串口数据 */
void BootLoader_Event(uint8_t *data, uint16_t datalen)
{
	int temp, i;																										//temp用于版本号sscanf判断格式	i用于for循环
	/* 去掉末尾的 \r\n：串口工具默认会带换行，而命令按接收长度严格匹配
	 * （菜单命令要求 1 字节、版本号要求 26 字节），带了换行就会被静默忽略。
	 * 但不能动 Xmodem 数据包 —— 那是二进制，尾字节可能就是 0x0D/0x0A，
	 * 裁掉会让 CRC 校验失败。 */
	if((BootStaFlag & IAP_XMODEMData_FLAG) == 0)
	{
		while((datalen > 0) &&
		      ((data[datalen - 1] == '\r') || (data[datalen - 1] == '\n')))
		{
			datalen --;
		}
	}
	
	if(BootStaFlag == 0)																//如果BootStaFlag等于0，没有任何事件，进入if，判断是哪个命令
	{
		if((datalen == 1) && (data[0] == '1'))											//如果数据长度1字节且字符是1
		{
			U1_printf("擦除A区\r\n");													//串口输出信息
			MyFlash_EraseFlash(MyFlash_A_Start_Page, MyFlash_A_Page_Num);				//擦除A分区占用的扇区
		}
		else if((datalen == 1) && (data[0] == '2'))										//如果数据长度1字节且字符是2
		{
			U1_printf("通过Xmodem协议，串口IAP下载A区程序，请使用bin格式文件\r\n");			//串口输出信息
			MyFlash_EraseFlash(MyFlash_A_Start_Page, MyFlash_A_Page_Num);				//擦除A分区占用的扇区
			BootStaFlag |= (IAP_XMODEMC_FLAG | IAP_XMODEMData_FLAG);					//置位 IAP_XMODEMC_FLAG 和 IAP_XMODEMData_FLAG 标志位
			UpDataA.XmodemTimer = 0;													//Xmodem发送大写C间隔变量清零
			UpDataA.XmodemNum = 0;														//保持接收Xmodem协议数据包个数的变量清零
		}
		else if((datalen == 1) && (data[0] == '3'))										//如果数据长度1字节且字符是3
		{
			U1_printf("设置版本号\r\n");													//串口输出信息
			BootStaFlag |= IAP_SETVERSION_FLAG;											//置位 IAP_SETVERSION_FLAG 标志位
		}
		else if((datalen == 1) && (data[0] == '4'))										//如果数据长度1字节且字符是4
		{
			U1_printf("查询版本号\r\n");													//串口输出信息
			AT24C02_ReadOTAInfo();														//从24c02读取保存的数据
			U1_printf("版本号:%s\r\n", OTA_Info.OTA_Ver);								//串口输出信息
			BootLoader_Info();															//串口输出命令行信息
		}
		else if((datalen == 1) && (data[0] == '5'))										//如果数据长度1字节且字符是5
		{
			U1_printf("向外部FLASH下载程序，输入需要使用的块编号（1-9）\r\n");			//串口输出信息
			BootStaFlag |= W25Q64_DoLo_FLAG;											//置位 W25Q64_DoLo_FLAG 标志位
		}
		else if((datalen == 1) && (data[0] == '6'))										//如果数据长度1字节且字符是6
		{
			U1_printf("使用外部FLASH内的程序，输入需要使用的块编号（1-9）\r\n");			//串口输出信息
			BootStaFlag |= W25Q64_To_Flash_Dolo_FLAG;									//置位 W25Q64_To_Flash_Dolo_FLAG 标志位
		}
		else if((datalen == 1) && (data[0] == '0'))										//AT 自测：只验证 ESP8266 通信，不涉及 OTA
		{
			U1_printf("ESP8266 AT 通信自测（115200）...\r\n");
			G4_Init(G4_BAUD_DEFAULT);										//初始化 USART2 + PB2 复位脚
			if(G4_AT_Test() == G4_OK)
			{
				U1_printf("[结果] AT 通信正常\r\n");
			}
			else
			{
				U1_printf("[结果] AT 无应答 —— 依次查：\r\n");
				U1_printf("  1) 模组里是不是 AT 固件（USB 接电脑 115200 发 AT 应回 OK）\r\n");
				U1_printf("  2) 模块 TXD->PA3、RXD->PA2，别接反\r\n");
				U1_printf("  3) 模组供电是否够（3.3V，峰值电流大）\r\n");
			}
			BootLoader_Info();										//回到菜单
		}
		else if((datalen == 1) && (data[0] == '8'))										//连 WiFi：用 wifi_cfg.h 里的凭据
		{
			U1_printf("连 WiFi: %s ...\r\n", WIFI_SSID);
			G4_Init(G4_BAUD_DEFAULT);

			/* ⚠️ 顺序不能反：ESP-AT 默认是 SoftAP 模式，
			 * 不先 AT+CWMODE=1 就 AT+CWJAP，会秒回 ERROR。 */
			if(G4_SetStation() != G4_OK)
			{
				U1_printf("[结果] 设置 Station 模式失败 —— 模组可能没跑 AT 固件\r\n");
			}
			else if(G4_JoinAP(WIFI_SSID, WIFI_PASS) == G4_OK)
			{
				U1_printf("[结果] WiFi 连接成功\r\n");
			}
			else
			{
				U1_printf("[结果] 连 WiFi 失败 —— 查 ssid/密码 是否正确、热点是不是 2.4G\r\n");
				U1_printf("        (ESP8266 只支持 2.4GHz，不支持 5GHz)\r\n");
			}
			BootLoader_Info();
		}
		else if((datalen == 1) && (data[0] == '9'))										//连服务器：TCP
		{
			U1_printf("连服务器 %s:%d ...\r\n", SERVER_HOST, SERVER_PORT);
			G4_Init(G4_BAUD_DEFAULT);				/* 也初始化一次，[9] 才能单独执行；G4_Init 不复位模组，不会断掉 [8] 建立的连接 */
			if(G4_TcpConnect(SERVER_HOST, SERVER_PORT) == G4_OK)
			{
				U1_printf("[结果] TCP 连接成功\r\n");
			}
			else
			{
				U1_printf("[结果] TCP 连接失败 —— 查穿透是否在线、端口是否映射、[8] 是否已连上 WiFi\r\n");
			}
			BootLoader_Info();
		}
		else if((datalen == 1) && (data[0] == 'o'))										//4b-2b-1：内网 OTA 下载到 W25Q64
		{
			U1_printf("内网 OTA：收固件写 W25Q64 块 0（不置 OTA_Flag，校验过后交给搬运）\r\n");
			if(OTA_NetDownload())
			{
				BootLoader_Info();
				return;				/* 已置 UpData_A_Flag，让主循环去搬运，别在这里继续往下走 */
			}
			U1_printf("[OTA] 下载未通过校验，未触发搬运 —— A 区保持原样，可直接重试\r\n");
			BootLoader_Info();
		}
		else if((datalen == 1) && (data[0] == 't'))										//4b-1：收发双向测试
		{
			/* 先测发送方向 */
			U1_printf("向服务器发送 PING ...\r\n");
			if(G4_TcpSend((const uint8_t *)"PING\r\n", 6) == G4_OK)
			{
				U1_printf("[结果] 发送成功 —— 看 Hercules 的 Received data 框\r\n");
			}
			else
			{
				U1_printf("[结果] 发送失败 —— 先确认 [8] 连WiFi、[9] 连服务器\r\n");
			}

			/* 再测接收方向 */
			U1_printf("20 秒内把所有收到的东西发过来（看到\"共收到 N 字节\"即结束）...\r\n");
			if(G4_RxTest(30000) == G4_OK)
			{
				U1_printf("[结果] 收发双向都正常\r\n");
			}
			else
			{
				U1_printf("[结果] 没收到回数据 —— 在 Hercules Send 框里发点内容\r\n");
			}
			BootLoader_Info();
		}
		else if((datalen == 1) && (data[0] == '7'))										//如果数据长度1字节且字符是6
		{
			U1_printf("重启\r\n");														//串口输出信息
			Delay_ms(100);																//延时100ms
			NVIC_SystemReset();															//重启
		}
	}
	
	/* 发生Xmodem事件，处理该事件 */
	else if(BootStaFlag & IAP_XMODEMData_FLAG)											//如果 IAP_XMODEMData_FLAG 置位表示开始通过Xmodem协议接收数据
	{
		if((datalen == 133) && (data[0] == 0x01))										//判断Xmodem协议从一包总长133字节且第一个字节帧头是0x01
		{
			BootStaFlag &=~ IAP_XMODEMC_FLAG;											//已经收到数据包了，所以清除 IAP_XMODEMC_FLAG，不再发送大写C
			UpDataA.XmodemCRC = Xmodem_CRC16(&data[3], 128);							//计算本次接收的数据包数据的 CRC
			if(UpDataA.XmodemCRC == data[131] * 256 + data[132])						//计算的 CRC 和接收到的 CRC 比较，一样说明正确，进入if
			{
				UpDataA.XmodemNum ++;													//已接收的数据包数量＋1
				memcpy(&UpDataA.UpDataBuff[((UpDataA.XmodemNum - 1) % (MyFlash_Page_Size / 128)) * 128], &data[3], 128);	//将本次接收的数据，暂存到UpDataA.UpDataBuff缓冲区
				if((UpDataA.XmodemNum % (MyFlash_Page_Size / 128)) == 0)				//对于c8t6而言，如果已接收的数据包数量是8的整数倍，说明都满1扇区的1024字节，进入if
				{
					if(BootStaFlag & W25Q64_DoLo_Xmodem_FLAG)							//判断如果是命令5启动Xmodem的话，进入if
					{
						for(i = 0; i < 4; i++)											//W25Q64每次写入256字节，对c8t6而言，1扇区1024字节，需要循环4次写
						{
							W25Q64_PageProgram((UpDataA.XmodemNum/8 - 1) * 4 + i + UpDataA.W25Q64_BlockNum * 64 * 4, &UpDataA.UpDataBuff[i * 256], 256);	//将接收的数据写入W25Q64
						}
					}
					else																//判断如果不是命令5启动Xmodem的话，那就是串口IAP启动的，进入else
					{
						MyFlash_WriteFlash(MyFlash_A_Start_Address + ((UpDataA.XmodemNum/(MyFlash_Page_Size / 128)) - 1) * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, MyFlash_Page_Size);		//写入到单片机A区相应的扇区
					}
				}
				U1_printf("\x06");														//正确，返回ACK给CRT软件
			}
			else																		//如果CRC校验错误，进入else
			{
				U1_printf("\x15");														//返回NCK给CRT软件
			}
		}
		if((datalen == 1) && (data[0] == 0x04))											//如果收到1个字节数据且是0x04，进入if，说明收到EOT，表明数据已经发送完毕
		{
			U1_printf("\x06");															//返回ACK给CRT软件
			if((UpDataA.XmodemNum % (MyFlash_Page_Size / 128)) != 0)					//对c8t6而言，判断是否还有不满足1扇区1024字节的数据，如果有则进入if，把剩余的数据写入
			{
				if(BootStaFlag & W25Q64_DoLo_Xmodem_FLAG)								//判断如果是命令5启动Xmodem的话，进入if
				{
					/* 尾包只写「真正有新数据」的页：一页 256 字节 = 2 个 128 字节包，
					 * 剩余 rem 字节需要 (rem+255)/256 页。原实现固定写 4 页，
					 * 其中不含新数据的页会把 UpDataBuff 里残留的旧包写进 W25Q64。 */
					uint16_t rem = (UpDataA.XmodemNum % 8) * 128;
					uint16_t pages = (rem + 255) / 256;
					for(i = 0; i < pages; i ++)
					{
						W25Q64_PageProgram((UpDataA.XmodemNum/8) * 4 + i + UpDataA.W25Q64_BlockNum * 64 * 4, &UpDataA.UpDataBuff[i * 256], 256);		//将接收的数据写入W25Q64
					}
				}
				else																	//判断如果不是命令5启动Xmodem的话，那就是串口IAP启动的，进入else
				{
					MyFlash_WriteFlash(MyFlash_A_Start_Address + ((UpDataA.XmodemNum/(MyFlash_Page_Size / 128))) * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, (UpDataA.XmodemNum % (MyFlash_Page_Size / 128)) * 128);		//写入到单片机A区相应的扇区
				}
			}
			BootStaFlag &=~ IAP_XMODEMData_FLAG;										//Xmodem接收完毕，清除标志位
			if(BootStaFlag & W25Q64_DoLo_Xmodem_FLAG)									//判断如果是命令5启动Xmodem的话，进入if
			{
				BootStaFlag &=~ W25Q64_DoLo_Xmodem_FLAG;								//清除 W25Q64_DoLo_Xmodem_FLAG 标志位
				OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] = UpDataA.XmodemNum * 128;	//计算并保存本次传输的程序大小
				AT24C02_WriteOTAInfo();													//保存到24c02
				Delay_ms(100);															//延时
				BootLoader_Info();														//输出命令行信息
			}	
			else
			{
				Delay_ms(100);															//延时
				NVIC_SystemReset();														//重启
			}
		}
	}
	
	/* 发生设置版本号事件，处理该事件 */
	else if(BootStaFlag & IAP_SETVERSION_FLAG)											//进入分支，处理设置版本号事件
	{
		if(datalen == 26)																//判断版本号长度是不是26，是的话进入if
		{
			if(sscanf((char *)data, "VER-%d.%d.%d-%d/%d/%d-%d:%d", &temp, &temp, &temp, &temp, &temp, &temp, &temp, &temp) == 8)		//判断版本号格式，正确进入if
			{
				memset(OTA_Info.OTA_Ver, 0, 32);										//清除 OTA_Info.OTA_Ver 缓冲区
				memcpy(OTA_Info.OTA_Ver, data, 26);										//将串口发送过来的版本号拷贝到 OTA_Info.OTA_Ver 缓冲区
				AT24C02_WriteOTAInfo();													//写入24c02
				U1_printf("版本正确\r\n");												//串口输出信息
				BootStaFlag &=~ IAP_SETVERSION_FLAG;									//清除标志位
				BootLoader_Info();														//输出命令行信息
			}
			else																		//判断版本号格式是否错误
			{
				U1_printf("版本号格式错误\r\n");	
			}
		}
		else																			//判断版本号长度是否错误
		{
			U1_printf("版本号长度错误\r\n");	
		}
	}
	
	/* 发生命令5（向外部FLASH下载程序）事件，处理该事件 */
	else if(BootStaFlag & W25Q64_DoLo_FLAG)												//进入分支，处理命令5，将程序文件写入W25Q64
	{
		if(datalen == 1)																//数据长度是1正确，进入if，表示W25Q64的块编号
		{
			if((data[0] >= 0x31) && (data[0] <= 0x39))									//判断W25Q64的块编号，范围1-9，正确进入if
			{
				UpDataA.W25Q64_BlockNum = data[0] - 0x30;								//将块编号由字符1-9，转换成数字1-9
				BootStaFlag |= (IAP_XMODEMC_FLAG | IAP_XMODEMData_FLAG | W25Q64_DoLo_Xmodem_FLAG);	//置位3个标志位
				UpDataA.XmodemTimer = 0;												//Xmodem发送大写C，间隔时间变量清零
				UpDataA.XmodemNum = 0;													//保持接收Xmodem协议数据包个数的变量清零
				OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] = 0;							//W25Q64的块标号对应的程序大小变量清零
				W25Q64_Erase64K(UpDataA.W25Q64_BlockNum);								//擦除相应的W25Q64块
				U1_printf("通过Xmodem协议，向W25Q64第%d个块下载程序，请使用bin格式文件\r\n", UpDataA.W25Q64_BlockNum);	//串口输出信息
				BootStaFlag &=~ W25Q64_DoLo_FLAG;										//清除标志位
			}
			else																		//判断W25Q64的块标号，范围1-9，错误进入else，串口输出信息
			{
				U1_printf("编号错误\r\n");
			}
		}
		else																			//判断数据长度，不是1的话错误进入else，串口输出信息
		{
			U1_printf("数据长度错误\r\n");
		}
	}
	
	/* 发生命令6（使用外部FLASH内程序）事件，处理该事件 */
	else if(BootStaFlag & W25Q64_To_Flash_Dolo_FLAG)									//进入分支，处理命令6，使用W25Q64的程序
	{
		if(datalen == 1)																//数据长度是1正确，进入if，表示W25Q64的块编号
		{
			if((data[0] >= 0x31) && (data[0] <= 0x39))									//判断W25Q64的块标号，范围1-9，正确进入if
			{
				UpDataA.W25Q64_BlockNum = data[0] - 0x30;								//将块编号由字符1-9，转换成数字1-9
				BootStaFlag |= UpData_A_Flag;											//置位标志位，说明需要更新A区
				BootStaFlag &=~ W25Q64_To_Flash_Dolo_FLAG;								//清除标志位
			}
			else																		//判断W25Q64的块标号，范围1-9，错误进入else，串口输出信息
			{
				U1_printf("编号错误\r\n");
			}
		}
		else																			//判断数据长度，不是1的话错误进入else，串口输出信息
		{
			U1_printf("数据长度错误\r\n");
		}
	}
}


/* 设置SP指针 */
__asm void MSR_SP(uint32_t address)
{
	MSR MSP, r0										//addr的值加载到了r0通用寄存器，然后通过MSR指令，将通用寄存器r0的值写入到MSP主堆栈指针
	BX r14											//返回调用MSP_SP函数的主函数
}

/* 跳转A分区 */
void LOAD_A(uint32_t address)
{
	if(( *(uint32_t *)address >= 0x20000000) && ( *(uint32_t *)address <= 0x20004FFF))		//判断sp栈顶指针的范围是否合法，在对应型号的RAM控件范围内
	{
		MSR_SP( *(uint32_t *)address);														//设置sp
		load_A = (load_a)*(uint32_t *)(address + 4);										//将函数指针load_A指向A区的复位变量
		BootLoader_Clear();																	//清除B区使用的外设
		load_A();																			//调用函数指针load_A，改变pc指针，从而转向A区的复位向量变量，完成跳转
	}
	else
	{
		U1_printf("跳转A区失败\r\n");
	}
}

void BootLoader_Clear(void)
{
	/* 关中断再反初始化：跳转后 VTOR 指向 A 区向量表，
	 * A 区没有 USART1/USART2 的中断服务函数，
	 * 残留中断一触发就会进启动文件的 Default_Handler（死循环）→ A 区挂住。 */
	NVIC_DisableIRQ(USART1_IRQn);
	NVIC_DisableIRQ(USART2_IRQn);
	USART_DeInit(USART1);
	USART_DeInit(USART2);
	GPIO_DeInit(GPIOA);
	GPIO_DeInit(GPIOB);
}

/* Xmodem的CRC16校验（整段，初值 0）
 *
 * 实现挪到 Xmodem_CRC16_Update()，这里只负责给初值。
 * 拆开的原因：原来把初值 0x0000 写死在函数体里，没法"每收一页算一次再续算"
 * —— 那样得到的是最后一页的 CRC，不是整包的。4b-2b 边收边算、以及收完
 * 从 W25Q64 回读分块重算，都依赖增量形式。 */
uint16_t Xmodem_CRC16(uint8_t *data, uint32_t datalen)
{
	return Xmodem_CRC16_Update(0x0000, data, datalen);
}

/* CRC16 增量式：给定已有 crc 继续往下算。
 *
 * 算法与原来逐位等价：初值由调用方给、多项式 0x1021、MSB 优先、不反转、不末异或。
 * 与上位机 scripts/crc16.py 的 crc16_update() 必须一致 ——
 * 两边不一致的话，回读校验会永远失败。改这里之前先看那边。 */
uint16_t Xmodem_CRC16_Update(uint16_t crc, uint8_t *data, uint32_t datalen)
{
	uint8_t i;
	uint16_t CRC_Ipoly = 0x1021;
	
	while(datalen --)
	{
		crc = (*data << 8) ^ crc;
		for(i = 0; i < 8; i ++)
		{
			if(crc & 0x8000)
			{
				crc = (crc << 1) ^ CRC_Ipoly;
			}
			else
			{
				crc = (crc << 1);
			}
		}
		data ++;
	}
	return crc;
}












