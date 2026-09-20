#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "iic.h"
#include "m24c02.h"
#include "W25Q64.h"

uint8_t MID;
uint16_t DID;
uint8_t wdata[256];
uint8_t rdata[256];

int main(void)
{
	uint16_t i, j;
	
	OLED_Init();
	W25Q64_Init();
	USART1_Init(9600);
	
	OLED_ShowString(1,1,"MID:   DID:"); 
	W25Q64_ReadID(&MID, &DID);
	
	OLED_ShowHexNum(1, 5, MID, 2);
	OLED_ShowHexNum(1, 12, DID, 4);
	
	W25Q64_Erase64K(0);
	
	for(i = 0; i < 256; i++)
	{
		for(j = 0; j < 256; j++)
		{
			wdata[j] = i;
		}
		W25Q64_PageProgram(i, wdata, 256);
	}
	
	Delay_ms(50);
	
	for(i = 0; i < 256; i ++)
	{
		W25Q64_ReadData(i * 256, rdata, 256);
		for(j = 0; j < 256; j++)
		{
			U1_printf("Addr%d = %x\r\n", i * 256 + j, rdata[j]);
		}
	}

	
	while(1)
	{
		
	}
}
