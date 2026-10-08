#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "onenet_token.h"
#include "ota_layout.h"

/* 应用版本号：OTA 升级包的目标版本必须与此字面量一致。
 * 改版本只改这一处 —— 开机打印和以后向平台上报都用它。 */
#define APP_VERSION		"1.0.0"

/* OTA 信息在 RAM 里的实例。ota_layout.h 只给"布局"（类型和声明），
 * 每个可执行体各自持有一份数据 —— 所以这里必须定义一次，
 * 否则链接器报 Undefined symbol OTA_Info（m24c02.c 要用它）。
 * B 区的 BootLoader 在自己 main.c 里也有一份同名实例，两者互不影响。 */
OTA_InfoCB OTA_Info;

int main(void)
{
	OLED_Init();
	OLED_ShowString(1,1,"XZY");
	USART1_Init(9600);
	U1_printf("APP v%s\r\n", APP_VERSION);		//开机版本标识：串口一眼看出升级是否生效

	/* 签名自检：Token 算错时服务器只会笼统回一句 "auth failed"，
	 * 先在这里跑一遍，把"算法写错"和"网络/配置问题"分开。
	 * 失败时打印的是第一个失败的用例编号，对照 sha1.c / onenet_token.c 的注释定位。 */
	{
		uint8_t rc = OneNet_TokenSelfTest();
		if(rc == 0)	U1_printf("Token SelfTest: PASS\r\n");
		else		U1_printf("Token SelfTest: FAIL rc=%d\r\n", rc);
	}

	U1_printf("%d %c %x\r\n",0x30, 0x30, 0x30);
	while(1)
	{
		if(U1CB.URxDataOut != U1CB.URxDataIn)
		{
			U1_printf("本次接收了%d字节数据\r\n",U1CB.URxDataOut->end - U1CB.URxDataOut->start + 1);
			for(uint16_t i = 0; i <U1CB.URxDataOut->end - U1CB.URxDataOut->start + 1; i ++)
			{
				U1_printf("%c", U1CB.URxDataOut->start[i]);
			}
			U1_printf("\r\n");
			U1CB.URxDataOut ++;							 //Out指针后移一位
			if(U1CB.URxDataOut == U1CB.URxDataEnd)		 //判断是否到达END
			{
				U1CB.URxDataOut = &U1CB.URxDataPtr[0];
			}
		}
	}
}

/*OUT，IN都是指向的创建的同一个结构体类型（有start和end）的数组，
接收数据的时候IN在改变数组中其中一个成员的start和end的指向，
OUT和IN所指向的数组是同一系列。*/
