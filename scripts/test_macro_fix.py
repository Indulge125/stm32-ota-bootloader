# -*- coding: utf-8 -*-
"""用 gcc 实测 main.h 的分区宏展开对不对。

为什么值得单独测：这几个宏是"链接期常量"，编译不会报错、链接也不会报错，
但算出来的值可以是错的。4b-2b-1 就栽在这上面 ——
(uint32_t)MyFlash_A_Page_Num * MyFlash_Page_Size 展开成 (uint32_t)64 - 36*1024，
无符号回绕成 42 亿，"长度超容量"永远为假，容量检查形同虚设。
测试 D 拿 31468 字节固件打进来才暴露。

所以这里直接从 main.h 抽宏原文编译运行，而不是靠眼睛看。

⚠ 本测试硬编码了当前分区（B 28 页 / A 36 页）。
  改分区之后必须同步改下面 BODY 里的期望值，否则它会正确地失败。
"""
import os, re, subprocess, sys

ROOT = r"d:\develop\stm32-ota固件升级"
MAIN_H = os.path.join(ROOT, r"6-串口IAP功能\User\main.h")
WORK = os.path.join(ROOT, "_tools", "_cchk")
GCC = r"D:\MinGW\bin\gcc.exe"
sys.stdout.reconfigure(encoding="utf-8")
os.makedirs(WORK, exist_ok=True)

src = open(MAIN_H, encoding="utf-8-sig").read().replace("\r\n", "\n")
macros = [l.split("//")[0].rstrip() for l in src.split("\n") if l.startswith("#define MyFlash_")]
if len(macros) < 6:
    sys.exit("★ 从 main.h 只抽到 %d 个宏，路径或格式不对" % len(macros))
print("从 main.h 抽到 %d 个宏" % len(macros))

BODY = r'''
int main(void)
{
	uint32_t limit = (uint32_t)MyFlash_A_Page_Num * MyFlash_Page_Size;
	int bad = 0;

	printf("limit = %u  (0x%08X)   应为 38912 (0x9800)\n", limit, limit);
	if(limit != 38912u) { printf("  ★ 错\n"); bad = 1; }

	printf("A 区页数   = %u            应为 38\n", (unsigned)MyFlash_A_Page_Num);
	if(MyFlash_A_Page_Num != 38) { printf("  ★ 错\n"); bad = 1; }

	printf("A 区起址   = 0x%08X   应为 0x08006800\n", (unsigned)MyFlash_A_Start_Address);
	if(MyFlash_A_Start_Address != 0x08006800u) { printf("  ★ 错\n"); bad = 1; }

	printf("A 区起始页 = %u            应为 26\n", (unsigned)MyFlash_A_Start_Page);
	if(MyFlash_A_Start_Page != 26) { printf("  ★ 错\n"); bad = 1; }

	printf("\n用 limit 判定长度：\n");
	printf("  45000 字节 -> %s\n", (45000u > limit) ? "拦下 ✓" : "★ 没拦（就是之前那个 bug）");
	if(!(45000u > limit)) bad = 1;
	printf("  13000 字节 -> %s\n", (13000u > limit) ? "★ 误拦" : "放行 ✓");
	if(13000u > limit) bad = 1;
	printf("  38912 字节 -> %s\n", (38912u > limit) ? "★ 误拦" : "放行 ✓");
	if(38912u > limit) bad = 1;

	printf("\n%s\n", bad ? "★★ 有错" : "★ 宏展开全部正确");
	return bad;
}
'''

cfile = os.path.join(WORK, "m.c")
open(cfile, "w", encoding="utf-8").write(
    "#include <stdio.h>\n#include <stdint.h>\n" + "\n".join(macros) + "\n" + BODY)

exe = os.path.join(WORK, "m.exe")
r = subprocess.run([GCC, "-O0", "-o", exe, cfile], capture_output=True)
if r.returncode:
    print("★ gcc 编译失败：\n" + (r.stdout + r.stderr).decode("utf-8", "replace"))
    sys.exit(1)
sys.exit(subprocess.run([exe]).returncode)
