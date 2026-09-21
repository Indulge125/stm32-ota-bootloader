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

int main(void)
{
	uint8_t i;					//用于for循环

	OLED_Init();
	MyIIC_Init();				//IIC初始化
	USART1_Init(9600);			//串口初始化
	AT24C02_ReadOTAInfo();		//从24c02读取数据到OTA_Info结构体
	W25Q64_Init();				//W25Q64初始化
	
	U1_printf("OTA_Flag = %x\r\n",OTA_Info.OTA_Flag);		//串口打印OTA_Flag的值
	
	OLED_ShowString(1,1,"MID:   DID:"); 
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
			U1_printf("长度%d字节\r\n", OTA_Info.FileLen[UpDataA.W25Q64_BlockNum]);							//串口1输出信息
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
				U1_printf("A区更新完毕\r\n");		//串口1输出信息
				NVIC_SystemReset();					//重启
			}
			else									//判断长度是否是4的整数倍，不是的话进入else
			{
				U1_printf("长度错误\r\n");			//串口1输出信息
				BootStaFlag &=~ UpData_A_Flag;		//清除UpData_A_Flag标志位
			}
		}
	}
}
