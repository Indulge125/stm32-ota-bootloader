# -*- coding: utf-8 -*-
"""用模拟设备验证 ota_server.py 的协议，不需要硬件。

跑三条路径：
  1) 发 "OTA_REQ\\n"  → 应收到 "OTA <len> <crc>\\n" + 固件，长度/内容/CRC 全部核对
  2) 发 "PING\\r\\n"  → 应**不带头**，裸发固件（4b-2a 回归路径）
  3) "OTA_REQ" + --corrupt  → 头部仍声明原始 CRC，实收数据被翻了一位，
     两者必须对不上（证明"CRC 通过"这件事本身是有意义的：
     正常路径下服务器按自己发的文件算 CRC，永远自洽，压根测不出校验有没有生效）
"""
import os, socket, subprocess, sys, tempfile, time

ROOT = r"d:\develop\stm32-ota固件升级"
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.stdout.reconfigure(encoding="utf-8")

from crc16 import crc16_xmodem, self_test

PORT = 18080
FW = os.path.join(ROOT, r"1.1-(A区)串口测试程序", "Objects", "Project.bin")


def run_case(req, expect_header, extra=None, expect_crc_match=True, fw=None):
    # 服务端的进度输出写到**临时文件**，不要用 subprocess.PIPE。
    #
    # ⚠️ 为什么：服务端每发一块就打一行"发送进度"，而本测试直到最后才读输出。
    #    Windows 的匿名管道缓冲只有约 4KB —— 固件 13000 字节时进度输出约 2.3KB，
    #    塞得下，所以一直没暴露；固件涨到 23628 字节后 93 行约 4.2KB 就填满了，
    #    服务端的 print 随即**阻塞**，传输停在中途，表现成"收不到数据超时"。
    #    这是脚手架的问题，不是协议的问题。写文件不会阻塞，事后也还能查。
    log = tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace")
    srv = subprocess.Popen(
        [sys.executable, "ota_server.py", "--port", str(PORT), "--delay", "0.001"]
        + (extra or []),
        cwd=os.path.join(ROOT, "scripts"),
        stdout=log, stderr=subprocess.STDOUT)
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
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill(); srv.wait(timeout=5)

        log.seek(0)
        out = log.read()
        log.close()

        print("  --- 服务器输出 ---")
        # 进度行会刷出几十上百条（用 \r 分隔，写进文件后 splitlines 会逐条拆开），
        # 只留最后一条 —— 前面那些不提供信息，只会把有用的行挤下去。
        lines = (out or "").splitlines()
        shown, i = [], 0
        while i < len(lines):
            if "发送进度" in lines[i]:
                j = i
                while j < len(lines) and "发送进度" in lines[j]:
                    j += 1
                shown.append(lines[j-1])
                i = j
            else:
                shown.append(lines[i]); i += 1
        for l in shown:
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
