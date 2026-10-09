#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通过 B 区 BootLoader 菜单设置/查询 OTA 版本号。

为什么需要脚本而不是手敲：
    BootLoader 要求版本号**正好 26 个字节**，且格式必须匹配
        sscanf(data, "VER-%d.%d.%d-%d/%d/%d-%d:%d")
    它按接收长度严格匹配 —— 多一个字节（比如串口工具自动补的回车）或少一个，
    命令会被**静默忽略**，没有任何报错提示。
    在 Tera Term 里手敲 26 个字符，几乎不可能保证一个不差。
    而这个通路本来就是给机器用的，不该靠手敲。

用法：
    python scripts/set_version.py                       # COM20 + 默认版本串
    python scripts/set_version.py --port COM3
    python scripts/set_version.py --version "VER-1.0.0-2026/10/09-12:00"
    python scripts/set_version.py --query-only          # 只查，不设

运行后如果板子不在菜单里，脚本会**持续尝试进入**（每 500ms 发一个小写 w），
所以你可以从容地按板子的复位键 —— 有 30 秒窗口。

⚠️ 为什么不更快地刷 w：BootLoader 进入命令行时是"死等 5 秒"，
   那段时间主循环不消费串口事件；高频刷字符会把它的接收环形缓冲冲爆，
   之后的数据包会被静默丢弃（README 的「已知限制」里记过这条）。
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# 默认版本串：正好 26 字节，格式匹配 VER-%d.%d.%d-%d/%d/%d-%d:%d
DEFAULT_VERSION = "VER-1.0.0-2026/09/20-15:30"
MENU_MARK = "擦除A区"          # 菜单里必然出现的一行
ENTER_WINDOW_S = 30            # 尝试进入菜单的总时长
W_INTERVAL_S = 0.5             # 发 w 的间隔（别更快，见文件头注释）


def read_all(ser, wait=0.4):
    """把当前缓冲区里的东西都读出来"""
    time.sleep(wait)
    n = ser.in_waiting
    return ser.read(n) if n else b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM20")
    ap.add_argument("--version", default=DEFAULT_VERSION)
    ap.add_argument("--query-only", action="store_true")
    args = ap.parse_args()

    if len(args.version) != 26:
        sys.exit("版本串必须是 26 个字符，当前 %d 个：%r" % (len(args.version), args.version))

    ser = serial.Serial(args.port, 9600, timeout=0.2)
    print("串口已打开: %s @9600" % args.port)
    print()
    print("正在尝试进入 BootLoader 菜单 ……")
    print("  > 如果板子不在菜单里，请现在**按一下板子的复位键**，有 %d 秒窗口" % ENTER_WINDOW_S)
    print()

    # ---- 1. 进入菜单 ----
    in_menu = False
    t0 = time.time()
    while time.time() - t0 < ENTER_WINDOW_S:
        ser.write(b"w")
        out = read_all(ser)
        if out:
            try:
                txt = out.decode("utf-8", "replace")
            except Exception:
                txt = repr(out)
            if MENU_MARK in txt:
                print("  ✓ 已进入菜单")
                in_menu = True
                break
            if txt.strip():
                print("  (收到: %s)" % txt.strip().replace("\n", " | ")[:100])
        time.sleep(W_INTERVAL_S)

    if not in_menu:
        ser.close()
        sys.exit("\n✗ %d 秒内没能进入菜单 —— 确认板子已上电、串口是 %s、波特率 9600"
                 % (ENTER_WINDOW_S, args.port))

    time.sleep(0.3)

    # ---- 2. 设置版本号 ----
    if not args.query_only:
        print()
        print("发送 3 （设置OTA版本号）……")
        ser.write(b"3")
        time.sleep(0.4)
        prompt = read_all(ser).decode("utf-8", "replace")
        if prompt.strip():
            print("  板子: %s" % prompt.strip().replace("\n", " | ")[:120])

        # ⚠️ 版本串**不带任何结尾**：多一个 \r 或 \n 就会变成 27/28 字节，
        #    BootLoader 的长度判断过不去，命令被静默忽略。
        print("发送版本串: %r  （%d 字节，故意不带换行）" % (args.version, len(args.version)))
        ser.write(args.version.encode("ascii"))

        t0 = time.time()
        got = ""
        while time.time() - t0 < 5:
            chunk = read_all(ser, 0.3).decode("utf-8", "replace")
            got += chunk
            if "版本正确" in got or "格式错误" in got:
                break
        got = got.strip()

        if "版本正确" in got:
            print("  ✓ 板子回执: 版本正确")
        elif "格式错误" in got:
            ser.close()
            sys.exit("  ✗ 板子回执: 版本号格式错误 —— 检查格式 VER-x.y.z-YYYY/MM/DD-HH:MM")
        else:
            print("  ⚠ 没等到明确的回执（收到: %r）" % got[:120])
            print("     可能是回执被上面的读取吃掉了，继续往下用查询验证")

    # ---- 3. 查询确认 ----
    print()
    print("发送 4 （查询OTA版本号）……")
    ser.write(b"4")
    t0 = time.time()
    got = ""
    while time.time() - t0 < 4:
        chunk = read_all(ser, 0.3).decode("utf-8", "replace")
        got += chunk
        if "版本号" in got and ("VER-" in got or "乱" in got):
            break
    ser.close()

    print()
    print("=" * 60)
    for line in got.splitlines():
        if line.strip():
            print("  板子> " + line.strip())
    print("=" * 60)

    if ("VER-" + args.version[4:]) in got or args.version in got:
        print("\n✓ 版本号已正确写入并读回")
        return 0
    print("\n✗ 读回的版本号不匹配 —— 把上面「板子>」那几行贴出来定位")
    return 1


if __name__ == "__main__":
    sys.exit(main())
