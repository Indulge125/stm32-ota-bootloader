#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Token 签名 PC 侧对账 —— 照 test_crc_equiv.py 的思路做。

做法：把 A 区工程里**真实的** sha1.c / base64.c / onenet_token.c 抽出来，
用 gcc 编译成 PC 程序跑自检，再和 onenet_token_vectors.json 逐字节对账。

为什么要这样：
    改签名算法之后烧板子验证一轮要 20 分钟；在 PC 上跑一次 2 秒。
    "协议对不对"和"硬件/链路对不对"必须能分开，否则两个方向同时猜。

用法：
    python scripts/test_sign.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import base64
import hmac
import hashlib
import urllib.parse

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HW = os.path.join(ROOT, "1.1-(A区)串口测试程序", "Hardware")
VECTORS = os.path.join(HERE, "onenet_token_vectors.json")
HARNESS = os.path.join(HERE, "test_sign_pc.c")

SOURCES = ["sha1.c", "sha1.h", "base64.c", "base64.h",
           "onenet_token.c", "onenet_token.h"]

# PC 编译时顶替 stm32f10x.h —— 只需要整数类型
SHIM = """#ifndef __STM32F10X_H
#define __STM32F10X_H
#include <stdint.h>
#include <stddef.h>
#endif
"""

GCC_CANDIDATES = ["gcc", r"D:\MinGW\bin\gcc.exe", r"C:\MinGW\bin\gcc.exe"]


def find_gcc():
    for c in GCC_CANDIDATES:
        exe = shutil.which(c) if os.sep not in c else (c if os.path.exists(c) else None)
        if exe:
            return exe
    return None


def build_and_run(gcc):
    tmp = tempfile.mkdtemp(prefix="signbuild_")
    try:
        for f in SOURCES:
            src = os.path.join(HW, f)
            if not os.path.exists(src):
                sys.exit("缺少源文件：%s" % src)
            shutil.copy2(src, os.path.join(tmp, f))
        with open(os.path.join(tmp, "stm32f10x.h"), "w", encoding="utf-8") as f:
            f.write(SHIM)
        shutil.copy2(HARNESS, os.path.join(tmp, "test_sign_pc.c"))

        exe = os.path.join(tmp, "test_sign.exe")
        cmd = [gcc, "-Wall", "-O1", "-std=gnu99",
               "-o", exe,
               os.path.join(tmp, "test_sign_pc.c"),
               os.path.join(tmp, "sha1.c"),
               os.path.join(tmp, "base64.c"),
               os.path.join(tmp, "onenet_token.c")]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            print("=== gcc 编译失败 ===")
            print(p.stderr)
            return None, p.stderr
        if p.stderr.strip():
            print("=== gcc 警告 ===")
            print(p.stderr.strip())

        r = subprocess.run([exe], capture_output=True, text=True)
        return r.stdout, None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def py_reference(access_key_b64, res, et):
    """纯 Python 再算一遍，作为独立的第三方对照"""
    key = base64.b64decode(access_key_b64)
    sfs = "%d\nsha1\n%s\n2022-05-01" % (et, res)
    sig = base64.b64encode(hmac.new(key, sfs.encode(), hashlib.sha1).digest()).decode()
    return "version=2022-05-01&res=%s&et=%d&method=sha1&sign=%s" % (
        urllib.parse.quote(res, safe=""), et, urllib.parse.quote(sig, safe=""))


def main():
    gcc = find_gcc()
    if not gcc:
        sys.exit("找不到 gcc。请确认已装 MinGW，或改 GCC_CANDIDATES 里的路径。")
    print("gcc: %s\n" % gcc)

    out, err = build_and_run(gcc)
    if out is None:
        sys.exit(1)

    # --- 解析 C 侧输出 ---
    kv = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()

    print("=" * 74)
    print("① C 侧分层自检（非 0 即失败，数字是第一个失败的用例编号）")
    print("=" * 74)
    for k, desc in [("SHA1_SELFTEST", "SHA1 + HMAC-SHA1（6 个向量）"),
                    ("BASE64_SELFTEST", "Base64 编解码（9 个向量）"),
                    ("TOKEN_SELFTEST", "完整 Token 构造")]:
        v = kv.get(k, "?")
        print("   %-18s %-34s %s" % (k, desc, "✅ 通过" if v == "0" else "❌ 失败 rc=%s" % v))

    print()
    print("=" * 74)
    print("② 完整 Authorization 三方对账")
    print("=" * 74)
    with open(VECTORS, encoding="utf-8") as f:
        vec = json.load(f)
    expect = vec["token_case"]["expected_authorization"]
    got = kv.get("AUTH_SYNTHETIC", "")
    ref = py_reference(vec["token_case"]["access_key_b64"],
                       vec["token_case"]["res"], vec["token_case"]["et"])

    print("   fixture (JSON)  : %s" % expect)
    print("   C 侧算出的      : %s" % got)
    ok1 = (got == expect)
    print("   -> %s" % ("✅ 一致" if ok1 else "❌ 不一致"))

    print()
    print("   Python 独立重算 : %s" % ref)
    ok2 = (ref == expect)
    print("   -> %s" % ("✅ 与 fixture 一致" if ok2 else "❌ 与 fixture 不一致"))

    print()
    print("=" * 74)
    print("③ 边界用例")
    print("=" * 74)
    # 长密钥（>64 字节）走 HMAC 的"先摘要"分支
    lk = "A" * 100
    want_lk = py_reference(lk, "products/PID/devices/DN", 1767225600)
    got_lk = kv.get("AUTH_LONGKEY", "")
    print("   长密钥(100B) rc=%s" % kv.get("LONGKEY_BUILD_RC", "?"))
    print("   C : %s" % got_lk)
    print("   Py: %s" % want_lk)
    ok3 = (got_lk == want_lk and got_lk != "")
    print("   -> %s" % ("✅ 一致" if ok3 else "❌ 不一致"))

    tiny = kv.get("TINY_BUFFER_RC", "?")
    ok4 = (tiny == "2")
    print("   极小缓冲区(16B) rc=%s  -> %s" % (tiny, "✅ 正确拒绝" if ok4 else "❌ 应当拒绝"))

    print()
    print("=" * 74)
    allok = (kv.get("ALL_PASS") == "1") and ok1 and ok2 and ok3 and ok4
    print("总判定: %s" % ("✅ 全部通过" if allok else "❌ 有失败项"))
    print("=" * 74)
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
