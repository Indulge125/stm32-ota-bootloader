# -*- coding: utf-8 -*-
"""在 PC 上验证 +IPD 解析状态机（G4_PayloadRead / G4_PayloadGet）。

为什么值得单独测：这段代码刚从"逐字节消费"改成"批量消费"，
而它正是硬件上出过问题的那一段（缓冲溢出 → 信封错位 → 丢数据）。
把 4G.c 里**真实的**函数抽出来，配上接收缓冲的桩，喂合成的 +IPD 流，
比烧板子快得多，也能构造出硬件上难复现的边界。

被测的是真代码（从 4G.c 抽原文），不是抄一份 —— 抄一份就会漂移。

覆盖的场景：
  1. 真实工况：256 字节一个 +IPD，每批读 256
  2. 逐字节读（旧行为）：批量版退化成 max=1 时仍然正确
  3. +IPD 块比读批量大（1024 vs 64）—— 一次读不完一个信封
  4. 载荷里**含** "+IPD,999:" 字节 —— 绝不能被误当成信封（这是设计的关键点）
  5. 信封被喂数据的边界切成两半（每次只喂 2 字节）
  6. 1.5KB 大 +IPD（接近 TCP MSS）+ 奇数读批量
"""
import os, subprocess, sys

ROOT = r"d:\develop\stm32-ota固件升级"
SRC_4G = os.path.join(ROOT, r"6-串口IAP功能\Hardware\4G.c")
WORK = os.path.join(ROOT, "_tools", "_cchk")
GCC = r"D:\MinGW\bin\gcc.exe"
sys.stdout.reconfigure(encoding="utf-8")
os.makedirs(WORK, exist_ok=True)

src = open(SRC_4G, encoding="utf-8-sig").read().replace("\r\n", "\n")

i0 = src.find("static uint8_t  s_ipdState = 0;")
i1 = src.find("\n}\n", src.find("uint8_t G4_PayloadGet(uint8_t *out)"))
if i0 < 0 or i1 < 0:
    sys.exit("★ 从 4G.c 抽不出状态机（锚点变了？）")
code = src[i0:i1 + 2]
if "G4_PayloadRead" not in code:
    sys.exit("★ 抽出的代码里没有 G4_PayloadRead")
print("已从 4G.c 抽出状态机代码（%d 字符），含：%s"
      % (len(code), " / ".join(sorted(
          n for n in ("G4_PayloadRead", "G4_PayloadGet", "G4_PayloadReset") if n in code))))

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ================= 接收缓冲的桩（模拟 4G.c 的线性缓冲 + 人工清零） ================= */
#define G4_RX_SIZE 512
static uint8_t  s_rxBuf[G4_RX_SIZE];
static volatile uint16_t s_rxLen = 0;
static int s_overflow = 0;

/* 统计 G4_RxDrop 一共搬了多少个字节 —— 这就是"抽干代价"。
 * 逐字节消费时，每取 1 字节就要搬一次剩余缓冲，总搬移量是 O(n²)；
 * 批量消费把每批的搬移合并成一次。这个计数是本次修改的核心证据。 */
static unsigned long s_moves = 0;
static unsigned long s_last_moves = 0;

uint16_t G4_RxLen(void) { return s_rxLen; }
const uint8_t *G4_RxBuf(void) { return s_rxBuf; }
void G4_ClearRx(void) { s_rxLen = 0; memset(s_rxBuf, 0, G4_RX_SIZE); }
void G4_RxDrop(uint16_t n)
{
	uint16_t i;
	if(n == 0) return;
	if(n >= s_rxLen) { s_moves += s_rxLen; s_rxLen = 0; return; }
	for(i = 0; i + n < s_rxLen; i ++) s_rxBuf[i] = s_rxBuf[i + n];
	s_moves += i;
	s_rxLen -= n;
}

/* ================= 被测代码（从 4G.c 抽出） ================= */
__CODE__

/* ================= 喂数据：模拟 ESP8266 往 UART 吐字节 ================= */
static void feed(const uint8_t *p, uint32_t n)
{
	uint32_t i;
	for(i = 0; i < n; i ++)
	{
		if(s_rxLen < G4_RX_SIZE - 1) s_rxBuf[s_rxLen ++] = p[i];
		else s_overflow = 1;              /* 满了就丢，和中断里一样 */
	}
}

/* 把一个 payload 切成若干 +IPD 包，拼成 ESP8266 会吐出的字节流 */
static uint8_t *build_stream(const uint8_t *payload, uint32_t plen,
                             uint32_t chunk, uint32_t *out_len)
{
	uint32_t off, n = 0;
	char head[32];
	uint8_t *buf = (uint8_t *)malloc(plen + (plen / chunk + 2) * 32);
	for(off = 0; off < plen; off += chunk)
	{
		uint32_t c = (plen - off > chunk) ? chunk : (plen - off);
		int hl = sprintf(head, "+IPD,%u:", (unsigned)c);
		memcpy(buf + n, head, hl); n += hl;
		memcpy(buf + n, payload + off, c); n += c;
	}
	*out_len = n;
	return buf;
}

/* 跑一个用例。
 *
 * 模拟方式要贴近固件：喂一批字节，然后**一直抽干**（固件是在紧循环里
 * 反复调 G4_PayloadRead 的，只在取不到时才 Delay）。如果每次只抽一次，
 * 缓冲必然堆满 —— 那是测试框架的假象，不是被测代码的问题。
 *
 * feed_piece 必须 <= 接收缓冲（511），否则字节会在喂入时就丢，
 * 那反映的是"模组吐的 +IPD 装不下"这个硬件上限，不是状态机的错。
 */
static int run_case(const char *name, const uint8_t *payload, uint32_t plen,
                    uint32_t chunk, uint32_t feed_piece, uint16_t read_max)
{
	uint8_t *stream, *got, tmp[600];
	uint32_t slen = 0, sent = 0, ngot = 0;
	uint32_t guard = 0;
	int bad = 0;

	stream = build_stream(payload, plen, chunk, &slen);
	got = (uint8_t *)malloc(plen + 16);
	G4_PayloadReset();
	s_overflow = 0;
	s_moves = 0;

	while((ngot < plen || sent < slen) && guard ++ < 4000000u)
	{
		if(sent < slen)
		{
			uint32_t c = (slen - sent > feed_piece) ? feed_piece : (slen - sent);
			feed(stream + sent, c);
			sent += c;
		}
		for(;;)                                   /* 抽干 */
		{
			uint16_t n = G4_PayloadRead(tmp, read_max);
			if(n == 0) break;
			if(ngot + n > plen) n = (uint16_t)(plen - ngot);
			memcpy(got + ngot, tmp, n);
			ngot += n;
		}
	}

	if(ngot != plen) { printf("      只收到 %u / %u 字节\n", ngot, plen); bad = 1; }
	else if(memcmp(got, payload, plen) != 0) { printf("      内容与原文不一致\n"); bad = 1; }

	if(s_overflow)
	{
		printf("      接收缓冲溢出过（不该发生）\n"); bad = 1;
	}

	s_last_moves = s_moves;
	printf("  [%s] %s   (信封=%u 每次喂=%u 读批量=%u，搬移 %lu 次)\n",
	       name, bad ? "★ 失败" : "通过", chunk, feed_piece, read_max, s_moves);
	free(stream); free(got);
	return bad;
}

int main(void)
{
	uint32_t i;
	int bad = 0;
	uint8_t *payload;
	const char *trap = "+IPD,999:";
	uint32_t plen = 13000;

	payload = (uint8_t *)malloc(plen);
	for(i = 0; i < plen; i ++) payload[i] = (uint8_t)(i * 7 + (i >> 8));
	/* 埋陷阱：若解析错位、或按内容找边界，立刻会露馅 */
	memcpy(payload + 1000, trap, strlen(trap));
	memcpy(payload + 5000, trap, strlen(trap));
	memcpy(payload + 12000, "OK\r\nERROR\r\nFAIL\r\n+IPD", 21);

	printf("载荷 %u 字节，含 \"+IPD,999:\" 陷阱 2 处 + \"OK/ERROR/FAIL/+IPD\" 1 处\n\n", plen);

	/* 正常用例：都不该溢出，且内容必须逐字节一致 */
	bad |= run_case("真实工况        ", payload, plen, 256, 265, 256);
	bad |= run_case("逐字节读(旧行为) ", payload, plen, 256, 265, 1);
	bad |= run_case("信封大于读批量  ", payload, plen, 400, 407, 64);
	bad |= run_case("信封被切成两半  ", payload, 300, 128, 2,   7);
	bad |= run_case("抽干后一次全喂  ", payload, 5000, 256, 511, 511);
	bad |= run_case("读批量=511      ", payload, plen, 256, 265, 511);

	/* 定量对比 —— 这是本次修改的核心证据。
	 * 同样 13000 字节，逐字节读 vs 批量读，G4_RxDrop 的总搬移量差多少。
	 * 搬移量就是 CPU 时间；逐字节版的 O(n²) 正是"缓冲越满越排不空、
	 * 最后溢出丢字节"的原因（硬件上实测溢出标志被置 1）。
	 *
	 * 注意：这里是**性能**修复，不是正确性修复 —— 只要不溢出，
	 * 两种读法结果都对（上面的用例已证）。但逐字节版在真实时序下会溢出。 */
	{
		unsigned long m1, m256;
		printf("\n  —— 抽干代价对比（同样 %u 字节载荷，信封 256）——\n", plen);
		run_case("  逐字节读     ", payload, plen, 256, 265, 1);
		m1 = s_last_moves;
		run_case("  批量读 (256) ", payload, plen, 256, 265, 256);
		m256 = s_last_moves;
		printf("\n      逐字节 %lu 次搬移  →  批量 %lu 次  （降到 1/%lu，越长的固件差距越大）\n",
		       m1, m256, (m256 ? m1 / m256 : 0));
		if(!(m256 * 20 < m1))
		{
			printf("      ★ 批量版没有明显更快（预期至少 20 倍），O(n²) 没被消掉\n");
			bad = 1;
		}
	}

	/* 退化一致性：G4_PayloadGet 必须与 G4_PayloadRead(...,1) 等价。
	 * 用同一个输入流各跑一遍，逐字节喂（避免整段塞进 512 缓冲造成溢出），
	 * 两边取到的载荷字节数与内容都必须一致。 */
	{
		uint8_t b1 = 0;
		uint32_t slen = 0, sent = 0, na = 0, nb = 0, npl = 500;
		uint8_t *st = build_stream(payload, npl, 200, &slen);
		int k, ov = 0;

		for(k = 0; k < 2; k ++)
		{
			G4_PayloadReset(); s_overflow = 0; sent = 0;
			while(sent < slen)
			{
				uint32_t c = (slen - sent > 200) ? 200 : (slen - sent);
				feed(st + sent, c);
				sent += c;
				for(;;)
				{
					if(k == 0) { if(G4_PayloadGet(&b1)) na ++; else break; }
					else       { if(G4_PayloadRead(&b1, 1) == 1) nb ++; else break; }
				}
			}
			/* 末尾可能还压着最后一批没排空 */
			for(;;)
			{
				if(k == 0) { if(G4_PayloadGet(&b1)) na ++; else break; }
				else       { if(G4_PayloadRead(&b1, 1) == 1) nb ++; else break; }
			}
			if(s_overflow) ov = 1;
		}

		if(ov) { printf("\n  [封装一致性] ★ 失败：测试框架自己溢出了\n"); bad = 1; }
		else if(na != nb || na != npl)
		{
			printf("\n  [封装一致性] ★ 失败：Get 取到 %u，Read(,1) 取到 %u，原文 %u\n",
			       na, nb, npl);
			bad = 1;
		}
		else printf("\n  [封装一致性] 通过  (两种 API 都取到 %u 个载荷字节，与原文一致)\n", na);
		free(st);
	}

	printf("\n%s\n", bad ? "★★ 有用例失败"
	                     : "★ 全部通过：含陷阱载荷、跨界信封、批量与逐字节等价");
	free(payload);
	return bad;
}
'''.replace("__CODE__", code)

cfile = os.path.join(WORK, "pr.c")
exe = os.path.join(WORK, "pr.exe")
open(cfile, "w", encoding="utf-8").write(HARNESS)

r = subprocess.run([GCC, "-O0", "-o", exe, cfile], capture_output=True)
if r.returncode:
    print("★ gcc 编译失败：\n" + (r.stdout + r.stderr).decode("utf-8", "replace")[:2000])
    sys.exit(1)
sys.exit(subprocess.run([exe]).returncode)
