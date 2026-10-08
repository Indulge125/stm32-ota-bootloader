#ifndef __BOOT_H
#define __BOOT_H

typedef void (*load_a)(void);

void BootLoader_Branch(void);
uint8_t BootLoader_Enter(uint8_t timeout);
void BootLoader_Info(void);
__asm void MSR_SP(uint32_t address);
void LOAD_A(uint32_t address);
void BootLoader_Clear(void);
void BootLoader_Event(uint8_t *data, uint16_t datalen);

uint16_t Xmodem_CRC16(uint8_t *data, uint32_t datalen);
/* 增量式：给定已有 crc 继续算。4b-2b 边收边算、以及从 W25Q64 回读分块重算都靠它。 */
uint16_t Xmodem_CRC16_Update(uint16_t crc, uint8_t *data, uint32_t datalen);

#endif

