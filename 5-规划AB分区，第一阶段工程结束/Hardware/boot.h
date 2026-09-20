#ifndef __BOOT_H
#define __BOOT_H

typedef void (*load_a)(void);

void BootLoader_Branch(void);
uint8_t BootLoader_Enter(uint8_t timeout);
void BootLoader_Info(void);
__asm void MSR_SP(uint32_t address);
void LOAD_A(uint32_t address);
void BootLoader_Clear(void);


#endif

