#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"


int main(void)
{
	OLED_Init();
	OLED_ShowString(1,1,"XZY"); 
	USART1_Init(9600);
	U1_printf("%d %c %x\r\n",0x30, 0x30, 0x30);
	while(1)
	{
		if(U1CB.URxDataOut != U1CB.URxDataIn)
		{
			U1_printf("Received %d bytes\r\n",U1CB.URxDataOut->end - U1CB.URxDataOut->start + 1);
			for(uint16_t i = 0; i <U1CB.URxDataOut->end - U1CB.URxDataOut->start + 1; i ++)
			{
				U1_printf("%c", U1CB.URxDataOut->start[i]);
			}
			U1_printf("\r\n");
			U1CB.URxDataOut ++;							 //Out指针后移一位
			if(U1CB.URxDataOut == U1CB.URxDataEnd)		 //判断是否到达END
			{
				U1CB.URxDataOut = &U1CB.URxDataPtr[0];
			}
		}
	}
}

/*OUT，IN都是指向的创建的同一个结构体类型（有start和end）的数组，
接收数据的时候IN在改变数组中其中一个成员的start和end的指向，
OUT和IN所指向的数组是同一系列。*/
