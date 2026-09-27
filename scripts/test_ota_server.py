# -*- coding: utf-8 -*-
"""用模拟设备验证 ota_server.py 的协议，不需要硬件。

跑三条路径：
  1) 发 "OTA_REQ\\n"  → 应收到 "OTA <len> <crc>\\n" + 固件，长度/内容/CRC 全部核对
  2) 发 "PING\\r\\n"  → 应**不带头**，裸发固件（4b-2a 回归路径）
  3) "OTA_REQ" + --corrupt  → 头部仍声明原始 CRC，实收数据被翻了一位，
     两者必须对不上（证明"CRC 通过"这件事本身是有意义的：
     正常路径下服务器按自己发的文件算 CRC，永远自洽，压根测不出校验有没有生效）
"""
import os, socket, subprocess, sys, time

ROOT = r"d:\develop\stm32-ota固件升级"
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.stdout.reconfigure(encoding="utf-8")

from crc16 import crc16_xmodem, self_test

PORT = 18080
FW = os.path.join(ROOT, r"1.1-(A区)串口测试程序", "Objects", "Project.bin")


def run_case(req, expect_header, extra=None, expect_crc_match=True, fw=None):
    srv = subprocess.Popen(
        [sys.executable, "ota_server.py", "--port", str(PORT), "--delay", "0.001"]
        + (extra or []),
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
        data = open(fw or FW, "rb").read()
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
            print("  头部声明 0x%04X / 原始文件 0x%04X / 实收 0x%04X"
                  % (hdr_crc, want, got))
            if expect_crc_match:
                good = ok and hdr_len == len(data) and want == hdr_crc == got
            else:
                # 注错模式：头声明的是**原始**数据的 CRC，实收的必须对不上；
                # 而且只应差被翻转的那一个字节。
                diff = sum(1 for x, y in zip(payload, data) if x != y)
                print("  注错校验 : 头部==原始 %s / 实收!=头部 %s / 差异字节数 %d"
                      % ("✓" if hdr_crc == want else "✗",
                         "✓" if got != hdr_crc else "✗", diff))
                good = (hdr_len == len(data)) and (hdr_crc == want) \
                       and (got != hdr_crc) and (diff == 1)
            print("  用例判定 : %s" % ("通过 ✓" if good else "失败 ✗"))
            return good
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
print("=" * 60)
print("用例 3：--corrupt 注错 —— 头部声明原始 CRC，实收必须对不上")
print("=" * 60)
r3 = run_case(b"OTA_REQ\n", True, extra=["--corrupt", "5000"], expect_crc_match=False)

print("结果：用例1 %s / 用例2 %s / 用例3 %s"
      % ("通过" if r1 else "失败", "通过" if r2 else "失败", "通过" if r3 else "失败"))
sys.exit(0 if (r1 and r2 and r3) else 1)
