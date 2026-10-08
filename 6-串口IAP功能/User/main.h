#ifndef __MAIN_H
#define __MAIN_H

/* OTA 相关的结构体与标志值已抽到 Hardware/ota_layout.h ——
 * 那是 A 区 App 与 B 区 BootLoader 的共享契约，两个工程必须引用同一份定义。
 * 本文件只保留 BootLoader 自己关心的东西：Flash 分区宏、运行期标志。 */
#include "ota_layout.h"

#define MyFlash_StartAddress 	0x08000000														//FLASH起始地址
#define MyFlash_Page_Size 		1024															//FLASH扇区大小
#define MyFlash_Page_Num	 	64																//FLASH总扇区个数
#define MyFlash_B_Page_Num	 	28																//B区扇区个数
#define MyFlash_A_Page_Num   	(MyFlash_Page_Num - MyFlash_B_Page_Num)							//A区扇区个数
#define MyFlash_A_Start_Page 	(MyFlash_B_Page_Num)												//A区起始扇区编号
#define MyFlash_A_Start_Address	(MyFlash_StartAddress + MyFlash_A_Start_Page * MyFlash_Page_Size)	//A区起始地址

/* ⚠ 上面三个宏的括号不能省。
 * 曾经它们没括号，而 A_Page_Num 展开是 "64 - 36"；
 * 一旦有人写成 (uint32_t)MyFlash_A_Page_Num * 1024，
 * 就会被解析成 (uint32_t)64 - (36 * 1024) —— 无符号回绕成 42 亿，
 * 于是"长度是否超出 A 区容量"这类判断永远为假，检查形同虚设。
 * 在实参位置（如 MyFlash_EraseFlash(36, MyFlash_A_Page_Num)）恰好不出错，
 * 所以这个坑很隐蔽 —— 直到 2026-09-27 反例测试才把它挖出来。 */

#define UpData_A_Flag				0x00000001													//状态标志位，置位表明需要更新A区

#define	IAP_XMODEMC_FLAG			0x00000002
#define	IAP_XMODEMData_FLAG			0x00000004
#define	IAP_SETVERSION_FLAG			0x00000008
#define	W25Q64_DoLo_FLAG			0x00000010
#define	W25Q64_DoLo_Xmodem_FLAG		0x00000020
#define	W25Q64_To_Flash_Dolo_FLAG	0x00000040

typedef struct
{								
	uint8_t UpDataBuff[MyFlash_Page_Size];				//更新A区时，用于保存从W25Q64中读取的数据
	uint32_t W25Q64_BlockNum;							//用于记录从哪个W25Q64的块中读取数据
	uint32_t XmodemTimer;
	uint32_t XmodemNum;
	uint32_t XmodemCRC;
}UpDataA_CB;											//更新A区用的结构体

extern UpDataA_CB UpDataA;								//外部声明变量
extern uint32_t BootStaFlag;							//外部声明变量


#endif
