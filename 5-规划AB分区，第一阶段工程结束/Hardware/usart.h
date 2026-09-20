#ifndef __USART_H
#define __USART_H

#include "stdarg.h"
#include "stdio.h"
#include "string.h"

#define USART1_RX_SIZE 	2048			//接收缓冲区大小
#define USART1_TX_SIZE 	2048			//发送缓冲区大小
#define USART1_RX_MAX 	256				//接收最大值
#define Num 			10				//se指针对结构体数组长度

typedef struct
{
	uint8_t *start;						//开始指针
	uint8_t *end;						//结束指针
}UCB_URxBuffptr;						//se指针对结构体


typedef struct
{
	uint16_t URxCounter;				//统计接收的数据量
	UCB_URxBuffptr URxDataPtr[Num];		//结构体数组，每一个成员都是上方结构体成员
	UCB_URxBuffptr *URxDataIn;			//结构体指针，用于标记接收数据
	UCB_URxBuffptr *URxDataOut;			//结构体指针，用于提取接收的数据
	UCB_URxBuffptr *URxDataEnd;			//In和Out指针的结尾标志
}UCB_CB;								//串口控制结构体

void USART1_Init(uint32_t bandrate);
void MyDMA_Init(void);
void U1Rx_PtrInit(void);
void USART1_IRQHandler(void);
void U1_printf(char *format, ...);

extern UCB_CB U1CB;
extern uint8_t USART1_RxBuff[USART1_RX_SIZE];

#endif

