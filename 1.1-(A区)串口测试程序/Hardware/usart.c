#include "stm32f10x.h"                  // Device header
#include "usart.h"

uint8_t USART1_RxBuff[USART1_RX_SIZE];	//接收缓冲区ADC1
uint8_t USART1_TxBuff[USART1_TX_SIZE];
UCB_CB	U1CB;


void USART1_Init(uint32_t bandrate)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);
	
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode= GPIO_Mode_AF_PP ;//复用推挽输出用于串口发送数据
	GPIO_InitStructure.GPIO_Pin= GPIO_Pin_9 ;//上拉输入IPU用于串口的接收
	GPIO_InitStructure.GPIO_Speed= GPIO_Speed_50MHz ;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	
	GPIO_InitStructure.GPIO_Mode= GPIO_Mode_IPU ;
	GPIO_InitStructure.GPIO_Pin= GPIO_Pin_10 ;
	GPIO_InitStructure.GPIO_Speed= GPIO_Speed_50MHz ;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	
	USART_DeInit(USART1);
	
	USART_InitTypeDef USAERT_InitStructure;
	USAERT_InitStructure.USART_BaudRate= bandrate;//波特率
	USAERT_InitStructure.USART_HardwareFlowControl= USART_HardwareFlowControl_None;//硬件流控制
	USAERT_InitStructure.USART_Mode= USART_Mode_Tx | USART_Mode_Rx;//串口模式
	USAERT_InitStructure.USART_Parity= USART_Parity_No;//校验位
	USAERT_InitStructure.USART_StopBits= USART_StopBits_1;//停止位
	USAERT_InitStructure.USART_WordLength= USART_WordLength_8b;//字长
	USART_Init(USART1,&USAERT_InitStructure);
	
	USART_ITConfig(USART1,USART_IT_IDLE,ENABLE);
	
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	
	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel= USART1_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelCmd= ENABLE;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority= 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority= 1;
	NVIC_Init(&NVIC_InitStructure);
	
	U1Rx_PtrInit();
	MyDMA_Init();
	
	USART_Cmd(USART1,ENABLE);
	USART_DMACmd(USART1, USART_DMAReq_Rx, ENABLE);
}

void MyDMA_Init(void)
{
	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1,ENABLE);//开始DMA1的时钟
	
	DMA_DeInit(DMA1_Channel5);
	
	DMA_InitTypeDef DMA_InirStructure;
	
	DMA_InirStructure.DMA_PeripheralBaseAddr = 0x40013804;//外设站点的起始地址:USART1的数据寄存器地址
	DMA_InirStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设站点的数据宽度
	DMA_InirStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;//外设站点的地址是否自增
	DMA_InirStructure.DMA_MemoryBaseAddr = (uint32_t)USART1_RxBuff;//存储器站点的起始地址
	DMA_InirStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;//存储器站点的数据宽度
	DMA_InirStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;//存储器站点的地址是否自增
	DMA_InirStructure.DMA_DIR =DMA_DIR_PeripheralSRC ;//传输方向,本实验将DataA作为外设站点，数据传输从外设站点到存储器站点，所以此时外设站点为源头(SRC)
	DMA_InirStructure.DMA_BufferSize = USART1_RX_MAX + 1;//缓存区大小，就是传输计数器
	DMA_InirStructure.DMA_Mode = DMA_Mode_Normal;//传输模式，是否使用自动重装
	DMA_InirStructure.DMA_M2M = DMA_M2M_Disable;//选择是否是存储器到存储器，就是选择硬件触发还是软件触发
	DMA_InirStructure.DMA_Priority = DMA_Priority_Medium;//优先级
	DMA_Init(DMA1_Channel5,&DMA_InirStructure);
	
	DMA_Cmd(DMA1_Channel5, ENABLE);
	
}

void U1Rx_PtrInit(void)
{
	U1CB.URxDataIn = &U1CB.URxDataPtr[0];
	U1CB.URxDataOut = &U1CB.URxDataPtr[0];
	U1CB.URxDataEnd = &U1CB.URxDataPtr[Num - 1];
	
	U1CB.URxDataIn->start = USART1_RxBuff;
	U1CB.URxCounter = 0;
}

void USART1_IRQHandler(void)//循环，空闲中断
{
	if(USART_GetITStatus(USART1, USART_IT_IDLE) == SET)//产生空闲中断
	{
		USART_GetFlagStatus(USART1, USART_FLAG_IDLE);//先读USART_SR寄存器
		USART_ReceiveData(USART1);					 //再读USART_DR寄存器
		U1CB.URxCounter += (USART1_RX_MAX + 1) - DMA_GetCurrDataCounter(DMA1_Channel5);//DMA总量-DMA通道的剩余量=DMA接收量
		U1CB.URxDataIn->end = &USART1_RxBuff[U1CB.URxCounter - 1];//end指针指向接收量的结尾
		U1CB.URxDataIn ++;							 //In指针后移一位，并且每次后移一位都要和end指针相比
		if(U1CB.URxDataIn == U1CB.URxDataEnd)		 //判断是否到达END
		{
			U1CB.URxDataIn = &U1CB.URxDataPtr[0];
		}
		if(USART1_RX_SIZE - U1CB.URxCounter >= USART1_RX_MAX)
		{
			U1CB.URxDataIn->start = &USART1_RxBuff[U1CB.URxCounter];//还可以继续存放，从上一次累加的数组开始继续存放即可
		}
		else//否则就回卷回到数组最开始的地方
		{
			U1CB.URxDataIn->start = USART1_RxBuff;
			U1CB.URxCounter = 0;
		}
		
		DMA_DeInit(DMA1_Channel5);//串口空闲中断之后关掉DMA
		DMA_InitTypeDef DMA_InirStructure;
	
		DMA_InirStructure.DMA_PeripheralBaseAddr = 0x40013804;//外设站点的起始地址:USART1的数据寄存器地址
		DMA_InirStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设站点的数据宽度
		DMA_InirStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;//外设站点的地址是否自增
		DMA_InirStructure.DMA_MemoryBaseAddr = (uint32_t)U1CB.URxDataIn->start;//存储器站点的起始地址
		DMA_InirStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;//存储器站点的数据宽度
		DMA_InirStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;//存储器站点的地址是否自增
		DMA_InirStructure.DMA_DIR =DMA_DIR_PeripheralSRC ;//传输方向,本实验将DataA作为外设站点，数据传输从外设站点到存储器站点，所以此时外设站点为源头(SRC)
		DMA_InirStructure.DMA_BufferSize = USART1_RX_MAX + 1;//缓存区大小，就是传输计数器
		DMA_InirStructure.DMA_Mode = DMA_Mode_Normal;//传输模式，是否使用自动重装
		DMA_InirStructure.DMA_M2M = DMA_M2M_Disable;//选择是否是存储器到存储器，就是选择硬件触发还是软件触发
		DMA_InirStructure.DMA_Priority = DMA_Priority_Medium;//优先级
		DMA_Init(DMA1_Channel5,&DMA_InirStructure);
	
		DMA_Cmd(DMA1_Channel5, ENABLE);//此时DMA准备好下次的接收
	}
}

void U1_printf(char *format, ...)//format：个数不确定，参数类型不确定。
{
	uint16_t i;
	va_list listdata;
	va_start(listdata, format); 
	vsprintf((char *)USART1_TxBuff, format, listdata);
	va_end(listdata);
	
	for(i = 0; i < strlen((const char*)USART1_TxBuff); i ++)
	{
		while(USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
		USART_SendData(USART1, USART1_TxBuff[i]);
	}
	while(USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}



