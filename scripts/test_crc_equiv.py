# -*- coding: utf-8 -*-
"""验证 CRC 重构零回归：把 boot.c 里**真实的** CRC 函数抽到 PC 上编译运行，
再和 scripts/crc16.py 对账。

为什么要这么验：4b-2b 把 Xmodem_CRC16() 拆成了 Xmodem_CRC16_Update()（增量式），
动的是**已验证代码**（Xmodem 102 包全 ACK 走的就是它）。"零回归"不能靠肉眼比对，
要在真实数据上跑出同一个数。

对三种调用方式各跑一遍 —— 正是固件里实际用到的三种：
  1. 整段       Xmodem 收包校验
  2. 每 256 续算 从 W25Q64 回读分块重算
  3. 逐字节续算  边收边算
"""
import os, re, subprocess, sys, tempfile

ROOT = r"d:\develop\stm32-ota固件升级"
BOOT_C = os.path.join(ROOT, r"6-串口IAP功能\Hardware\boot.c")
BIN = os.path.join(ROOT, r"1.1-(A区)串口测试程序\Objects\Project.bin")
WORK = os.path.join(ROOT, "_tools", "_cchk")
GCC = r"D:\MinGW\bin\gcc.exe"
sys.stdout.reconfigure(encoding="utf-8")
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from crc16 import crc16_xmodem, self_test

os.makedirs(WORK, exist_ok=True)
_fail = []


def extract_fn(src, sig):
    """从 C 源码里按函数签名抽出完整函数体（到行首的 } 为止）。"""
    i = src.find(sig)
    if i < 0:
        return None
    j = src.find("\n}", i)
    if j < 0:
        return None
    return src[i:j + 2]


src = open(BOOT_C, encoding="utf-8-sig").read().replace("\r\n", "\n")

fn_update = extract_fn(src, "uint16_t Xmodem_CRC16_Update(uint16_t crc")
fn_whole = extract_fn(src, "uint16_t Xmodem_CRC16(uint8_t *data")
if not fn_update or not fn_whole:
    sys.exit("★ 抽不出 CRC 函数 —— boot.c 里签名变了？")
print("已从 boot.c 抽出两个函数（各 %d / %d 字符）"
      % (len(fn_update), len(fn_whole)))

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

%s

%s

int main(int argc, char **argv)
{
	FILE *f;
	long n, i;
	uint8_t *buf;
	uint16_t c;

	if(argc < 2) return 2;
	f = fopen(argv[1], "rb");
	if(!f) return 3;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = (uint8_t *)malloc(n);
	if(fread(buf, 1, n, f) != (size_t)n) return 4;
	fclose(f);

	printf("whole %%04X\n", Xmodem_CRC16(buf, (uint32_t)n));

	c = 0x0000;
	for(i = 0; i < n; i += 256)
	{
		uint32_t m = (n - i) > 256 ? 256 : (uint32_t)(n - i);
		c = Xmodem_CRC16_Update(c, buf + i, m);
	}
	printf("chunk %%04X\n", c);

	c = 0x0000;
	for(i = 0; i < n; i ++)
	{
		c = Xmodem_CRC16_Update(c, buf + i, 1);
	}
	printf("byte  %%04X\n", c);

	c = 0x0000;
	printf("empty %%04X\n", Xmodem_CRC16_Update(c, buf, 0));

	free(buf);
	return 0;
}
''' % (fn_update, fn_whole)

cfile = os.path.join(WORK, "crc_test.c")
exe = os.path.join(WORK, "crc_test.exe")
open(cfile, "w", encoding="utf-8").write(HARNESS)

r = subprocess.run([GCC, "-O0", "-o", exe, cfile], capture_output=True)
if r.returncode:
    print("★ gcc 编译失败：\n" + (r.stdout + r.stderr).decode("utf-8", "replace")[:1500])
    sys.exit(1)
print("gcc 编译通过")

self_test()
data = open(BIN, "rb").read()
print("测试数据：%s（%d 字节）" % (os.path.basename(BIN), len(data)))

r = subprocess.run([exe, BIN], capture_output=True, text=True)
got = {}
for line in r.stdout.splitlines():
    k, v = line.split()
    got[k] = int(v, 16)
print("C 侧结果：%s" % {k: "0x%04X" % v for k, v in got.items()})

want = crc16_xmodem(data)
print("Python 结果：0x%04X" % want)
print()

names = {"whole": "整段（Xmodem 收包）",
         "chunk": "每 256 字节续算（回读校验）",
         "byte":  "逐字节续算（边收边算）",
         "empty": "空输入"}
for k, label in names.items():
    expect = want if k != "empty" else 0x0000
    ok = got.get(k) == expect
    print("  %-28s C=0x%04X 期望=0x%04X  %s"
          % (label, got.get(k, -1), expect, "OK" if ok else "★ 不符"))
    if not ok:
        _fail.append(label)

print()
if _fail:
    print("★★ %d 项不符" % len(_fail))
    sys.exit(1)
print("★ CRC 重构零回归：C 侧三种调用方式与 Python 全部一致（0x%04X）" % want)
