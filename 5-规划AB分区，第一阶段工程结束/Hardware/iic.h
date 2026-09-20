#ifndef __IIC_H
#define __IIC_H

void MyIIC_Init(void);
void MyIIC_Start(void);
void MyIIC_Stop(void);
void IIC_Send_Byte(uint8_t Byte);
uint8_t IIC_Receive_Byte(void);
void IIC_Send_Ack(uint8_t AckBit);
uint8_t IIC_Receive_Ack(void);

#endif

