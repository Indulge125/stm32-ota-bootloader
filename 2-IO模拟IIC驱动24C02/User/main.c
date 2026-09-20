#include "stm32f10x.h"                  // Device header
#include "Delay.h"
//#include "OLED.h"
#include "usart.h"
#include "iic.h"
#include "m24c02.h"

uint8_t buff[256];
uint8_t buff1[8] = {10,2,3,5,6,7,8,4};

int main(void)
{
	uint16_t i;
	
//	OLED_Init();
//	OLED_ShowString(1,1,"XZY"); 
	USART1_Init(9600);
	MyIIC_Init();
	
//	for(i = 0; i <256; i ++)
//	{
//		AT24C02_WriteByte(i, 255 - i);
//		Delay_ms(5);
//	}
	
	for(i = 0; i <32; i ++)
	{
		AT24C02_WritePage(i * 8, buff1);
		Delay_ms(5);
	}
		
	AT24C02_ReadData(0, buff, 256);
	
	for(i = 0; i <256; i ++)
	{
		U1_printf("数据%d = %x\r\n", i, buff[i]);
	}
	
	while(1)
	{
		
	}
}
