#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
内网 OTA 服务器端 —— 监听 TCP，等设备连上来，把固件发给它。

用法：
    python ota_server.py                          # 默认监听 8080，发 A 区工程的 bin
    python ota_server.py --file 固件.bin          # 指定固件
    python ota_server.py --port 8080 --chunk 256 --delay 0.05

⚠️ 监听的是**内网穿透指向的那个内网端口**（当前 8080），不是外网的 25340。
   穿透配置：1bv0ss0284120.vicp.fun:25340  →  10.238.153.252:8080
   所以本脚本要在**跑穿透客户端的那台机器**上运行，并占用 8080。
   跑之前先关掉 Hercules —— 它占着同一个端口。

分块发送：--chunk 每块字节数，--delay 每块之间的间隔（秒）。
现在给的是"小步慢发"，目的是先把 MCU 侧的 +IPD 流式解析验证对；
等 MCU 侧加上 ACK 流控（4b-2b），delay 就可以去掉。

报文格式（4b-2b）：
    MCU   → 服务器   "OTA_REQ\n"
    服务器 → MCU     "OTA <真实长度> <CRC16>\n"
                     <真实长度 字节固件>

    · 真实长度 = 固件 bin 的实际字节数，不是 Xmodem 128 字节对齐后的长度。
      （A 区 bin 是 13000 字节，Xmodem 口径是 13056 —— 两者别混用）
    · CRC16 = CRC-16/XMODEM（初值 0x0000、多项式 0x1021），4 位十六进制。
      覆盖范围恰好是 '\n' 之后的 <真实长度> 个字节，不含头部自身。
    · CRC 放在头部而不是尾部：MCU 边收边算、收完比对；放尾部则 MCU 无法区分
      "最后 2 字节是 CRC 还是载荷"。

设备发 PING（[t] 4b-2a 接收测试）时不带头，按原行为裸发 —— 让 [t] 继续能当回归测试用。

⚠️ --delay 不要调成 0：MCU 侧接收缓冲只有 512 字节且溢出是静默的，
   而且 G4_PayloadGet 逐字节消费是 O(n)（缓冲越满单字节越贵），满速下会自锁。
   要提速得先做 ACK 流控。
"""
import argparse
import os
import socket
import sys
import time

from crc16 import crc16_xmodem, self_test

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FW = os.path.join(REPO_ROOT, "1.1-(A区)串口测试程序", "Objects", "Project.bin")


def main():
    ap = argparse.ArgumentParser(description="内网 OTA 服务器")
    ap.add_argument("--port", type=int, default=8080, help="监听端口（穿透指向的内网端口，默认 8080）")
    ap.add_argument("--file", default=None, help="要下发的固件（默认 A 区工程的 Project.bin）")
    ap.add_argument("--chunk", type=int, default=256, help="每块字节数（默认 256）")
    ap.add_argument("--delay", type=float, default=0.05, help="块间延时秒数（默认 0.05）")
    args = ap.parse_args()

    fw = args.file or DEFAULT_FW
    if not os.path.exists(fw):
        sys.exit("找不到固件：%s" % fw)
    data = open(fw, "rb").read()
    print("固件 : %s  (%d 字节)" % (fw, len(data)))
    self_test()                     # 发之前先确认 CRC 实现是自己的预期
    print()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print("监听 : 0.0.0.0:%d" % args.port)
    print()
    print(">>> 现在去串口那边：发 w 进菜单 → 8 连WiFi")
    print(">>>   o  内网 OTA 下载（4b-2b，会自动连服务器并收固件）")
    print(">>>   t  接收测试（4b-2a，仍按裸发模式，可用来回归）")
    print(">>> 设备连上来后本脚本会自动开始发送")
    print()

    conn, addr = srv.accept()
    print("设备已连接: %s:%d" % addr)
    print()
    print("等设备发来请求再开始（避免它在接收循环外时白丢数据）...")

    # ⚠️ 关键：不能一连上就发。
    # 设备在 "9 连服务器" 时就建立了 TCP 连接，但那时它还在菜单里、
    # 没进接收循环 —— 这期间发出去的数据没人接，会整段丢掉。
    # 实测症状：13000 字节只收到 5576，而且丢的正好是开头 7424 字节（连续一整段）。
    # 所以先等设备主动发一个请求（[t] 命令会先发 PING），收到后再开始。
    conn.settimeout(60)
    try:
        hello = conn.recv(64)
    except socket.timeout:
        print("!! 等 60 秒没等到设备请求，直接开发（可能会丢开头）")
        hello = b""
    if hello:
        print("收到设备请求: %r" % hello)
    print()

    # 设备发 OTA_REQ 时，先回一个长度头，MCU 才知道该收多少、才有东西可校验。
    # 发 PING（[t]）时保持原行为裸发，让 4b-2a 继续能当回归测试用。
    header = b""
    if b"OTA_REQ" in hello:
        crc = crc16_xmodem(data)
        header = ("OTA %d %04X\n" % (len(data), crc)).encode("ascii")
        print("协议头 : %r" % header)
        print("         真实长度 %d 字节，CRC16 = 0x%04X" % (len(data), crc))
    else:
        print("未见 OTA_REQ —— 按 4b-2a 的裸发模式发（[t] 接收测试用）")
    print()

    sent = 0
    t0 = time.time()
    try:
        if header:
            conn.sendall(header)
        for off in range(0, len(data), args.chunk):
            blk = data[off:off + args.chunk]
            conn.sendall(blk)
            sent += len(blk)
            pct = sent * 100 // len(data)
            print("\r发送进度 %3d%%  (%d/%d 字节)" % (pct, sent, len(data)), end="", flush=True)
            if args.delay > 0:
                time.sleep(args.delay)
        print()
        print()
        print("发送完毕：%d 字节，用时 %.1f 秒" % (sent, time.time() - t0))
        if header:
            print(">>> 去看串口那边：期望 CRC 应等于 %04X，实际 CRC 也要相等"
                  % crc16_xmodem(data))
        else:
            print(">>> 去看串口那边打印的「共收到 N 字节」——N 应该等于 %d" % len(data))
        print()
        # 保持连接一会儿，方便设备侧打印完摘要
        time.sleep(2)
    finally:
        conn.close()
        srv.close()


if __name__ == "__main__":
    main()
