#ifndef __MYFLASH_H
#define __MYFLASH_H

void MyFlash_EraseFlash(uint32_t Start_Address, uint32_t num);
void MyFlash_WriteFlash(uint32_t Address, uint32_t *Data, uint32_t num);


#endif
