#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "MyFLASH.h"
#include "main.h"
#include "iic.h"
#include "m24c02.h"
#include "boot.h"

load_a load_A;

void BootLoader_Branch(void)
{
	if(BootLoader_Enter(50) == 0)
	{
		if(OTA_Info.OTA_Flag == OTA_SET_FLAG)			//判断OTA_Flag是不是OTA_SET_FLAG定义的值，是的话进入if
		{
			U1_printf("OTA update\r\n");					//串口1输出信息
			BootStaFlag |= UpData_A_Flag;				//置位标志位，表明需要更新A区
			UpDataA.W25Q64_BlockNum = 0;				//W25Q64_BlockNum等于0，表明是OTA要更新A区
		}
		else											//判断OTA_Flag是不是OTA_SET_FLAG定义的值，不是的话进入else
		{
			U1_printf("No OTA update, jumping to region A\r\n");			//串口1输出信息
			LOAD_A(MyFlash_A_Start_Address);			//跳转到A区
		}
	}
	else
	{
		U1_printf("Entering BootLoader command line\r\n");
		BootLoader_Info();
	}
}


uint8_t BootLoader_Enter(uint8_t timeout)
{
	U1_printf("Send lowercase 'w' within %dms to enter BootLoader command line\r\n", timeout * 100);
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
	U1_printf("[1] Erase region A\r\n");	
	U1_printf("[2] Download app to region A via Xmodem\r\n");	
	U1_printf("[3] Set OTA version\r\n");	
	U1_printf("[4] Query OTA version\r\n");	
	U1_printf("[5] Download firmware to external flash\r\n");	
	U1_printf("[6] Load firmware from external flash\r\n");	
	U1_printf("[7] Reset\r\n");	
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
}

void BootLoader_Clear(void)
{
	USART_DeInit(USART1);
	GPIO_DeInit(GPIOA);
	GPIO_DeInit(GPIOB);
}

