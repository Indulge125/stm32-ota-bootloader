#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""确保工程里所有含非 ASCII 的 C 源文件都是 UTF-8 带 BOM。

为什么需要这个：
    ARMCC5 默认按系统 ANSI 代码页解码源文件。源文件是 UTF-8 但**没有 BOM** 时，
    字符串字面量里的中文会被误解析，报 warning #870-D
    "invalid multibyte character sequence"，而且串口真的会发出错字节。

为什么不能只手动补一次：
    Keil uVision 的编辑器按自己的 Encoding 设置保存文件，默认会把 BOM 抹掉。
    也就是说 —— 你在 Keil 里改一次代码，BOM 就丢一次。
    靠人记着补，迟早会漏。

用法一（手动）：python scripts/ensure_bom.py
用法二（挂到 Keil，推荐，一劳永逸）：
    Options for Target → User → Before Build/Rebuild 填：
        python ..\\scripts\\ensure_bom.py "..\\1.1-(A区)串口测试程序"
    （路径按工程位置调整；多个工程可用分号分隔多个参数）

退出码恒为 0 —— 它是构建前置修整，不该因为"没东西要改"而中断编译。
"""
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BOM = b"\xef\xbb\xbf"
# 编译产物目录，本来就不该动
SKIP_DIRS = {"Objects", "Listings", "DebugConfig", "RTE", "__pycache__", ".git"}

# 存档 / 实验样本目录 —— 这些地方「没有 BOM」或「是 GBK」是**故意的**，
# 补 BOM 会破坏它们的用途，必须整个跳过：
#   OTA升级原版/    改动前的原版存档；加了 BOM 就不再是"原版"，失去对照意义
#   _tools/         一次性脚本目录，里面有 _enctest/ 编码实验样本
#                   （A_utf8_nobom.c / B_utf8_nobom.c / A_gbk.c / gbk_src.c
#                    / utf8_无BOM_默认.c —— 名字就写明了它们该是什么编码）
SKIP_PATH_PARTS = {"_tools", "OTA升级原版"}

EXTS = (".c", ".h")


def has_non_ascii(b):
    return any(x > 127 for x in b)


def fix_tree(root):
    changed, ok, ascii_only = [], 0, 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and d not in SKIP_PATH_PARTS]
        for fn in filenames:
            if not fn.lower().endswith(EXTS):
                continue
            p = os.path.join(dirpath, fn)
            try:
                b = open(p, "rb").read()
            except OSError as e:
                print("  跳过 %s: %s" % (p, e))
                continue

            if not has_non_ascii(b):
                ascii_only += 1
                continue
            if b.startswith(BOM):
                ok += 1
                continue

            open(p, "wb").write(BOM + b)
            changed.append(p)

    return changed, ok, ascii_only


def main():
    roots = sys.argv[1:] or ["."]
    total_changed = 0

    for root in roots:
        if not os.path.isdir(root):
            print("[ensure_bom] 跳过（不是目录）: %s" % root)
            continue
        changed, ok, ascii_only = fix_tree(root)
        base = os.path.basename(os.path.abspath(root))
        if changed:
            print("[ensure_bom] %s: 补了 %d 个文件的 BOM" % (base, len(changed)))
            for p in changed:
                print("    + %s" % os.path.relpath(p, root))
            total_changed += len(changed)
        else:
            print("[ensure_bom] %s: 无需修补（已有BOM %d 个 / 纯ASCII %d 个）"
                  % (base, ok, ascii_only))

    if total_changed:
        print("[ensure_bom] 共修补 %d 个文件 —— 请让 Keil 重新编译这些文件" % total_changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
