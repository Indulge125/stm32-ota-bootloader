#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "usart.h"
#include "iic.h"
#include "W25Q64.h"
#include "m24c02.h"
#include "onenet_token.h"
#include "ota_layout.h"
#include "onenet_ota.h"
#include <string.h>		/* memset / memcpy / strcmp：给下面的版本戳用 */

/* 应用版本号 APP_VERSION 定义在 onenet_ota.h ——
 * 那里是唯一能同时被 main.c 和 onenet_ota.c 看见、又语义合适的地方。
 * 改版本号只改那一处，具体清单见 onenet_ota.h 的注释。 */

/* OTA 信息在 RAM 里的实例。ota_layout.h 只给"布局"（类型和声明），
 * 每个可执行体各自持有一份数据 —— 所以这里必须定义一次，
 * 否则链接器报 Undefined symbol OTA_Info（m24c02.c 要用它）。
 * B 区的 BootLoader 在自己 main.c 里也有一份同名实例，两者互不影响。 */
OTA_InfoCB OTA_Info;

/* ==========================================================================
 * OLED 显示
 * ==========================================================================
 * 布局（128x64，8x16 字库，每行 16 个 ASCII 字符）：
 *
 *     行1   A-OTA v1.0.0      固件标识 + 版本
 *     行2   EE:OK  W25:OK     上电自检结果
 *     行3   OTA: idle         OTA 阶段（由 onenet_ota.c 通过回调刷新）
 *     行4   （进度）           下载时显示"第几片/共几片"
 *
 * 为什么先写"行1 + 版本"再去做自检：
 *   自检要读两颗外部器件，万一卡住（器件没接好时 I2C/SPI 可能长时间等待），
 *   屏幕上先有字就能证明"至少 A 区跑起来了、版本是哪个"——
 *   否则黑屏时连"是没启动还是卡在自检"都分不清。
 */
static void OledStatus(uint8_t line, const char *text)
{
	if(line < 1 || line > 4) return;

	/* 先清整行再写。不这么做的话，短字符串覆盖不掉上一次长字符串的尾巴 ——
	 * 比如 "12/30 OTA" 换成 "OTA: OK" 之后会显示成 "OTA: OK30 OTA"。 */
	OLED_ShowString(line, 1, "                ");	/* 16 个空格 = 整行 */
	OLED_ShowString(line, 1, (char *)text);
}

/* ==========================================================================
 * 上电硬件自检
 * ==========================================================================
 *
 * B 区 BootLoader 一直有这个自检，A 区当初没加 —— 代价是：
 * `MyIIC_Init()` 和 `W25Q64_Init()` 忘了调用这件事，一路跑到最后才暴露，
 * 而且暴露得很隐蔽：
 *
 *   两次外部存储完全没被初始化 → 引脚根本没配成 I2C / SPI
 *   → AT24C02_WriteOTAInfo() 和 W25Q64_PageProgram() 全是空操作
 *   → 但这两个函数返回 void、不报任何错
 *   → 下载照常"成功"（MD5 算的是 RAM 里的数据，跟 W25Q64 没关系）
 *   → 复位后 BootLoader 读到的是旧标志，判定"无更新"，跳回旧固件
 *
 * 现象就是"升级了，但什么都没发生"。所以这里必须主动确认一次通信。
 *
 * 返回 0 = 全部正常；非 0 = 有不正常的器件（调用方应跳过 OTA）。
 * 自检失败**不阻止设备启动**（业务照常跑），只阻止 OTA —— 因为带着这种
 * 故障去下载，只会白白浪费 30KB 流量和一分钟，然后什么也升不上去。
 * ========================================================================== */
static uint8_t HW_SelfTest(void)
{
	uint8_t  MID = 0;
	uint16_t DID = 0;
	uint8_t  ee_backup = 0, ee_read = 0, ee_ok = 0;
	uint8_t  bad = 0;

	/* ---- 1. W25Q64：读 JEDEC ID，正常是 MID=0xEF、DID=0x4017 ---- */
	W25Q64_ReadID(&MID, &DID);
	if(MID == 0xEF && DID == 0x4017)
	{
		OLED_ShowString(2, 8, "W25:OK  ");
		U1_printf("自检: W25Q64 OK (MID=0x%02X DID=0x%04X)\r\n", MID, DID);
	}
	else
	{
		bad = 1;
		OLED_ShowString(2, 8, "W25:FAIL");
		U1_printf("自检: W25Q64 无应答 (MID=0x%02X DID=0x%04X，应为 0xEF/0x4017)\r\n",
		          MID, DID);
		U1_printf("      查 PA4=CS/PA5=CLK/PA6=DO/PA7=DI 接线、模块供电，"
		          "以及 W25Q64_Init() 是否调用\r\n");
	}

	/* ---- 2. AT24C02：读写回环 ----
	 * 地址取 0xF0：OTA_InfoCB 只占 0x00~0x4F，不冲突。
	 * 先备份原值、测完还原，不留痕迹。 */
	AT24C02_ReadData(0xF0, &ee_backup, 1);
	ee_ok = (AT24C02_WriteByte(0xF0, 0x5A) == 0);	/* 返回 0 = 每一步都收到了 ACK */
	if(ee_ok)
	{
		AT24C02_ReadData(0xF0, &ee_read, 1);
		ee_ok = (ee_read == 0x5A);
	}
	AT24C02_WriteByte(0xF0, ee_backup);				/* 还原现场 */

	if(ee_ok)
	{
		OLED_ShowString(2, 1, "EE:OK   ");
		U1_printf("自检: AT24C02 OK\r\n");
	}
	else
	{
		bad = 1;
		OLED_ShowString(2, 1, "EE:FAIL ");
		U1_printf("自检: AT24C02 回环失败 —— 查 PB10/PB11 接线、模块 VCC/GND，"
		          "以及 A0/A1/A2 跳线是否全跨在 GND 侧\r\n");
	}

	if(bad)
	{
		U1_printf("      ⚠️ 外部存储有故障 —— 升级标志和固件都写不进去，"
		          "本次跳过 OTA\r\n");
	}
	return bad;
}

int main(void)
{
	OLED_Init();

	/* 先把"我是谁、什么版本"打上屏 —— 在做任何可能卡住的事之前。
	 * 黑屏时分不清"没启动"和"卡在自检"，这两行就是分界线。 */
	OLED_ShowString(1, 1, "A-OTA v" APP_VERSION);
	OLED_ShowString(3, 1, "OTA: boot       ");
	OLED_ShowString(4, 1, "                ");

	MyIIC_Init();				//软件 I2C：AT24C02 挂在 PB10/PB11
	USART1_Init(9600);			//调试串口
	AT24C02_ReadOTAInfo();		//把 EEPROM 里的 OTA 信息读进 OTA_Info
	W25Q64_Init();				//软件 SPI：W25Q64 挂在 PA4~PA7

	U1_printf("APP v%s\r\n", APP_VERSION);		//开机版本标识：串口一眼看出升级是否生效

	/* ---- 把当前固件版本刷进 OTA_Ver ----
	 *
	 * OTA_Ver 是 AT24C02 里那个"版本号字符串，给调试用"的字段。
	 * 之前它只能靠 B 区菜单的命令 3 手动设置 —— 而那条路很难走通：
	 *   BootLoader 要求**正好 26 字节**且格式匹配 sscanf("VER-%d.%d.%d-...")，
	 *   多一个（串口工具补的回车）或少一个都会被静默忽略，手敲几乎不可能不出错。
	 *   而且它走的还是"读出来原样写回"那条路：一旦某次读取失败，
	 *   整个结构体会被写成 0，版本号就永久变成垃圾，只能再进菜单重设。
	 *
	 * A 区自己就知道当前版本，开机刷一次，这个字段就永远有意义了 ——
	 * 串口查版本号看到的就是"设备实际在跑哪个版本"，这正是它该有的含义。
	 *
	 * 只在值真的不同时才写：AT24C02 有擦写寿命，没必要每次开机都动它。
	 * （升级时 StepApply() 会把它覆盖成"目标版本"，新固件起来后再刷成新版本，
	 *   两个时机配合起来，"查版本号"永远能看到有意义的值。） */
	{
		static const char stamp[] = "VER-" APP_VERSION;
		if(strcmp((const char *)OTA_Info.OTA_Ver, stamp) != 0)
		{
			memset(OTA_Info.OTA_Ver, 0, sizeof(OTA_Info.OTA_Ver));
			memcpy(OTA_Info.OTA_Ver, stamp, sizeof(stamp) - 1);
			AT24C02_WriteOTAInfo();
			U1_printf("OTA_Ver 已刷新为 %s\r\n", stamp);
		}
	}

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
	 *   整个流程含 30 次 HTTP 往返，要四十秒上下。塞进主循环会拖累业务，
	 *   而且每次都去问一遍对平台也是无谓的压力。
	 *   真要做周期性检查，应该由业务自己决定节奏（比如每 10 分钟一次）。
	 *
	 * ⚠️ 需要升级时这个函数**不会返回** —— 它会置好标志然后复位，
	 *    由 B 区 BootLoader 完成搬运。所以不要在后面写依赖它返回的逻辑。
	 *
	 * 硬件自检没过就不升级：带着"存储写不进去"的故障去下载，
	 * 只会白费流量和时间，最后什么也升不上去。 */
	if(HW_SelfTest() != 0)
	{
		U1_printf("硬件自检未通过 —— 本次跳过 OTA\r\n");
		OledStatus(3, "OTA: skip(hw)");
	}
	else
	{
		/* 把 OLED 显示注册给 OTA 模块 —— 它会在查任务/下载/校验各阶段
		 * 回调过来刷新第 3、4 行，抬头就能看到进度，不用盯串口。 */
		OTA_SetStatusCb(OledStatus);
		OTA_Run();
	}

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
