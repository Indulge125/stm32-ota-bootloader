# -*- coding: utf-8 -*-
"""用模拟设备验证 ota_server.py 的协议，不需要硬件。

跑两条路径：
  1) 发 "OTA_REQ\\n"  → 应收到 "OTA <len> <crc>\\n" + 固件，长度/内容/CRC 全部核对
  2) 发 "PING\\r\\n"  → 应**不带头**，裸发固件（4b-2a 回归路径）
"""
import os, socket, subprocess, sys, time

ROOT = r"d:\develop\stm32-ota固件升级"
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.stdout.reconfigure(encoding="utf-8")

from crc16 import crc16_xmodem, self_test

PORT = 18080
FW = os.path.join(ROOT, r"1.1-(A区)串口测试程序", "Objects", "Project.bin")


def run_case(req, expect_header):
    srv = subprocess.Popen(
        [sys.executable, "ota_server.py", "--port", str(PORT), "--delay", "0.001"],
        cwd=os.path.join(ROOT, "scripts"),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace")
    time.sleep(1.5)                     # 等它 bind + listen

    try:
        c = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        c.settimeout(20)
        c.sendall(req)

        buf = b""
        # 若期望有头，先收到 '\n' 为止
        if expect_header:
            while b"\n" not in buf:
                chunk = c.recv(4096)
                if not chunk:
                    raise RuntimeError("连接提前关闭，没收到协议头")
                buf += chunk
            line, rest = buf.split(b"\n", 1)
            print("  收到的头 : %r" % line)
            parts = line.decode("ascii").split()
            assert parts[0] == "OTA", "头不是 OTA 开头: %r" % line
            hdr_len, hdr_crc = int(parts[1]), int(parts[2], 16)
        else:
            rest = b""
            hdr_len = hdr_crc = None

        # 收满固件
        data = open(FW, "rb").read()
        while len(rest) < len(data):
            chunk = c.recv(65536)
            if not chunk:
                break
            rest += chunk
        c.close()

        payload = rest[:len(data)]
        ok = (payload == data)
        print("  固件     : %d 字节，逐字节比对 %s" % (len(payload), "一致 ✓" if ok else "不一致 ✗"))

        if expect_header:
            print("  头部长度 : %d（文件 %d）%s" % (hdr_len, len(data),
                  "✓" if hdr_len == len(data) else "✗"))
            want = crc16_xmodem(data)
            got = crc16_xmodem(payload)
            print("  期望 CRC : 0x%04X / 头部声明 0x%04X / 实收 0x%04X  %s"
                  % (want, hdr_crc, got, "✓" if want == hdr_crc == got else "✗"))
            return ok and hdr_len == len(data) and want == hdr_crc == got
        else:
            print("  未带头   : ✓（4b-2a 裸发路径）")
            return ok
    finally:
        srv.terminate()
        try:
            out = srv.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            srv.kill(); out = ""
        print("  --- 服务器输出 ---")
        for l in (out or "").splitlines():
            print("    | " + l)
        print()


self_test()
print("=" * 60)
print("用例 1：OTA_REQ 应当带头")
print("=" * 60)
r1 = run_case(b"OTA_REQ\n", True)
print("=" * 60)
print("用例 2：PING 应当不带头（4b-2a 回归）")
print("=" * 60)
r2 = run_case(b"PING\r\n", False)

print("结果：用例1 %s / 用例2 %s" % ("通过" if r1 else "失败", "通过" if r2 else "失败"))
sys.exit(0 if (r1 and r2) else 1)
