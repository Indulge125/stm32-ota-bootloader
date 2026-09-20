#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Xmodem-CRC 固件发送工具 —— 不依赖任何串口调试助手的图形界面。

用途：给 STM32 OTA BootLoader 下载 A 区固件。
动作序列（脚本全包，因为串口同时只能被一个程序占用）：
    1. 反复发送 'w' 抢 5 秒窗口，进入 BootLoader 命令行
    2. 发送 '2'，进入 Xmodem 接收模式
    3. 等待接收方的 'C'（Xmodem-CRC 握手信号）
    4. 逐包发送，每包等 ACK，收到 NAK 自动重传
    5. 发送 EOT 结束

用法：
    python xmodem_send.py --port COM8 --file "..\\1.1-(A区)串口测试程序\\Objects\\Project.bin"
    python xmodem_send.py --port COM8 --file xxx.bin --baud 4800

协议要点（与 boot.c 的实现严格对齐）：
    包格式 = SOH | 序号 | 255-序号 | 128 字节数据 | CRC高 | CRC低     共 133 字节
    CRC    = CRC-16/XMODEM，初值 0x0000，多项式 0x1021，覆盖 128 字节数据
    不足 128 字节的尾包用 0x1A(SUB) 填充
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，请先执行：pip install pyserial")

SOH, EOT, ACK, NAK, CAN, SUB = 0x01, 0x04, 0x06, 0x15, 0x18, 0x1A
CRC_REQ = 0x43  # 'C'，接收方请求 CRC 模式
BLOCK = 128


def crc16_xmodem(data: bytes) -> int:
    """CRC-16/XMODEM：初值 0x0000，多项式 0x1021，MSB 优先，不反转、不末异或。
    与 boot.c 的 Xmodem_CRC16() 逐位等价。"""
    crc = 0x0000
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def self_test() -> None:
    """用已知向量验证 CRC 实现。对不上就别往下发了。"""
    vectors = [(b"123456789", 0x31C3), (b"", 0x0000), (b"\x00", 0x0000)]
    for data, want in vectors:
        got = crc16_xmodem(data)
        if got != want:
            sys.exit("CRC 自检失败：%r -> 期望 0x%04X，实际 0x%04X" % (data, want, got))
    print("CRC-16/XMODEM 自检通过（\"123456789\" -> 0x31C3）")


def wait_for_byte(ser, timeout):
    """等一个有效响应字节；忽略接收方周期性发来的 'C' 和其它噪声。

    返回 (响应字节或 None, 沿途收到的其它字节列表)。
    后者用于诊断：若超时后只看到 'C'，说明包根本没被接收方接受
    （datalen 不是 133）—— 这比"没反应"有信息量得多。
    """
    others = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if v in (ACK, NAK, CAN):
            return v, others
        others.append(v)
    return None, others


def arm_bootloader(ser, log):
    """让板子进入 Xmodem 接收模式。

    板子可能处于两种状态，两种都要覆盖：
      A) 刚复位/上电，还在 BootLoader_Enter() 的 5 秒窗口内
         -> 发 'w' 才能进命令行
      B) 已经在命令行里等着（5 秒窗口早过了）
         -> 直接发 '2' 就行；此时 'w' 不匹配任何命令，被静默忽略、不会有任何回应

    ⚠️ 判据必须是接收方的 'C'，不能是"菜单文本出现" —— 板子已经在菜单里时
       不会再重打一遍菜单，等文本会永远等不到（踩过这个坑）。

    ⚠️ 'w' 只发 2 次：BootLoader 的事件环形缓冲只有 Num=10 个槽位
       （usart.h），且 BootLoader_Enter() 死等 5 秒期间主循环根本没跑、
       事件没人消费。发太多会把环形缓冲冲爆，In 指针绕回去覆盖尚未消费的
       槽位（里面存着 start/end 指针），之后 datalen 算成垃圾值 ——
       表现为数据包被静默丢弃，既无 ACK 也无 NAK（也踩过这个坑）。
    """
    for attempt in range(1, 5):
        hint = "" if attempt == 1 else "（请按一下板子的复位键）"
        log("第 %d 次尝试进入 Xmodem 模式 %s" % (attempt, hint))

        ser.reset_input_buffer()
        ser.write(b"w")          # 覆盖状态 A
        ser.flush()
        time.sleep(0.4)
        ser.write(b"w")
        ser.flush()
        time.sleep(0.4)

        ser.reset_input_buffer()
        ser.write(b"2")          # 覆盖状态 B（也是状态 A 进了菜单之后该发的）
        ser.flush()

        t0 = time.time()
        buf = b""
        while time.time() - t0 < 4.0:
            buf += ser.read(256)
            if CRC_REQ in buf:
                log("接收方已就绪（收到 'C' 握手信号）")
                return True
        log("  没等到 'C'")
    return False


def send_file(ser, path, log):
    data = open(path, "rb").read()
    total = (len(data) + BLOCK - 1) // BLOCK
    log("文件 %s，%d 字节，共 %d 包" % (os.path.basename(path), len(data), total))

    seq = 1
    sent = 0
    t_start = time.time()
    for idx in range(total):
        block = data[idx * BLOCK:(idx + 1) * BLOCK]
        block += bytes([SUB]) * (BLOCK - len(block))       # 尾包用 0x1A 填充
        crc = crc16_xmodem(block)
        packet = bytes([SOH, seq & 0xFF, (255 - seq) & 0xFF]) + block + \
                 bytes([crc >> 8, crc & 0xFF])

        for retry in range(10):
            ser.write(packet)
            ser.flush()
            r, others = wait_for_byte(ser, 8.0)
            if r == ACK:
                break
            if r == CAN:
                sys.exit("接收方发送了 CAN，传输被中止")
            if r is None:
                log("  第 %d 包超时，重传" % seq)
                if others:
                    shown = "".join(chr(c) if 32 <= c < 127 else "\\x%02X" % c
                                    for c in others[:40])
                    log("    超时期间收到 %d 个其它字节：%s" % (len(others), shown))
                    if all(c == CRC_REQ for c in others):
                        log("    全是 'C' -> 接收方还在等握手，说明这包它没当成完整包收下")
                else:
                    log("    完全没收到任何字节")
            else:
                log("  第 %d 包收到 NAK，重传" % seq)
        else:
            sys.exit("第 %d 包重传 10 次仍失败，检查波特率/接线" % seq)

        seq += 1
        sent += 1
        if sent % 10 == 0 or sent == total:
            pct = sent * 100 // total
            speed = sent * BLOCK / max(time.time() - t_start, 0.001)
            log("  进度 %3d%%  (%d/%d 包, %.0f 字节/秒)" % (pct, sent, total, speed))
    return len(data), total


def main():
    ap = argparse.ArgumentParser(description="Xmodem-CRC 固件发送工具")
    ap.add_argument("--port", required=True, help="串口号，如 COM8")
    ap.add_argument("--file", required=True, help="要发送的 .bin 文件")
    ap.add_argument("--baud", type=int, default=9600, help="波特率（默认 9600，与 BootLoader 一致）")
    args = ap.parse_args()

    def log(msg):
        print(msg, flush=True)

    if not os.path.exists(args.file):
        sys.exit("找不到文件：%s" % args.file)
    if os.path.getsize(args.file) == 0:
        sys.exit("文件是空的：%s" % args.file)

    self_test()

    log("打开 %s @ %d 8-N-1" % (args.port, args.baud))
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except Exception as e:
        sys.exit("打不开串口 %s：%s\n（串口助手占用了？先关掉）" % (args.port, e))

    try:
        if not arm_bootloader(ser, log):
            sys.exit("没能让 BootLoader 进入 Xmodem 模式。\n"
                     "  检查：① 板子是否已烧好 BootLoader ② 串口号是否为 %s ③ 关掉其它占用串口的程序" % args.port)
        size, blocks = send_file(ser, args.file, log)

        log("发送 EOT ...")
        for _ in range(10):
            ser.write(bytes([EOT]))
            ser.flush()
            r, _ = wait_for_byte(ser, 8.0)
            if r == ACK:
                break
        else:
            sys.exit("EOT 没收到 ACK，传输可能未确认")
        log("收到 ACK，传输完成")
        time.sleep(0.5)
        tail = ser.read(4096)
        if tail:
            log("设备后续输出：%s" % tail.decode("utf-8", "replace").strip())
        log("\n完成：%d 字节 / %d 包。板子应已自动复位并跳转到 A 区程序。" % (size, blocks))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
