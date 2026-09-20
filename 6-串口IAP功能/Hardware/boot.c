#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "MyFLASH.h"
#include "main.h"
#include "iic.h"
#include "m24c02.h"
#include "boot.h"
#include "W25Q64.h"

load_a load_A;

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
	USART_DeInit(USART1);
	GPIO_DeInit(GPIOA);
	GPIO_DeInit(GPIOB);
}

/* Xmodem的CRC16校验 */
uint16_t Xmodem_CRC16(uint8_t *data, uint16_t datalen)
{
	uint8_t i;
	uint16_t CRC_Init = 0x0000;
	uint16_t CRC_Ipoly = 0x1021;
	
	while(datalen --)
	{
		CRC_Init = (*data << 8) ^ CRC_Init;
		for(i = 0; i < 8; i ++)
		{
			if(CRC_Init & 0x8000)
			{
				CRC_Init = (CRC_Init << 1) ^ CRC_Ipoly;
			}
			else
			{
				CRC_Init = (CRC_Init << 1);
			}
		}
		data ++;
	}
	return CRC_Init;
}












