#include "stm32f10x.h"                  // Device header
#include "iic.h"
#include "Delay.h"

/* ================= AT24C02 软件 IIC 引脚配置 =================
 * 原用 PB6(SCL)/PB7(SDA)，但这两个脚被 OLED 模块物理遮挡，改用 PB10/PB11。
 * PB10/PB11 是纯 GPIO，本工程中未被任何外设占用，且与 OLED(PB8/PB9) 不在同一排排针。
 * 如需再换脚，只改下面 4 个宏即可；换到 GPIOA 时把 IIC_GPIO_CLK 一并改成
 * RCC_APB2Periph_GPIOA。
 *
 * 选脚请避开：
 *   PA4~PA7      W25Q64 软件SPI
 *   PA9/PA10     USART1
 *   PB8/PB9      OLED 软件IIC
 *   PA13/PA14    SWD 调试口（保留，否则无法烧录/仿真）
 *   PB3/PB4/PA15 JTAG 脚，当 GPIO 用需要先调用
 *                GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE)
 *   PA11/PA12    USB DM/DP（以后要用 USB 需避开）
 *
 * 注意：本工程 Key.c 里把 PB11 声明成了按键，但 Key_Init() 从未被调用（死代码），
 *       所以当前不冲突。若以后要启用按键，必须先把按键挪到别的脚，否则会和
 *       AT24C02 的 SDA 打架。
 */
#define IIC_SCL_PORT        GPIOB
#define IIC_SCL_PIN         GPIO_Pin_10
#define IIC_SDA_PORT        GPIOB
#define IIC_SDA_PIN         GPIO_Pin_11
#define IIC_GPIO_CLK        RCC_APB2Periph_GPIOB

/* 写SCL电平 */
void MyIIC_W_SCL(uint8_t BitValue)
{
	GPIO_WriteBit(IIC_SCL_PORT, IIC_SCL_PIN, (BitAction)BitValue);
	Delay_us(10);
}

/* 写SDA电平 */
void MyIIC_W_SDA(uint8_t BitValue)
{
	GPIO_WriteBit(IIC_SDA_PORT, IIC_SDA_PIN, (BitAction)BitValue);
	Delay_us(10);
}

/* 读SDA电平 */
uint8_t MyIIC_R_SDA(void)
{
	uint8_t BitValue;
	BitValue = GPIO_ReadInputDataBit(IIC_SDA_PORT, IIC_SDA_PIN);
	Delay_us(10);
	return BitValue;
}

/* IIC初始化：SCL/SDA 都配成开漏输出。I2C 必须用开漏；
 * 开漏模式下输入通路仍然有效，所以 R_SDA 能读回引脚真实电平 */
void MyIIC_Init(void)
{
	RCC_APB2PeriphClockCmd(IIC_GPIO_CLK, ENABLE);
	
	GPIO_InitTypeDef GPIO_InitStructure;
 	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Pin = IIC_SCL_PIN;
 	GPIO_Init(IIC_SCL_PORT, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin = IIC_SDA_PIN;
 	GPIO_Init(IIC_SDA_PORT, &GPIO_InitStructure);
	
	GPIO_SetBits(IIC_SCL_PORT, IIC_SCL_PIN);		/* 空闲时 SCL、SDA 都为高 */
	GPIO_SetBits(IIC_SDA_PORT, IIC_SDA_PIN);
}

/* ===== 以下为原有 IIC 时序函数，逻辑未做任何改动 ===== */

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

/* 发送一个字节 */
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

/* 接收一个字节 */
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
