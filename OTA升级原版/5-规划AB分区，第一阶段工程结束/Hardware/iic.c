#include "stm32f10x.h"                  // Device header
#include "iic.h"
#include "Delay.h"

void MyIIC_W_SCL(uint8_t BitValue)
{
	GPIO_WriteBit(GPIOB, GPIO_Pin_6, (BitAction)BitValue);
	Delay_us(10);
}

void MyIIC_W_SDA(uint8_t BitValue)
{
	GPIO_WriteBit(GPIOB, GPIO_Pin_7, (BitAction)BitValue);
	Delay_us(10);
}

uint8_t MyIIC_R_SDA(void)
{
	uint8_t BitValue;
	BitValue = GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7);
	Delay_us(10);
	return BitValue;
}

/* IIC初始化 */
void MyIIC_Init(void)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
	
	GPIO_InitTypeDef GPIO_InitStructure;
 	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
 	GPIO_Init(GPIOB, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
 	GPIO_Init(GPIOB, &GPIO_InitStructure);
	
	GPIO_SetBits(GPIOB, GPIO_Pin_6 | GPIO_Pin_7);
	
}

/* IIC起始信号 */
void MyIIC_Start(void)
{
	MyIIC_W_SDA(1);
	MyIIC_W_SCL(1);
	MyIIC_W_SDA(0);
	MyIIC_W_SCL(0);
}

/* IIC终止信号 */
void MyIIC_Stop(void)
{	
	MyIIC_W_SDA(0);
	MyIIC_W_SCL(1);
	MyIIC_W_SDA(1);
}

/* 主机发送数据 */
void IIC_Send_Byte(uint8_t Byte)
{
	uint8_t i;
	
	for(i = 0; i< 8; i ++)
	{
		MyIIC_W_SDA(Byte & (0x80 >> i));
		MyIIC_W_SCL(1);
		MyIIC_W_SCL(0);
	}
}


/* 主机接收数据 */
uint8_t IIC_Receive_Byte(void)
{
	uint8_t i, Byte = 0x00;
	
	MyIIC_W_SDA(1);
	for(i = 0; i< 8; i++)
	{
		MyIIC_W_SCL(1);
		if(MyIIC_R_SDA() == 1)
		{
			Byte |= (0x80 >> i);
		}
		MyIIC_W_SCL(0);
	}
	return Byte;
}

/* 发送应答 */
void IIC_Send_Ack(uint8_t AckBit)
{
		MyIIC_W_SDA(AckBit);
		MyIIC_W_SCL(1);
		MyIIC_W_SCL(0);
}

/* 接收应答 */
uint8_t IIC_Receive_Ack(void)
{
	uint8_t AckBit = 0x00;
	
	MyIIC_W_SDA(1);
	MyIIC_W_SCL(1);
	AckBit = MyIIC_R_SDA();
	MyIIC_W_SCL(0);
	return AckBit;
}
