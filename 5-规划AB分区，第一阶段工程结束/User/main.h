#ifndef __MAIN_H
#define __MAIN_H

#define MyFlash_StartAddress 	0x08000000														//FLASH起始地址
#define MyFlash_Page_Size 		1024															//FLASH扇区大小
#define MyFlash_Page_Num	 	64																//FLASH总扇区个数
#define MyFlash_B_Page_Num	 	32																//B区扇区个数
#define MyFlash_A_Page_Num   	MyFlash_Page_Num - MyFlash_B_Page_Num							//A区扇区个数
#define MyFlash_A_Start_Page 	MyFlash_B_Page_Num												//A区起始扇区编号
#define MyFlash_A_Start_Address	MyFlash_StartAddress + MyFlash_A_Start_Page * MyFlash_Page_Size	//A区起始地址

#define UpData_A_Flag			0x00000001														//状态标志位，置位表明需要更新A区


#define OTA_SET_FLAG			0x0a050301														//OTA_Flag对勾状态对应的数值，如果OTA_Flag等于该值，说明需要OTA更新A区

typedef struct
{								
	/*AT24c02一页8个字节，OTA_Flag 4个字节，FileLen总共5*4=20个字节，两个加在一起就是24个字节，刚好循环写3页*/
	uint32_t OTA_Flag;			//标志性的变量，等于OTA_SET_FLAG定义的值，说明需要OTA更新A区
	uint32_t FileLen[5];		//W25Q64中不同块中程序固件的长度，0号成员固定对应W25Q64中程序编码0的块，用于OTA    
}OTA_InfoCB;					//OTA相关的信息结构体，需要保存到24c02

#define OTA_INFOCB_SIZE			sizeof(OTA_InfoCB)		//OTA相关的信息结构体占用的字节长度
	
typedef struct
{								
	uint8_t UpDataBuff[MyFlash_Page_Size];				//更新A区时，用于保存从W25Q64中读取的数据
	uint32_t W25Q64_BlockNum;							//用于记录从哪个W25Q64的块中读取数据
}UpDataA_CB;											//更新A区用的结构体

extern OTA_InfoCB OTA_Info;								//外部声明变量
extern UpDataA_CB UpDataA;								//外部声明变量
extern uint32_t BootStaFlag;							//外部声明变量


#endif
