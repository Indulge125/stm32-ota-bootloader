#ifndef __M24C02_H
#define __M24C02_H

#define AT24C02_WADDR	0xA0//M24C02写入地址
#define AT24C02_RADDR	0xA1//M24C02读出地址

uint8_t AT24C02_WriteByte(uint8_t addr, uint8_t wdata);
uint8_t AT24C02_WritePage(uint8_t addr, uint8_t *wdata);
uint8_t AT24C02_ReadData(uint8_t addr, uint8_t *rdata, uint16_t datalen);


#endif


