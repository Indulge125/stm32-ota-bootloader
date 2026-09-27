# 内网 OTA —— 进度与交接

> 2026-09-27 记录。用于新会话接手，也可作为项目进度快照。

## 一句话现状

**串口 OTA 已完整验证；内网 OTA 做到了「MCU 能通过 WiFi + 内网穿透 完整收下 13000 字节固件」，
下一步是把它写进 W25Q64 并触发搬运。**

---

## 工程坐标

| 项 | 值 |
|---|---|
| 工作目录 | `d:\develop\stm32-ota固件升级` |
| 仓库 | https://github.com/Indulge125/stm32-ota-bootloader （public） |
| 最新提交 | `feat(分区): B 区 32→36KB / A 区 32→28KB`（**未推送**，本机连不上 GitHub，见文末；具体 hash 见 `git log -1`） |
| 硬件 | STM32F103C8T6 + OLED(PB8/9) + AT24C02(PB10/11) + W25Q64(PA4-7) + ESP8266 D1 Mini(USART2: PA2/PA3) |
| 调试串口 | USART1 PA9/PA10，**9600**（Tera Term，UTF-8） |
| ESP8266 | USART2 PA2/PA3，**115200** |
| 分区 | B 区(BootLoader) 36KB @0x08000000；A 区(App) 28KB @0x08009000 |
| BootLoader 镜像 | **29448 / 36864，余量 7416** |

---

## 已完成并验证的

### 串口 OTA（全部实测通过）
- 命令 `1`~`7`：擦除/串口IAP下载/版本号/查版本/外部FLASH下载/外部FLASH搬运/复位
- 两条升级路径端到端跑通：命令 `2` 直写 A 区、命令 `5`→`6` 经 W25Q64 搬运
- 上位机：`scripts/flash.bat`（双击）/ `scripts/xmodem_send.py`（自带 Xmodem-CRC）

### 内网 OTA（前三步）
- `[0]` ESP8266 AT 自测 ✓
- `[8]` 连 WiFi（`AT+CWMODE=1` → `AT+CWJAP`）✓ —— **注意顺序，ESP-AT 默认 SoftAP，不先设 Station 会秒回 ERROR**
- `[9]` 连服务器（`AT+CIPMUX=0` → `AT+CIPSTART`）✓ —— 服务器端 Hercules 看到连接 ✓
- `[t]` **4b-2a 流式 +IPD 接收测试** ✓✓ **13000 字节一字节不差**（头尾均与固件对上）

链路：`服务器(内网:8080) → 花生壳穿透(外网:25340) → ESP8266(WiFi) → +IPD 流式解析 → MCU`

服务器端脚本：`scripts/ota_server.py`（监听 8080，等设备发 PING 后再发固件）

---

## 下一步（按顺序）

### 第一步：扩分区 ✅ 代码已完成（2026-09-27），待硬件复测

**完成情况**：B 区 32KB → **36KB**，A 区 32KB → **28KB**，四处同步改完：

| # | 位置 | 结果 |
|---|---|---|
| 1 | `6-串口IAP功能/User/main.h` 的 `MyFlash_B_Page_Num` | `32` → `36` ✅ |
| 2 | `6-串口IAP功能/Project.uvprojx` 的 `<OCR_RVCT4>` 大小 | `0x8000` → `0x9000` ✅ |
| 3 | A 区工程 `.uvprojx` 的 `<OCR_RVCT4>` | 起址 → `0x8009000`、大小 → `0x7000` ✅ |
| 4 | A 区工程 `Start/system_stm32f10x.c` 的 `VECT_TAB_OFFSET` | `0x8000` → `0x9000` ✅ |

顺手修掉 `OTA学习笔记.md` 分区图里 B 区那行的**旧值残留**（还写着 20KB / 第 0~19 页，
上次只改了 A 区那行）。

**实测容量**（`python _tools/verify_repartition.py`）：

| 镜像 | 大小 | 分区容量 | 余量 |
|---|---|---|---|
| BootLoader | 29448 | 36864（36KB） | **7416**（原 3320） |
| A 区 App | 14100 | 28672（28KB） | 14572 |

A 区复位向量 `0x08009235` 落在新区内，链接起址与向量表偏移配套正确。

**⚠ 还没做硬件验证 —— 改动过的东西不能沿用旧验证**：
1. 重新编译并烧录 **B 区**（BootLoader）
2. 重新编译 **A 区** 程序（换了链接起址，必须重编），经命令 `2` 或 `5`+`6` 写入
3. 复测命令 `5` / `6` 两条外部 FLASH 路径，确认搬运后能跳转
4. 之后再做 4b-2b

脚本：`_tools/repartition_flash.py`（可重复执行，逐处校验命中，`--dry` 预演）

### 第二步：4b-2b —— 收固件写 W25Q64 并触发搬运

**协议**（服务器先发一个长度头，MCU 才知道该收多少、能校验）：
```
MCU  →  "OTA_REQ\n"
服务器 → "OTA <总长度>\n" + <总长度 字节固件>
```

**MCU 侧逻辑**：
1. 发请求，等头部 `OTA <len>`（用现有的 `G4_PayloadGet` 逐字节解析）
2. 每收满 256 字节 → `W25Q64_PageProgram`（页对齐）
3. 收够 len 字节 → 校验总数
4. 写 EEPROM：`OTA_Info.FileLen[1] = 长度`、`OTA_Info.OTA_Flag = OTA_SET_FLAG`、`AT24C02_WriteOTAInfo()`
5. `NVIC_SystemReset()` → BootLoader 检测到 OTA_Flag → 自动搬运（这条路径已验证）

**注意**：
- `FileLen` 必须是 4 的倍数（`main.c` 里有 `% 4 == 0` 的检查）
- W25Q64 写前要 `W25Q64_Erase64K(1)`
- 服务器发完不必加延时，但要保证 MCU 收完再断连

**顺手要做的**：`G4_RxDrop` 逐字节调用是 O(n²)，4b-2b 把接收循环改成批量消费
（新增 `G4_PayloadRead(dst, max)` 一次取一批）。**目前它不是瓶颈，别提前改。**

---

## 必须遵守的约束（都是踩过坑换来的）

1. **一次只改一个变量** —— 曾同时改两处导致根因永远无法归因。
2. **改完 BootLoader 代码必须量容量**：`python _tools/verify_repartition.py`
   （链接成功 ≠ 装得下，链接器只按 IROM 的 start/size 检查）
3. **源码编码 = 串口输出编码**：ARMCC 原样透传字节。源码 UTF-8+BOM，
   串口发 UTF-8，**上位机终端必须设 UTF-8**。
4. **`.uvprojx` 里 `<OCR_RVCT4>` 才是生效的 IROM1**，`<IROM>` 那个改它没用。
5. **Python 写补丁脚本的转义规则**（今天踩了五六次）：
   - 要 C 里的 `\r\n` → Python 里写 `\\r\\n`
   - 要 C 里的 `\\x` → Python 里写 `\\\\x`
   - 制表符 `\t` 直接写
   - **工程里的 .c 是 CRLF**，多行常量匹配前必须归一化行尾
6. **丢失数据的形状区分病因**：连续整段丢 = 还没开始听；零散丢 = 听不过来。
7. **别在日志里找不到就断定代码没执行** —— 先确认日志覆盖范围
   （`G4_Cmd` 打日志，`G4_SendCmd` 不打）。

---

## 环境事实

| 项 | 值 |
|---|---|
| Keil | `D:\keil5`，编译器 `/d/keil5/ARM/ARMCC/bin/armcc.exe` |
| 命令行编译验证 | 见 `_tools/`，需手动加 `-DSTM32F10X_MD`（Keil 才自动带） |
| GitHub 推送 | 443 间歇性被重置；先直连，失败再挂代理 `127.0.0.1:7897`。**2026-09-27 两者都不通**，扩分区这次提交与 `b51b5c0` 都还在本地 |
| WiFi 凭据 | `6-串口IAP功能/Hardware/wifi_cfg.h`（**已 gitignore，不进仓库**） |
| 服务器监听端口 | **8080**（穿透指向的内网端口），不是外网 25340 |
| 未推送分支 | `wip/xiaomi-4g-attempt`（含小米 AI 那版改造，**内嵌 WiFi 明文密码，建议删掉**） |

---

## 相关文档

- [`工程问题与踩坑记录.md`](工程问题与踩坑记录.md) —— 8 章，重点是「怎么定位的」
- [`AT24C02-调试记录.md`](AT24C02-调试记录.md) —— 三个叠加硬件故障的完整推理链
- `README.md` —— 功能、接线、分区、验证状态
