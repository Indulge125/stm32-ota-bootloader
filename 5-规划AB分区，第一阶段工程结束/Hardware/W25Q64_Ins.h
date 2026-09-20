#ifndef __W25Q64_INS_H
#define __W25Q64_INS_H

/*=====================================
 *          基本操作指令
 *=====================================*/
#define W25Q64_CMD_WRITE_ENABLE        0x06    // 写使能，允许页编程/擦除操作
#define W25Q64_CMD_WRITE_DISABLE       0x04    // 写禁止，禁止页编程/擦除操作
#define W25Q64_CMD_READ_STATUS_REG1    0x05    // 读状态寄存器1
#define W25Q64_CMD_READ_STATUS_REG2    0x35    // 读状态寄存器2
#define W25Q64_CMD_WRITE_STATUS_REG    0x01    // 写状态寄存器


/*=====================================
 *            数据读写指令
 *=====================================*/
#define W25Q64_CMD_READ_DATA           0x03    // 标准读数据（104MHz以下时钟）
#define W25Q64_CMD_FAST_READ           0x0B    // 快速读数据（支持QPI模式）
#define W25Q64_CMD_FAST_READ_DUAL      0x3B    // 双线快速读
#define W25Q64_CMD_FAST_READ_QUAD      0x6B    // 四线快速读
#define W25Q64_CMD_PAGE_PROGRAM        0x02    // 页编程（一次最多256字节）
#define W25Q64_CMD_MULTI_PAGE_PROGRAM  0x32    // 多页编程（支持页缓存）


/*=====================================
 *            擦除操作指令
 *=====================================*/
#define W25Q64_CMD_SECTOR_ERASE        0x20    // 扇区擦除（4KB）
#define W25Q64_CMD_BLOCK_ERASE_32KB    0x52    // 32KB块擦除
#define W25Q64_CMD_BLOCK_ERASE_64KB    0xD8    // 64KB块擦除
#define W25Q64_CMD_CHIP_ERASE          0xC7    // 芯片擦除（全部8MB）
#define W25Q64_CMD_CHIP_ERASE_ALT      0x60    // 芯片擦除（替代指令）


/*=====================================
 *          设备配置指令
 *=====================================*/
#define W25Q64_CMD_READ_ID             0x9F    // 读JEDEC ID（厂商ID+设备ID）
#define W25Q64_CMD_READ_UNIQUE_ID      0x4B    // 读唯一ID（64位）
#define W25Q64_CMD_DEVICE_RESET        0x66    // 设备复位
#define W25Q64_CMD_ENTER_POWER_DOWN    0xB9    // 进入掉电模式
#define W25Q64_CMD_EXIT_POWER_DOWN     0xAB    // 退出掉电模式
#define W25Q64_CMD_ENTER_LOW_POWER     0xF4    // 进入低功耗模式
#define W25Q64_CMD_READ_POWER_MODE     0x3A    // 读电源模式状态


/*=====================================
 *          高级功能指令
 *=====================================*/
#define W25Q64_CMD_ENABLE_QPI          0x38    // 使能四通道SPI（QPI）
#define W25Q64_CMD_DISABLE_QPI         0xFF    // 禁用四通道SPI
#define W25Q64_CMD_READ_PARAMETER      0x34    // 读参数寄存器
#define W25Q64_CMD_WRITE_PARAMETER     0x31    // 写参数寄存器
#define W25Q64_CMD_READ_ALTERNATE      0x15    // 读备用寄存器
#define W25Q64_CMD_WRITE_ALTERNATE     0x11    // 写备用寄存器


/*=====================================
 *          状态寄存器位定义
 *=====================================*/
// 状态寄存器1（Status Register 1）
#define W25Q64_SR1_WEL                 0x02    // 写使能锁存位（Write Enable Latch）
#define W25Q64_SR1_BP0                 0x04    // 块保护位0
#define W25Q64_SR1_BP1                 0x08    // 块保护位1
#define W25Q64_SR1_BP2                 0x10    // 块保护位2
#define W25Q64_SR1_BP3                 0x20    // 块保护位3
#define W25Q64_SR1_WP                  0x80    // 写保护位（Write Protect）

// 状态寄存器2（Status Register 2）
#define W25Q64_SR2_DQPS                0x03    // 数据队列预取大小
#define W25Q64_SR2_LPS                 0x04    // 低功耗模式使能
#define W25Q64_SR2_CMP                 0x08    // 比较模式使能
#define W25Q64_SR2_BP4                 0x10    // 块保护位4
#define W25Q64_SR2_SUS                 0x20    // 挂起状态
#define W25Q64_SR2_BUSY                0x40    // 忙标志位（操作进行中）
#define W25Q64_SR2_WPERR               0x80    // 写保护错误标志

#define W25Q64_DUMMY_BYTE			   0xFF	   //接收时交换过去的无用数据

#endif
