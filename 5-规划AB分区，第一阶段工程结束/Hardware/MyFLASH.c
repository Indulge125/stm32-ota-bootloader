#include "stm32f10x.h"                  // Device header

void MyFlash_EraseFlash(uint32_t Start_Address, uint32_t num)
{
	uint16_t i;
	
	FLASH_Unlock();
	for(i = 0; i < num; i ++)
	{
		FLASH_ErasePage((0x08000000 + Start_Address * 1024) + (i * 1024));
	}
	FLASH_Lock();
}


void MyFlash_WriteFlash(uint32_t Address, uint32_t *Data, uint32_t num)
{
	FLASH_Unlock();
	while(num)
	{
		FLASH_ProgramWord(Address, *Data);
		num -= 4;
		Address += 4;
		Data ++;
	}
	FLASH_Lock();
}
	
