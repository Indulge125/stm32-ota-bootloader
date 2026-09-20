#include "stm32f10x.h"                  // Device header
#include "iic.h"
#include "Delay.h"
#include "m24c02.h"

/* AT24C02一共能存储256个字节，每一页能存储8个字节 */

/* 按字节写入 */
uint8_t AT24C02_WriteByte(uint8_t addr, uint8_t wdata)
{
	MyIIC_Start();//IIC起始信号
	IIC_Send_Byte(AT24C02_WADDR);//发送写地址
	if(IIC_Receive_Ack() != 0)
	{
		return 1;
	}
	IIC_Send_Byte(addr);//发送要存储的数据的存储地址
	if(IIC_Receive_Ack() != 0)
	{
		return 2;
	}
	IIC_Send_Byte(wdata);//发送要存储的数据
	if(IIC_Receive_Ack() != 0)
	{
		return 3;
	}
	MyIIC_Stop();
	Delay_ms(5);
	return 0;
}

/* 按页写入 AT24C02 8页每字节*/
uint8_t AT24C02_WritePage(uint8_t addr, uint8_t *wdatabuf)
{
	uint8_t i;
	
	MyIIC_Start();//IIC起始信号
	IIC_Send_Byte(AT24C02_WADDR);//发送写地址
	if(IIC_Receive_Ack() != 0)
	{
		return 1;
	}
	IIC_Send_Byte(addr);//发送要存储的数据的存储地址
	if(IIC_Receive_Ack() != 0)
	{
		return 2;
	}
	for(i = 0; i < 8; i ++)
	{
		IIC_Send_Byte(wdatabuf[i]);
		if(IIC_Receive_Ack() != 0)
			{
				return 3+i;
			}
	}
	MyIIC_Stop();
	return 0;
}

/* 读操作 */
/*addr读取的地址，
读取的数据存到rdata数组中，
datalen是数组长度*/
uint8_t AT24C02_ReadData(uint8_t addr, uint8_t *rdatabuf, uint16_t datalen)
{
	MyIIC_Start();//IIC起始信号
	IIC_Send_Byte(AT24C02_WADDR);//发送写地址
	if(IIC_Receive_Ack() != 0)
	{
		return 1;
	}
	IIC_Send_Byte(addr);//发送要读取的数据的存储地址
	if(IIC_Receive_Ack() != 0)
	{
		return 2;
	}
	
	MyIIC_Start();//IIC起始信号
	IIC_Send_Byte(AT24C02_RADDR);//发送读地址
	if(IIC_Receive_Ack() != 0)
	{
		return 3;
	}
	for(uint8_t i = 0; i < datalen  - 1; i ++)
	{
		rdatabuf[i] = IIC_Receive_Byte();
		IIC_Send_Ack(0);//主机应答，所以为0
	}
	rdatabuf[datalen - 1] = IIC_Receive_Byte();
	IIC_Send_Ack(1);//最后一位不需要主机应答，所以为1
	MyIIC_Stop();
	return 0;
}
