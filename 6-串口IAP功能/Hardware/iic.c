#include "stm32f10x.h"                  // Device header
#include "iic.h"
#include "Delay.h"

/* ================= AT24C02 软件 IIC 引脚配置 =================
 * 原用 PB6(SCL)/PB7(SDA)，但 PB6/PB7 被 OLED 的 GND/VCC 占作电源通路，
 * 改用 PB10/PB11。PB10/PB11 是纯 GPIO，本工程中未被任何外设占用，
 * 且与 OLED 的软件 IIC(PB8/PB9) 不在同一排排针。
 * 如需再换脚，只改下面 4 个宏即可；换到 GPIOA 时把 IIC_GPIO_CLK 一并改成
 * RCC_APB2Periph_GPIOA。
 *
 * 选脚请避开：
 *   PA4~PA7      W25Q64 软件SPI
 *   PA9/PA10     USART1
 *   PB8/PB9      OLED 软件IIC
 *   PB6/PB7      OLED 的 GND / VCC（是电源通路，不能当 GPIO 用）
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

/* 运行期可变的引脚号。正常通信时恒等于上面的宏；只有诊断会用 MyIIC_SwapPins()
 * 把 SCL/SDA 临时对调，用来判断硬件上两根线是不是接反了。
 * 用变量而不是宏，就不用为「对调后再扫一次」复制一整套位操作代码。
 * 本文件所有函数都在主循环/初始化上下文调用，不存在并发访问。 */
static uint16_t s_scl_pin = IIC_SCL_PIN;
static uint16_t s_sda_pin = IIC_SDA_PIN;

/* 诊断用：对调 SCL/SDA 的角色（调用偶数次等于还原） */
void MyIIC_SwapPins(void)
{
	uint16_t t;

	t = s_scl_pin;
	s_scl_pin = s_sda_pin;
	s_sda_pin = t;
}

/* 写SCL电平 */
void MyIIC_W_SCL(uint8_t BitValue)
{
	GPIO_WriteBit(IIC_SCL_PORT, s_scl_pin, (BitAction)BitValue);
	Delay_us(10);
}

/* 写SDA电平 */
void MyIIC_W_SDA(uint8_t BitValue)
{
	GPIO_WriteBit(IIC_SDA_PORT, s_sda_pin, (BitAction)BitValue);
	Delay_us(10);
}

/* 读SDA电平 */
uint8_t MyIIC_R_SDA(void)
{
	uint8_t BitValue;
	BitValue = GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin);
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

	s_scl_pin = IIC_SCL_PIN;		/* 复位到默认映射 */
	s_sda_pin = IIC_SDA_PIN;

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

/* ================= 诊断辅助函数 =================
 * 只在自检失败时被调用，正常通信流程不依赖它们。
 *
 * 三个探针配合起来，能把「一条线的接线状态」判成三种：
 *   MyIIC_ProbeExternalPullup() 读到 1          -> 正常：线上挂着模块的上拉电阻
 *   外部上拉=0 且 MyIIC_ProbeForcedLow() 读到 1 -> 被硬拉在地（接错到 GND）
 *   外部上拉=0 且 拉不起来=0                    -> 悬空（线没接上）
 */

/* 释放总线（把 SCL/SDA 都置高）后，读回两条线的真实电平。
 * 返回值：bit1 = SCL，bit0 = SDA，正常都应为 1。
 * 原理：开漏输出模式下 GPIO 的输入通路仍然有效，所以能读回引脚真实电平。
 * 用途：哪条读到 0，说明它被拉死了（短路 / 器件卡住 / 没上拉）。
 * 把「释放」和「读回」合成一个接口，是为了避免调用者忘记先释放总线。 */
uint8_t MyIIC_ReadIdleLevel(void)
{
	uint8_t v = 0;

	MyIIC_W_SCL(1);			/* 释放总线：两条线都置高 */
	MyIIC_W_SDA(1);
	Delay_us(10);			/* 等线上电平稳定 */

	if(GPIO_ReadInputDataBit(IIC_SCL_PORT, s_scl_pin) != 0)
	{
		v |= 0x02;
	}
	if(GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin) != 0)
	{
		v |= 0x01;
	}
	return v;
}

/* 探测 SCL/SDA 上有没有接外部上拉电阻 —— 判定「模块有没有电气连到这两个脚」。
 * 原理：把引脚临时从开漏输出改成内部下拉输入（约 40kΩ），再读电平。
 *   模块板载上拉电阻一般 4.7kΩ，远强于内部 40kΩ 下拉：
 *     读到 1 -> 该线上确实挂着外部上拉，说明模块电气上接在这一脚；
 *     读到 0 -> 该线上探不到外部上拉（线没接触 / 插错排 / 短到地 / 模块没电）。
 * 返回值：bit1 = SCL 有外部上拉，bit0 = SDA 有外部上拉。
 *        0x03 = 两根都接好；0x00 = 都没有；0x01/0x02 = 只有一根。
 * 注意：本函数结束会把引脚恢复成开漏输出并置高。 */
uint8_t MyIIC_ProbeExternalPullup(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	uint8_t v = 0;

	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPD;	/* 内部下拉输入 */
	GPIO_InitStructure.GPIO_Pin   = s_scl_pin;
	GPIO_Init(IIC_SCL_PORT, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin   = s_sda_pin;
	GPIO_Init(IIC_SDA_PORT, &GPIO_InitStructure);

	Delay_us(200);						/* 内部下拉只有 40kΩ，给线容和外部上拉足够时间分出胜负 */

	if(GPIO_ReadInputDataBit(IIC_SCL_PORT, s_scl_pin) != 0)
	{
		v |= 0x02;
	}
	if(GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin) != 0)
	{
		v |= 0x01;
	}

	/* 恢复成开漏输出并置高，把总线放回空闲态 */
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin  = s_scl_pin;
	GPIO_Init(IIC_SCL_PORT, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin  = s_sda_pin;
	GPIO_Init(IIC_SDA_PORT, &GPIO_InitStructure);
	MyIIC_W_SCL(1);
	MyIIC_W_SDA(1);

	return v;
}

/* 反向探测：用内部上拉（约 40kΩ）看哪条线连拉都拉不起来 —— 那就是被硬拉在地了。
 * 与 MyIIC_ProbeExternalPullup() 配合，可区分「悬空」和「短到 GND」：
 *   外部上拉=0 且 本函数读到 1 -> 该线被硬拉在地（接到了 GND，或器件把线卡住）
 *   外部上拉=0 且 本函数读到 0 -> 该线悬空（线没接上）
 * 返回值：bit1 = SCL 拉不起来，bit0 = SDA 拉不起来。
 * 注意：本函数结束会把引脚恢复成开漏输出并置高。 */
uint8_t MyIIC_ProbeForcedLow(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	uint8_t v = 0;

	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;	/* 内部上拉输入 */
	GPIO_InitStructure.GPIO_Pin   = s_scl_pin;
	GPIO_Init(IIC_SCL_PORT, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin   = s_sda_pin;
	GPIO_Init(IIC_SDA_PORT, &GPIO_InitStructure);

	Delay_us(200);

	if(GPIO_ReadInputDataBit(IIC_SCL_PORT, s_scl_pin) == 0)
	{
		v |= 0x02;
	}
	if(GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin) == 0)
	{
		v |= 0x01;
	}

	/* 恢复成开漏输出并置高 */
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin  = s_scl_pin;
	GPIO_Init(IIC_SCL_PORT, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin  = s_sda_pin;
	GPIO_Init(IIC_SDA_PORT, &GPIO_InitStructure);
	MyIIC_W_SCL(1);
	MyIIC_W_SDA(1);

	return v;
}

/* 查 SCL/SDA 两根线之间是否短接：只拉低一根，看另一根是否被带着变低。
 * 返回值：bit1 = 拉低 SCL 时 SDA 跟着低；bit0 = 拉低 SDA 时 SCL 跟着低。
 *        0 = 没有互相短接。
 * 前提：两根线都能被释放为高。若某根线本来就低，「跟随变低」恒成立会误判，
 *       这时本函数返回 0x04 表示测试无效（bit2 置位），bit1/bit0 无意义。
 *       这个前提很重要 —— 曾经因为一根线已接地而误报过短接。 */
uint8_t MyIIC_TestShort(void)
{
	uint8_t v = 0;

	MyIIC_W_SDA(1);			/* 让两根线都先回到高 */
	MyIIC_W_SCL(1);
	Delay_us(10);

	if((GPIO_ReadInputDataBit(IIC_SCL_PORT, s_scl_pin) == 0) ||
	   (GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin) == 0))
	{
		return 0x04;		/* 有线释放后不为高，无法判断是否短接 */
	}

	MyIIC_W_SCL(0);			/* 只拉低 SCL */
	Delay_us(10);
	if(GPIO_ReadInputDataBit(IIC_SDA_PORT, s_sda_pin) == 0)
	{
		v |= 0x02;			/* SDA 被带着变低 -> 两根线短接 */
	}
	MyIIC_W_SCL(1);			/* SCL 放回高 */
	Delay_us(10);

	MyIIC_W_SDA(0);			/* 再只拉低 SDA */
	Delay_us(10);
	if(GPIO_ReadInputDataBit(IIC_SCL_PORT, s_scl_pin) == 0)
	{
		v |= 0x01;			/* SCL 被带着变低 -> 两根线短接 */
	}
	MyIIC_W_SDA(1);			/* SDA 放回高，恢复空闲态 */

	return v;
}

/* 扫描 7 位 I2C 地址 0x01~0x7F，把有应答的「8 位写地址」存进 found[]，返回器件个数。
 * 用途：AT24C02 在 0xA0 不应答时，区分是「总线上一个器件都没有」还是「器件在别的地址」
 *      —— A0/A1/A2 接错会把地址从 0xA0 挪到 0xA2 / 0xA4 / 0xA6 ...
 *      返回的数值和 AT24C02_WADDR(0xA0) 同口径，可直接比对。
 * 耗时：127 次寻址，约 32ms。
 * 注意：若 SDA 线悬空，读回的是随机值，会产生假应答 —— 结果不可信。
 *       所以扫描前应先看 MyIIC_ProbeExternalPullup() 两根线是否都正常。 */
uint8_t MyIIC_ScanAddr(uint8_t *found, uint8_t max)
{
	uint16_t addr;
	uint8_t  cnt = 0;

	for(addr = 0x01; addr <= 0x7F; addr ++)
	{
		MyIIC_Start();
		IIC_Send_Byte((uint8_t)(addr << 1));		/* 写方向 */
		if(IIC_Receive_Ack() == 0)					/* 收到应答 */
		{
			if(cnt < max)
			{
				found[cnt] = (uint8_t)(addr << 1);
			}
			cnt ++;
		}
		MyIIC_Stop();
	}
	return cnt;
}
