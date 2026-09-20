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
	
	U1_printf("OTA_Flag = %x\r\n",OTA_Info.OTA_Flag);
	
	OLED_ShowString(1,1,"MID:   DID:"); 
	BootLoader_Branch();		//分支判断
	
	while(1)
	{
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
					MyFlash_WriteFlash(MyFlash_StartAddress + i * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, MyFlash_Page_Size);		//写入到单片机A区相应的扇区
				}
				if(OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024 != 0)									//判断是否还有不足一个完整扇区的数据，有的话进入if
				{
					W25Q64_ReadData(i * 1024 + UpDataA.W25Q64_BlockNum * 64 * 1024, UpDataA.UpDataBuff, OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024);					//从w25q64读取不足一个完整扇区的数据
					MyFlash_WriteFlash(MyFlash_StartAddress + i * MyFlash_Page_Size, (uint32_t *)UpDataA.UpDataBuff, OTA_Info.FileLen[UpDataA.W25Q64_BlockNum] % 1024);		//然后写入单片机A区相应的扇区
				}
				if(UpDataA.W25Q64_BlockNum == 0)	//如果w25q64_BlockNum是0，表示是OTA更新A区，进入if
				{
					OTA_Info.OTA_Flag = 0;			//设置OTA_Flag，只要不是OTA_SET_FLAG定义的值即可
					AT24C02_WriteOTAInfo();			//写入24c02中保存
				}
				NVIC_SystemReset();					//重启
			}
			else									//判断长度是否是4的整数倍，不是的话进入else
			{
				U1_printf("长度错误\r\n");			//串口1输出信息
				BootStaFlag &=~ UpData_A_Flag;		//清除UpData_A_Flag标志位，取反清除标志位，否则while又进循环了
			}
		}
	}
}
