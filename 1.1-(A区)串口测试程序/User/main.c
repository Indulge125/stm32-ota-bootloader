#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "onenet_token.h"
#include "ota_layout.h"
#include "onenet_http.h"
#include <string.h>

/* 应用版本号：OTA 升级包的目标版本必须与此字面量一致。
 * 改版本只改这一处 —— 开机打印和以后向平台上报都用它。 */
#define APP_VERSION		"1.0.0"

/* OTA 信息在 RAM 里的实例。ota_layout.h 只给"布局"（类型和声明），
 * 每个可执行体各自持有一份数据 —— 所以这里必须定义一次，
 * 否则链接器报 Undefined symbol OTA_Info（m24c02.c 要用它）。
 * B 区的 BootLoader 在自己 main.c 里也有一份同名实例，两者互不影响。 */
OTA_InfoCB OTA_Info;

/* POST /version 的 body。
 * f_version 是模组版本号（本项目里 ESP8266 的固件不参与 OTA，填占位值）；
 * s_version 才是平台拿去和升级包「目标版本」比对的那个 —— 必须和 APP_VERSION
 * 一致，否则平台永远认为版本不匹配（或反过来永远不下发任务）。 */
#define VERSION_BODY	"{\"s_version\":\"" APP_VERSION "\",\"f_version\":\"1.0.0\"}"

/* ==========================================================================
 * M3 联调：对 OneNET 发一次 POST /version
 * ==========================================================================
 * 目的：验证「签名 + HTTP + 网络」三层是否全通。
 * 验收标准：串口收到 {"code":0,"msg":"succ"}。
 *
 * 这是**临时联调代码**。M4 写完完整的 OTA 流程（上报版本 → 检测任务 →
 * 分片下载 → 写 W25Q64 → 置标志）之后，这部分会被真正的业务逻辑取代。 */
static void M3_HttpTest(void)
{
	HTTP_Resp resp;
	char      path[96];
	uint8_t   buf[128];
	uint16_t  n;
	uint32_t  total = 0;
	uint32_t  waited = 0;
	uint8_t   rc;

	U1_printf("\r\n===== M3: POST /version =====\r\n");

	if(HTTP_DevPath(path, sizeof(path), "version") != 0)
	{
		U1_printf("路径拼装失败\r\n");
		return;
	}
	U1_printf("path = %s\r\n", path);

	rc = HTTP_Start("POST", path, VERSION_BODY,
	                (uint16_t)strlen(VERSION_BODY), 0, &resp);
	if(rc != HTTP_OK)
	{
		U1_printf(">>> HTTP_Start 失败 rc=%u\r\n", (unsigned)rc);
		return;
	}

	/* 收 body。用 Connection: close 时没有可靠的分帧信号，
	 * 所以按 content_len 判结束：收够了就停，或者超时放弃。 */
	while(waited < 8000)
	{
		n = HTTP_ReadBody(buf, (uint16_t)(sizeof(buf) - 1));
		if(n > 0)
		{
			buf[n] = '\0';
			U1_printf("%s", (char *)buf);
			total += n;
			if(resp.content_len != 0xFFFFFFFFu && total >= resp.content_len) break;
			continue;
		}
		if(resp.content_len != 0xFFFFFFFFu && total >= resp.content_len) break;
		Delay_ms(10);
		waited += 10;
	}
	U1_printf("\r\n>>> body 共 %u 字节（等了 %ums）\r\n", (unsigned)total, (unsigned)waited);

	HTTP_End();
	U1_printf("===== M3 结束 =====\r\n\r\n");
}

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

	M3_HttpTest();		//M3 联调：验证签名+HTTP+网络三层。M4 完成后删掉

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
