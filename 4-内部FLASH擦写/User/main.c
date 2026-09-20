#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "MyFLASH.h"

uint32_t buff[1024];
uint32_t i;

int main(void)
{
	OLED_Init();

	USART1_Init(9600);
	
	OLED_ShowString(1,1,"MID:   DID:"); 

	for(i = 0; i < 1024; i ++)
	{
		buff[i] =0x12345678;
	}
	
	MyFlash_EraseFlash(60, 4);
	
	MyFlash_WriteFlash(60 * 1024 + 0x08000000, buff, 1024 * 4);
	
	for(i = 0; i < 1024; i ++)
	{
		U1_printf("%x\r\n", *(uint32_t *)((60 * 1024 + 0x08000000) + (i * 4)));
	}

	
	while(1)
	{
		
	}
}
