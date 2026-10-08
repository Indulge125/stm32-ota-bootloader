#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "onenet_token.h"
#include "ota_layout.h"
#include "onenet_ota.h"

/* 应用版本号 APP_VERSION 定义在 onenet_ota.h ——
 * 那里是唯一能同时被 main.c 和 onenet_ota.c 看见、又语义合适的地方。
 * 改版本号只改那一处，具体清单见 onenet_ota.h 的注释。 */

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

	/* ---- OneNET OTA：查一次，需要就升 ----
	 *
	 * 为什么放在这里（初始化之后、主循环之前），而不是放进主循环轮询：
	 *   整个流程含 13 次 HTTP 往返，要十几秒。塞进主循环会拖累业务，
	 *   而且每次都去问一遍对平台也是无谓的压力。
	 *   真要做周期性检查，应该由业务自己决定节奏（比如每 10 分钟一次）。
	 *
	 * ⚠️ 需要升级时这个函数**不会返回** —— 它会置好标志然后复位，
	 *    由 B 区 BootLoader 完成搬运。所以不要在后面写依赖它返回的逻辑。 */
	OTA_Run();

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
