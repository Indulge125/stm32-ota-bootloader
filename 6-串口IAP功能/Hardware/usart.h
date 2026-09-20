#ifndef __USART_H
#define __USART_H

#include "stdarg.h"
#include "stdio.h"
#include "string.h"

#define USART1_RX_SIZE 	2048			//接收缓冲区大小
#define USART1_TX_SIZE 	2048			//发送缓冲区大小
#define USART1_RX_MAX 	256				//接收最大值
#define Num 			32				//se指针对结构体数组长度
/* 槽位数量决定「主循环没跑的时候」能缓存多少个接收事件。
 * BootLoader_Enter() 死等 5 秒期间主循环不消费事件，上位机若在这段
 * 窗口内高频发送字符（比如自动化脚本连续发 'w'），事件会把环形缓冲
 * 塞满；塞满后 In 指针会绕回去覆盖尚未消费的槽位（里面存着 start/end
 * 指针），之后 datalen 算成垃圾值，数据包被静默丢弃。
 * 10 -> 32 是为了把这个窗口撑大（RAM 代价 32*8=256 字节）。
 * 注意这只是缓解：槽位仍然有限，上位机不应在开机窗口内刷字符。 */

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

