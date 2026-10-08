#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MD5 PC 侧对账 —— 照 test_crc_equiv.py / test_sign.py 的路子。

验两件事：
  1. A 区工程里**真实的** md5.c 能过 RFC 1321 的标准向量。
  2. 对**真实的固件文件**，用多种分片大小跑"边收边算"，
     结果与 Python hashlib 一致 —— 且和 OneNET 平台上记录的 md5 一致。

第 2 条是关键：一次性算对、分片算错是很常见的 bug，
只用一个分片大小是测不出来的，所以这里跑 1024 / 256 / 7 三种。

用法：
    python scripts/test_md5.py
    python scripts/test_md5.py "testbin/app_v1.1.0.bin"
"""
import base64
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HW = os.path.join(ROOT, "1.1-(A区)串口测试程序", "Hardware")
HARNESS = os.path.join(HERE, "test_md5_pc.c")

SOURCES = ["md5.c", "md5.h"]
SHIM = """#ifndef __STM32F10X_H
#define __STM32F10X_H
#include <stdint.h>
#include <stddef.h>
#endif
"""
GCC_CANDIDATES = ["gcc", r"D:\MinGW\bin\gcc.exe", r"C:\MinGW\bin\gcc.exe"]

# OneNET 上那个升级包（app_v1.1.0.bin）的记录值，来自 /check 的响应
PLATFORM_MD5 = "36c72ba22c830a7dcd0c8abb5b8b6414"
PLATFORM_SIZE = 13028

# 分片大小：1024 是 M4 实际用的；256 是 W25Q64 页大小；7 是刻意不对齐的怪值，
# 专门用来暴露"跨分片续算"里对齐/取模写错的问题。
CHUNK_SIZES = [1024, 256, 7]


def find_gcc():
    for c in GCC_CANDIDATES:
        exe = shutil.which(c) if os.sep not in c else (c if os.path.exists(c) else None)
        if exe:
            return exe
    return None


def build(gcc):
    tmp = tempfile.mkdtemp(prefix="md5build_")
    for f in SOURCES:
        shutil.copy2(os.path.join(HW, f), os.path.join(tmp, f))
    with open(os.path.join(tmp, "stm32f10x.h"), "w", encoding="utf-8") as f:
        f.write(SHIM)
    shutil.copy2(HARNESS, os.path.join(tmp, "test_md5_pc.c"))
    exe = os.path.join(tmp, "t.exe")
    p = subprocess.run([gcc, "-Wall", "-O1", "-std=gnu99", "-o", exe,
                        os.path.join(tmp, "test_md5_pc.c"),
                        os.path.join(tmp, "md5.c")],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print("=== gcc 编译失败 ===")
        print(p.stderr)
        sys.exit(1)
    if p.stderr.strip():
        print("=== gcc 警告 ===")
        print(p.stderr.strip())
    return tmp, exe


def run(exe, fw=None, chunk=None):
    args = [exe]
    if fw:
        args.append(fw)
        if chunk:
            args.append(str(chunk))
    r = subprocess.run(args, capture_output=True, text=True)
    kv = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    return kv, r.returncode


def main():
    gcc = find_gcc()
    if not gcc:
        sys.exit("找不到 gcc。")
    print("gcc: %s\n" % gcc)

    fw = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "testbin", "app_v1.1.0.bin")
    if not os.path.isabs(fw):
        fw = os.path.join(ROOT, fw)
    if not os.path.exists(fw):
        sys.exit("固件文件不存在：%s\n（先编出 A 区的 .bin，或用参数指定）" % fw)

    tmp, exe = build(gcc)
    try:
        print("=" * 74)
        print("① C 侧标准向量自检（RFC 1321，6 条）")
        print("=" * 74)
        kv, rc = run(exe)
        v = kv.get("MD5_SELFTEST", "?")
        print("   MD5_SELFTEST = %s   %s" % (v, "✅ 通过" if v == "0" else "❌ 失败 rc=%s" % v))
        print()

        py_md5 = hashlib.md5(open(fw, "rb").read()).hexdigest()
        size = os.path.getsize(fw)
        print("=" * 74)
        print("② 真实固件分片累加（%s，%d 字节）" % (os.path.basename(fw), size))
        print("=" * 74)
        print("   Python hashlib : %s" % py_md5)
        print()

        ok = True
        for ck in CHUNK_SIZES:
            kv, _ = run(exe, fw, ck)
            c_md5 = kv.get("FILE_MD5", "")
            same = (c_md5 == py_md5)
            ok = ok and same
            print("   分片 %4d 字节 -> %d 片  C=%s  %s"
                  % (ck, int(kv.get("FILE_CHUNKS", 0)), c_md5,
                     "✅" if same else "❌ 与 hashlib 不一致"))

        print()
        print("=" * 74)
        print("③ 与 OneNET 平台上记录的值对账")
        print("=" * 74)
        same_plat = (py_md5 == PLATFORM_MD5)
        print("   平台记录        : %s  (%d 字节)" % (PLATFORM_MD5, PLATFORM_SIZE))
        print("   本地文件        : %s  (%d 字节)" % (py_md5, size))
        print("   -> %s" % ("✅ 一致 —— 设备下载后算出的 MD5 会和这个对上"
                           if same_plat and size == PLATFORM_SIZE
                           else "⚠️ 不一致（换过固件？那就以本地这份为准重新上传升级包）"))
        print()
        print("=" * 74)
        print("总判定: %s" % ("✅ 全部通过" if (v == "0" and ok) else "❌ 有失败项"))
        print("=" * 74)
        return 0 if (v == "0" and ok) else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
