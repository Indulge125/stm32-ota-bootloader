#include "stm32f10x.h"                  // Device header
#include "boot.h"
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "MyFLASH.h"
#include "main.h"
#include "iic.h"
#include "m24c02.h"
#include "W25Q64.h"


OTA_InfoCB OTA_Info;			//保存在24c02内的OTA信息相关的结构体
UpDataA_CB UpDataA;				//A区更新要用到的结构体
uint32_t BootStaFlag;			//记录全局状态新标志位

/* 把一条线的接线状态翻成人话，给串口日志用。
 * 判据（两个探针配合）：
 *   探到外部上拉          -> 正常
 *   探不到上拉但拉得起来  -> 悬空（线没接上）
 *   探不到上拉且拉不起来  -> 被硬拉在地（接到了 GND） */
static const char *LineState(uint8_t pull, uint8_t forced, uint8_t mask)
{
	if((pull & mask) != 0)
	{
		return "OK";
	}
	if((forced & mask) != 0)
	{
		return "tied to GND";
	}
	return "floating(open)";
}

/* ================= AT24C02 专项诊断 =================  */
/* 自检报 ERR 时才调用，把笼统的「没应答」细分成可操作的情况：
 *   ① 线没接好         -> 探不到外部上拉（悬空），或连内部上拉都拉不起来（短到 GND）
 *   ② SCL/SDA 接反     -> 把引脚角色对调后再扫，扫到器件
 *   ③ 两根线互相短接   -> 拉低一根，另一根跟着变低
 *   ④ 器件在别的地址   -> 扫描在 0xA2 / 0xA4 / ... 上找到器件（A0/A1/A2 接错）
 *   ⑤ 芯片本身不工作   -> 线都正常、没短接、没接反，但任何地址都不应答
 * 摘要显示在 OLED 第 2~4 行，完整解读打印到串口。
 */
static void AT24C02_Diag(void)
{
	uint8_t found[8];
	uint8_t n, n2, pins, pull, forced, shrt;
	uint8_t w_ret, r_ret;
	uint8_t ee_backup = 0, ee_read = 0;
	uint8_t i, bad;

	/* ① 释放总线后读回两条线的真实电平：正常应都是 1 */
	pins = MyIIC_ReadIdleLevel();

	/* ② 探测两条线上有没有外部上拉电阻（判定模块是否真的接在这两个脚上） */
	pull = MyIIC_ProbeExternalPullup();

	/* ③ 反向探测：哪条线连内部上拉都拉不起来（= 被硬拉在地） */
	forced = MyIIC_ProbeForcedLow();

	/* ④ 扫描全部 7 位地址（约 32ms） */
	n = MyIIC_ScanAddr(found, 8);

	/* ⑤ 常规扫描一无所获时，把 SCL/SDA 角色对调再扫一次 —— 判断两根线是否接反 */
	n2 = 0;
	if(n == 0)
	{
		MyIIC_SwapPins();
		n2 = MyIIC_ScanAddr(found, 8);
		MyIIC_SwapPins();						/* 立刻换回来，别影响后面的读写测试 */
	}

	/* ⑥ 查两根线之间是否短接 */
	shrt = MyIIC_TestShort();

	/* ⑦ 在正确地址 0xA0 上重做一次，把返回码完整留下来 */
	AT24C02_ReadData(0xF0, &ee_backup, 1);
	w_ret = AT24C02_WriteByte(0xF0, 0x5A);
	r_ret = AT24C02_ReadData(0xF0, &ee_read, 1);
	if(w_ret == 0)
	{
		AT24C02_WriteByte(0xF0, ee_backup);		/* 只有写通了才需要还原 */
	}

	/* ⑧ OLED 摘要 */
	OLED_ShowString(2, 1, "Idle:");
	OLED_ShowNum(2, 6, pins, 1);				/* bit1=SCL bit0=SDA，应为 3 */
	OLED_ShowString(2, 8, "Prb:");
	OLED_ShowNum(2, 12, pull, 1);				/* bit1=SCL bit0=SDA 探到外部上拉，应为 3 */

	if(n > 0)
	{
		OLED_ShowString(3, 1, "Scan:");
		OLED_ShowNum(3, 6, n, 1);
		OLED_ShowString(3, 8, "0x");
		OLED_ShowHexNum(3, 10, found[0], 2);		/* 器件实际应答的地址 —— 最该看的值 */
	}
	else
	{
		OLED_ShowString(3, 1, "Scan:0 Swp:");
		OLED_ShowNum(3, 12, n2, 1);				/* 对调引脚角色后扫到的器件数 */
		if(n2 > 0)
		{
			OLED_ShowHexNum(3, 14, found[0], 2);
		}
	}

	OLED_ShowString(4, 1, "W:");
	OLED_ShowNum(4, 3, w_ret, 1);
	OLED_ShowString(4, 5, "R:");
	OLED_ShowNum(4, 7, r_ret, 1);
	OLED_ShowString(4, 9, "G:");
	OLED_ShowNum(4, 11, forced, 1);				/* 非 0 表示有线上连内部上拉都拉不起来 */

	/* ⑨ 串口详解 */
	U1_printf("\r\n----- AT24C02 Diagnostic -----\r\n");
	U1_printf("Idle level  : SCL=%d SDA=%d  (both should read 1)\r\n",
	          (pins >> 1) & 1, pins & 1);
	U1_printf("Pull-up probe: SCL=%d SDA=%d  (pin set to input pull-down, then read;\r\n",
	          (pull >> 1) & 1, pull & 1);
	U1_printf("            1 = an external 4.7k pull-up beats the internal 40k pull-down)\r\n");
	U1_printf("GND probe   : SCL=%d SDA=%d  (input pull-up instead; 0 = line hard-pulled to GND)\r\n",
	          (forced >> 1) & 1, forced & 1);
	U1_printf("Wiring      : SCL(PB10)=%s  SDA(PB11)=%s\r\n",
	          LineState(pull, forced, 0x02), LineState(pull, forced, 0x01));
	U1_printf("Address scan: %d device(s) responded\r\n", n);
	for(i = 0; (i < n) && (i < 8); i ++)
	{
		if(found[i] == AT24C02_WADDR)
		{
			U1_printf("  0x%02X <-- correct address of AT24C02\r\n", found[i]);
		}
		else
		{
			U1_printf("  0x%02X\r\n", found[i]);
		}
	}
	U1_printf("Swapped scan: %d device(s) responded (SCL/SDA roles exchanged)\r\n", n2);
	if(shrt == 0x04)
	{
		U1_printf("Short test  : invalid (a line did not go high when released)\r\n");
	}
	else
	{
		U1_printf("Short test  : %d  (bit1=SDA follows SCL low, bit0=SCL follows SDA low; 0=no short)\r\n", shrt);
	}
	U1_printf("Write test  : WriteByte=%d  (1=no ACK on dev addr, 2=on word addr, 3=on data; 0=OK)\r\n", w_ret);
	U1_printf("Read test   : ReadData =%d  (0=OK)\r\n", r_ret);
	U1_printf("Read-back   : wrote 0x5A, read 0x%02X  (expect 0x5A)\r\n", ee_read);

	/* ⑩ 结论 —— 按「最有指向性」的顺序判 */
	bad = 0;
	if((pull & 0x02) == 0)
	{
		bad = 1;
		if((forced & 0x02) != 0)
		{
			U1_printf("==> SCL(PB10) is hard-pulled to GND: this wire goes to GND\r\n");
		}
		else
		{
			U1_printf("==> SCL(PB10) is floating: module SCL is not connected to PB10\r\n");
		}
	}
	if((pull & 0x01) == 0)
	{
		bad = 1;
		if((forced & 0x01) != 0)
		{
			U1_printf("==> SDA(PB11) is hard-pulled to GND: this wire goes to GND\r\n");
		}
		else
		{
			U1_printf("==> SDA(PB11) is floating: module SDA is not connected to PB11\r\n");
		}
	}

	if(bad != 0)
	{
		U1_printf("    With a bad wire the scan result above is NOT trustworthy --\r\n");
		U1_printf("    a floating line reads random values and causes false ACKs. Fix wiring first.\r\n");
		U1_printf("    Check: jumper fully seated, on the PB10/PB11 row, not on a GND pin by mistake\r\n");
	}
	else if(n > 0)
	{
		if(found[0] == AT24C02_WADDR)
		{
			if((w_ret == 0) && (r_ret == 0) && (ee_read != 0x5A))
			{
				U1_printf("==> Address OK, write and read both report no error, but data does not stick --\r\n");
				U1_printf("    classic sign of WP held HIGH: the chip still ACKs, yet nothing is stored.\r\n");
				U1_printf("    Move the WP jumper to the GND side too (3-pin block: jumper the right pair).\r\n");
			}
			else
			{
				U1_printf("==> Device answers at the correct address 0xA0; see W/R codes if R/W still fails\r\n");
			}
		}
		else
		{
			U1_printf("==> Device is at 0x%02X, not 0xA0: A0/A1/A2 are not all tied to GND\r\n", found[0]);
			U1_printf("    (all three of A0/A1/A2 must be tied to GND; only then is the address 0xA0)\r\n");
		}
	}
	else if(n2 > 0)
	{
		U1_printf("==> Found 0x%02X after swapping SCL/SDA roles: the two wires are reversed\r\n", found[0]);
		U1_printf("    Swap the two wires (module SCL -> PB10, module SDA -> PB11)\r\n");
	}
	else if((shrt & 0x03) != 0)
	{
		U1_printf("==> SCL and SDA are shorted (Sh=%d): one line drags the other low\r\n", shrt & 0x03);
		U1_printf("    Check for a jumper on a wrong adjacent pin, or a solder bridge on the module\r\n");
	}
	else
	{
		U1_printf("==> Both wires are connected, no short, not reversed -- yet nothing answers.\r\n");
		U1_printf("    The problem is the AT24C02 chip itself:\r\n");
		U1_printf("    1) Is the chip fully seated in the 8-pin socket? (bad contact is common)\r\n");
		U1_printf("    2) Orientation: the notch/dot end must match the notch on the socket\r\n");
		U1_printf("    3) Compare each socket position with the silkscreen -- off by one pin?\r\n");
		U1_printf("    4) Try another AT24C02 (the chip may be damaged)\r\n");
	}
	U1_printf("------------------------\r\n");
}

/* ================= 上电硬件自检 =================  */
/* 目的：进 BootLoader 分支前，用最小代价确认两颗外部存储的接线/供电是否正常。
 * 没有这段代码时，W25Q64 或 AT24C02 没接好会「静默失败」：
 *   - 软件 IIC 收不到 ACK 只会返回错误码；软件 SPI 读回全 0xFF；
 *   - 程序照样往下跑，直到命令 5/6 写完才发现数据进了空气。
 *
 * 自检内容：
 *   1) W25Q64 读 JEDEC ID：MID 应为 0xEF，DID 应为 0x4017（W25Q64JV）
 *   2) AT24C02 读写回环：在 0xF0 写入 0x5A 再读回比较（先备份、测完还原）
 *      失败时自动转 AT24C02_Diag() 做细分定位
 *
 * 为什么用 0xF0：OTA_InfoCB 占 0x00~0x4F（sizeof=80 字节），0xF0 不在其中，
 *                不会破坏 OTA 元数据（OTA_Flag / FileLen / OTA_Ver）。
 *
 * 寿命核算：AT24C02 擦写寿命 100 万次，本测试每次上电写 2 字节，
 *           要 50 万次上电才到寿命，调试期完全不用担心。
 *
 * 注意：W25Q64 只读 ID、不做写测试 —— NOR Flash 写前必须先擦除，
 *       做写测试会破坏暂存的固件。写通路留给命令 5/6 的真实流程去验证。
 */
static void HW_SelfTest(void)
{
	uint8_t  MID;
	uint16_t DID;
	uint8_t  ee_backup = 0, ee_read = 0, ee_ok;

	/* ---- 1. W25Q64 读 JEDEC ID ---- */
	W25Q64_ReadID(&MID, &DID);

	OLED_ShowString(1, 1, "MID:");
	OLED_ShowHexNum(1, 5, MID, 2);					/* 正常应显示 EF */
	OLED_ShowString(1, 8, "DID:");
	OLED_ShowHexNum(1, 12, DID, 4);				/* 正常应显示 4017 */

	U1_printf("Self-test: W25Q64 MID=0x%02X DID=0x%04X\r\n", MID, DID);

	/* ---- 2. AT24C02 读写回环 ---- */
	AT24C02_ReadData(0xF0, &ee_backup, 1);			/* 先备份原值 */
	ee_ok = (AT24C02_WriteByte(0xF0, 0x5A) == 0);	/* 写入测试值；返回 0 表示每一步都收到了 ACK */
	if(ee_ok)
	{
		AT24C02_ReadData(0xF0, &ee_read, 1);		/* 读回 */
		ee_ok = (ee_read == 0x5A);					/* 比较 */
	}
	AT24C02_WriteByte(0xF0, ee_backup);				/* 还原现场，不留痕迹 */

	if(ee_ok)
	{
		OLED_ShowString(2, 1, "EE24C02:OK");
		OLED_ShowString(3, 1, "Flag:");
		OLED_ShowHexNum(3, 6, OTA_Info.OTA_Flag, 8);
		U1_printf("Self-test: AT24C02=OK, OTA_Flag=0x%08X\r\n", OTA_Info.OTA_Flag);
	}
	else
	{
		/* 失败才跑完整诊断 —— 正常启动不用付扫描那 32ms */
		AT24C02_Diag();
	}
}


int main(void)
{
	uint8_t i;					//用于for循环

	OLED_Init();
	MyIIC_Init();				//IIC初始化
	USART1_Init(9600);			//串口初始化
	AT24C02_ReadOTAInfo();		//从24c02读取数据到OTA_Info结构体
	W25Q64_Init();				//W25Q64初始化
	
	U1_printf("OTA_Flag = %x\r\n",OTA_Info.OTA_Flag);		//串口打印OTA_Flag的值
	
	HW_SelfTest();				/* 上电硬件自检：确认两颗外部存储通信正常 */
	BootLoader_Branch();		//分支判断
	
	while(1)
	{
		Delay_ms(10);
		
		/* 处理串口接收缓冲区的数据 */
		if(U1CB.URxDataOut != U1CB.URxDataIn)																//IN和OUT不相等的时候进入if，说明缓冲区有数据了
		{
			BootLoader_Event(U1CB.URxDataOut->start, U1CB.URxDataOut->end - U1CB.URxDataOut->start + 1);	//调用BootLoader_Event处理数据
			U1CB.URxDataOut ++;																				//Out指针后移一位
			if(U1CB.URxDataOut == U1CB.URxDataEnd)		 													//判断是否到达END
			{
				U1CB.URxDataOut = &U1CB.URxDataPtr[0];														//重新回到数组0号成员
			}
		}
		
		/* Xmodem协议发送C，标志着通信的开始 */
		if(BootStaFlag & IAP_XMODEMC_FLAG)		//如果IAP_XMODEMC_FLAG标志位置位，表明需要发送C
		{
			if(UpDataA.XmodemTimer >= 100)		//计算间隔时间，到时进入if
			{
				U1_printf("C");					//发送C
				UpDataA.XmodemTimer = 0;		//清除计算间隔时间的变量
			}
			UpDataA.XmodemTimer ++;				//计算间隔时间的变量++
		}
		
		/* UpData_A_Flag 置位，表明需要更新A区 */
		if(BootStaFlag & UpData_A_Flag)
		{
			U1_printf("Length: %d bytes\r\n", OTA_Info.FileLen[UpDataA.W25Q64_BlockNum]);							//串口1输出信息
			if(OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 4 == 0)											//判断长度是否是4的整数，是的话进入if
			{
				MyFlash_EraseFlash(MyFlash_A_Start_Page, MyFlash_A_Page_Num);								//擦除A区FLASH
				for(i = 0; i < OTA_Info.FileLen[UpDataA.W25Q64_BlockNum]/MyFlash_Page_Size; i ++)			//每次读写一个扇区数据，使用for循环，写入整数个扇区
				{
					W25Q64_ReadData(i * 1024 + UpDataA.W25Q64_BlockNum * 64 * 1024, UpDataA.UpDataBuff, MyFlash_Page_Size);						//先从w25q64读取一个单片机扇区的数据
					MyFlash_WriteFlash(MyFlash_A_Start_Address + i * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, MyFlash_Page_Size);		//写入到单片机A区相应的扇区
				}
				if(OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024 != 0)									//判断是否还有不足一个完整扇区的数据，有的话进入if
				{
					W25Q64_ReadData(i * 1024 + UpDataA.W25Q64_BlockNum * 64 * 1024, UpDataA.UpDataBuff, OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024);					//从w25q64读取不足一个完整扇区的数据
					MyFlash_WriteFlash(MyFlash_A_Start_Address + i * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024);		//然后写入单片机A区相应的扇区
				}
				if(UpDataA.W25Q64_BlockNum == 0)	//如果w25q64_BlockNum是0，表示是OTA更新A区，进入if
				{
					OTA_Info.OTA_Flag = 0;			//设置OTA_Flag，只要不是OTA_SET_FLAG定义的值即可
					AT24C02_WriteOTAInfo();			//写入24c02中保存
				}
				U1_printf("Region A update complete\r\n");		//串口1输出信息
				NVIC_SystemReset();					//重启
			}
			else									//判断长度是否是4的整数倍，不是的话进入else
			{
				U1_printf("Length error\r\n");			//串口1输出信息
				BootStaFlag &=~ UpData_A_Flag;		//清除UpData_A_Flag标志位
			}
		}
	}
}
