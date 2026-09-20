#ifndef __IIC_H
#define __IIC_H

void MyIIC_Init(void);
void MyIIC_Start(void);
void MyIIC_Stop(void);
void IIC_Send_Byte(uint8_t Byte);
uint8_t IIC_Receive_Byte(void);
void IIC_Send_Ack(uint8_t AckBit);
uint8_t IIC_Receive_Ack(void);

/* 诊断辅助（只在自检失败时用） */
uint8_t MyIIC_ReadIdleLevel(void);
uint8_t MyIIC_ProbeExternalPullup(void);
uint8_t MyIIC_ProbeForcedLow(void);
void    MyIIC_SwapPins(void);
uint8_t MyIIC_TestShort(void);
uint8_t MyIIC_ScanAddr(uint8_t *found, uint8_t max);

#endif
