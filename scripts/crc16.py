#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CRC-16/XMODEM —— 上位机侧的唯一实现，MCU 与服务器共用同一套参数。

初值 0x0000、多项式 0x1021、MSB 优先、不反转、不末异或。
与 MCU 侧 `6-串口IAP功能/Hardware/boot.c` 的 `Xmodem_CRC16_Update()` 逐位等价。

⚠️ 为什么单独成一个模块，而不是每个脚本各写一份：
   两边的实现必须一致，但「两份拷贝总有一天会漂移」—— 这个项目已经因为
   「两处配置必须同步」（Flash 分区表）被咬过一次，代价很大。
   所以 Python 侧只留这一份。改这里之前，先想清楚 MCU 侧要不要跟着改。

   同理，`crc16_update()`（增量式）和 `crc16_xmodem()`（整段式）必须给出
   相同结果 —— MCU 侧边收边算、以及从 W25Q64 回读分块重算，用的都是增量式，
   如果两者不等价，回读校验会永远失败。self_test() 里专门验了这一条。
"""
import sys


def crc16_update(crc: int, data: bytes) -> int:
    """增量计算：给定已有 crc 继续往下算。

    对应 MCU 侧 `Xmodem_CRC16_Update(crc, data, len)`。
    """
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def crc16_xmodem(data: bytes) -> int:
    """整段计算（初值 0x0000）。"""
    return crc16_update(0x0000, data)


def self_test(verbose: bool = True) -> None:
    """用已知向量验证实现。对不上就别往下发固件了。"""
    vectors = [(b"123456789", 0x31C3), (b"", 0x0000), (b"\x00", 0x0000)]
    for data, want in vectors:
        got = crc16_xmodem(data)
        if got != want:
            sys.exit("CRC 自检失败：%r -> 期望 0x%04X，实际 0x%04X" % (data, want, got))

    # 增量式必须与整段式等价：MCU 侧的回读校验依赖它，不等价会永远失败。
    sample = b"12345678901234567890"
    inc = 0x0000
    for i in range(0, len(sample), 3):          # 故意按不整除的 3 字节切
        inc = crc16_update(inc, sample[i:i + 3])
    whole = crc16_xmodem(sample)
    if inc != whole:
        sys.exit("CRC 自检失败：增量式 0x%04X != 整段式 0x%04X" % (inc, whole))

    if verbose:
        print('CRC-16/XMODEM 自检通过（"123456789" -> 0x31C3，增量式与整段式一致）')
