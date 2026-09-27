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
"""
import argparse
import os
import socket
import sys
import time

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

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print("监听 : 0.0.0.0:%d" % args.port)
    print()
    print(">>> 现在去串口那边：发 w 进菜单 → 8 连WiFi → 9 连服务器 → t 接收测试")
    print(">>> 设备连上来后本脚本会自动开始发送")
    print()

    conn, addr = srv.accept()
    print("设备已连接: %s:%d" % addr)
    print()

    sent = 0
    t0 = time.time()
    try:
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
        print(">>> 去看串口那边打印的「共收到 N 字节」——N 应该等于 %d" % len(data))
        print()
        # 保持连接一会儿，方便设备侧打印完摘要
        time.sleep(2)
    finally:
        conn.close()
        srv.close()


if __name__ == "__main__":
    main()
