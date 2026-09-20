#ifndef __USART_H
#define __USART_H

#include "stdarg.h"
#include "stdio.h"
#include "string.h"

#define USART1_RX_SIZE 	2048			//接收缓冲区大小
#define USART1_TX_SIZE 	2048			
#define USART1_RX_MAX 	256				//接收最大值
#define Num 			10				//结构体数组成员

typedef struct
{
	uint8_t *start;						//开始指针
	uint8_t *end;						//结束指针
}UCB_URxBuffptr;	


typedef struct
{
	uint16_t URxCounter;				//统计接收的数据量
	UCB_URxBuffptr URxDataPtr[Num];		//结构体数组，每一个成员都是上方结构体成员
	UCB_URxBuffptr *URxDataIn;			//结构体指针
	UCB_URxBuffptr *URxDataOut;
	UCB_URxBuffptr *URxDataEnd;
}UCB_CB;	

void USART1_Init(uint32_t bandrate);
void MyDMA_Init(void);
void U1Rx_PtrInit(void);
void USART1_IRQHandler(void);
void U1_printf(char *format, ...);

extern UCB_CB U1CB;

#endif

