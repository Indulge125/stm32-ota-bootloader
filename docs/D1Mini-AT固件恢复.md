# D1 Mini AT 固件恢复（被 Arduino 项目覆盖之后）

> 返回 → [README](../README.md)

**这块 ESP8266 D1 Mini 是共享硬件。** 它在这个项目里的角色是 **WiFi 模组**（跑 ESP-AT 固件，
由 STM32 通过 AT 指令驱动）；但在别的课程作业里它被当成**单片机**用（跑 Arduino sketch）。
两者烧的是**同一片 Flash**，不可能共存 —— 谁后烧谁覆盖。

> 2026-10-15 记录。起因：用 Arduino IDE 烧 `LedTcpClient.ino`（WpfApp12 的配套固件）做 C# 作业，
> 把 AT 固件整片覆盖，OTA 链路失效。

---

## 一、先分清：什么被覆盖了，什么没被覆盖

| 器件 | 存什么 | 被 Arduino 烧录影响吗 |
|---|---|---|
| STM32F103C8T6 | B 区 BootLoader + A 区 App | ❌ 完全不受影响（另一块板，另一片 Flash）|
| W25Q64（SPI Flash）| 从平台下载的升级包 | ❌ |
| AT24C02（EEPROM）| `OTA_Flag` / `OTA_Ver` | ❌ |
| **ESP8266 D1 Mini** | **ESP-AT v2.2.2.0 固件** | ✅ **整片 Flash 被覆盖** |

所以结论要分两半说，别混：

- **设备上"已经升级好的固件"（A 区）** → 没丢，它在 STM32 里。
- **设备"自己升级自己"的能力** → **没了**，因为链路上的 WiFi 模组不再是 AT 模组。

**恢复只需要重刷 ESP8266，STM32 侧一行代码、一个配置都不用改。**

### 断链示意图

```
STM32(USART2 PA2/PA3) ──AT指令──▶ ESP8266 ──▶ OneNET
                                     ↑
                          这里现在跑的是 LedTcpClient
                          （不认识 AT 指令，只会连 WPF 的 9000 端口）
```

---

## 二、症状

STM32 上电后，USART1（9600）会打出：

```
[HTTP] 切 Station 模式失败（AT+CWMODE=1）—— 模组没应答？查 USB 供电、PA2/PA3 接线、PB2 跳线
[HTTP] 连 WiFi 失败 —— 查 wifi_cfg.h 里的 SSID/密码，以及模组是否在范围内
```

⚠️ **这句话会把排查带偏**：它列了"供电 / 接线 / 跳线"三项，**唯独没有"模组里没装 AT 固件"**，
而现实中这是最容易发生的一种（只要有人拿它跑过一次 Arduino 项目）。

> 见到这两句，先按本文档回刷 AT 固件，再去查接线。

---

## 三、恢复步骤

### 0. 材料（全部在本机，不需要下载）

| 用途 | 路径 |
|---|---|
| **完整镜像（推荐）** | `D:\develop\ESP-WROOM-02-AT-V2.2.2.0\ESP-WROOM-02-AT-V2.2.2.0\factory\factory_WROOM-02.bin`（正好 2 MB）|
| 分段镜像（备选）| 同上目录下的 `bootloader\` `partition_table\` `esp-at.bin` `at_customize.bin` `customized_partitions\` |
| 烧录工具 | `D:\develop\flash_download_tool\flash_download_tool_3.9.11.exe` |

### 1. 先把 STM32 那侧断开

拔掉 STM32 `PA2/PA3` 到 D1 Mini 的杜邦线。

**为什么**：刷写期间 D1 Mini 串口会输出大量数据，STM32 的 USART2 中断会一直吃进来。
不影响刷写本身，但 STM32 串口日志会刷屏，出问题时你分不清是模组在说话还是刷写过程在抖。

### 2. 插上 D1 Mini，确认 COM 口

设备管理器 → 端口，应出现 **`USB Serial Port (COMxx)`**。
本项目历史上一直是 **COM18**（FTDI FT232，`VID_0403&PID_6001`）。

### 3. Flash Download Tool 配置

**方式 A —— 只填一行（推荐，一次覆盖整片）：**

| 起始地址 | 文件 |
|---|---|
| `0x0` | `factory_WROOM-02.bin` |

这是 2026-09-26 实机跑通过的那次（日志原文 `Wrote 2097152 bytes at 0x00000000 ... PASS / Hash of data verified`）。

**方式 B —— 按官方 `download.config` 分段：**

| 地址 | 文件 |
|---|---|
| `0x0` | `bootloader\bootloader.bin` |
| `0x8000` | `partition_table\partition-table.bin` |
| `0x9000` | `ota_data_initial.bin` |
| `0x10000` | `esp-at.bin` ← AT 应用本体，876656 字节 |
| `0xF0000` | `at_customize.bin` |
| `0xF1000` | `customized_partitions\factory_param.bin` ← **默认版，别用 `factory_param_gpio1_3.bin`**，理由见「踩坑」第 1 条 |

**面板参数：**

| 项 | 值 | 依据 |
|---|---|---|
| CrystalFreq | **`26MHz`** | ESP8266 用 26M 晶振 —— **选错会连不上**（连接波特率算错）|
| SPI MODE | `DIO` | 固件包 `flasher_args.json` |
| SPI SPEED | `80MHz` | 同上 |
| Flash Size | `2MB` | 同上 |
| 连接波特率 | `115200` | 历史日志 |
| `DoNotChgBin` | **不要勾** | 历史日志写入的是 `bootloader.bin_rep` / `factory_WROOM-02.bin_rep` |

> **`DoNotChgBin` 为什么不能勾**：ESP8266 的 Flash 参数（模式 / 频率 / 容量）写在 **bootloader 的镜像头**里，
> 工具要按上面的 SPI MODE / SPEED / Flash Size 去改写 bin 的开头一段，改写后的副本就是带 `_rep` 的那个
>（历史日志里正是 `bootloader.bin_rep`、`factory_WROOM-02.bin_rep`）。
> 勾上 `DoNotChgBin` 工具就不改了，写进去的 bootloader 与实际参数可能对不上。

> ⚠️ 芯片实测是 **4MB**（日志 `device_id 0x4016`），但固件包是按 **2MB** 配的。
> **照 2MB 配**，不要"顺手改成 4MB" —— 2MB 这套是验证通过的，多余的 2MB 用不到，不影响功能。

### 4. 点 START，等 `PASS`

成功标志（照抄历史日志）：

```
Wrote 2097152 bytes at 0x00000000 ...
Download result: PASS
Hash of data verified.
```

### 5. 验证：接回 STM32 看日志（⚠️ 别用 USB 发 AT）

**把 D1 Mini 接回 STM32，STM32 的调试口（USART1，9600，UTF-8）看日志。**

```
[TX] AT+CWMODE=1
[RX] AT+CWMODE=1\r\n\r\nOK\r\n        ← 出现这一行 = AT 链路通了
[TX] AT+CWJAP="...","..."
[RX] ...\r\nWIFI CONNECTED\r\nWIFI GOT IP\r\n\r\nOK\r\n
[TX] AT+CIPSTART="TCP","iot-api.heclouds.com",80
[RX] ...\r\nCONNECT\r\n
[HTTP] 200 OK -> 200，Ota-Errno=-1，Content-Length=71
[OTA] 无待升级任务（平台返回 code=12012）
===== OTA 结束 rc=0 =====
```

**`rc=0` + `[TX] AT+CWMODE=1 → OK` 同时出现 = 恢复完成。**

#### ⚠️ 为什么**不能**用「USB 发 AT，看回不回 OK」来验证

**因为这块板子的 AT 指令口不在 GPIO1/GPIO3，而 USB 串口芯片正好接的就是 GPIO1/GPIO3。**

| 谁在用 UART | 走哪两个脚 | 由谁决定 |
|---|---|---|
| ROM 启动日志 + 串口下载模式 | **GPIO1/GPIO3**（= D1 Mini 丝印 `TX`/`RX`）| **ROM 硬编码，改不了** |
| **AT 指令口** | **STM32 实际接线的那两个脚** | **`factory_param` 决定** |

所以：

- USB 能烧录、能看 ROM 日志 —— 这是烧录一直成功的原因；
- 但 **AT 固件的对话口在别处**，USB 上永远等不到 `OK`。

**在 USB 口发 AT 收不到回复，是正常现象，不是故障。** 2026-10-15 实测确认（见文末复验记录）。

> 只有把 `factory_param` 刷成 `factory_param_gpio1_3.bin` 时 USB 口才能对话 AT ——
> 但那会**切断 STM32 那条链**。别为了测 AT 而留下这个状态。

#### USB 口仍然有用：看 ROM 日志判断模组死活

ROM 启动日志**固定 74880 波特率**，用 115200 读出来全是乱码（本次实测踩过这个坑）。

串口工具设 **74880 + ASCII**，复位板子：

```
ets Jan  8 2013,rst cause:2, boot mode:(3,6)

load 0x40100000, len 7464, room 16
tail 8
chksum 0x34
...
csum 0xf9
```

| 字段 | 健康值 | 含义 |
|---|---|---|
| `rst cause:2` | 2 | 正常复位（4 = 看门狗反复重启，异常）|
| `boot mode:(3,6)` | 3 | 从 Flash 启动（1 = 停在下载模式，异常）|
| `csum 0x??` | 出现 | ROM 已校验镜像并跳转执行 → **镜像没烧坏** |

看到 `csum` 只能说明「固件在 Flash 上、ROM 读得出来、校验通过」，**不能证明 AT 通** —— AT 通不通只能靠上面那个 STM32 日志判。

> ⚠️ **别和 STM32 的终端设置搞混**：STM32 自己的 USART1 命令行是 **9600 且不要勾发送新行**
> （命令按接收长度严格匹配）。ESP8266 那边是 115200 / ROM 日志 74880，是另一个口、另一套规矩。

### 6. 接回 STM32 时的两个注意

- **不需要手动给 ESP8266 配 WiFi** —— 理由见下一节。
- **D1 Mini 的供电**：3.3V，由 STM32 板那侧给。烧录时是 USB 供电，接回后确认它仍有电。

---

## 四、为什么 WiFi 凭据不用重新配

`1.1-(A区)串口测试程序/Hardware/onenet_http.c` 的 `EnsureWiFi()` 每次上电都无条件执行一遍：

```
G4_SetStation()                        →  AT+CWMODE=1     （先切 Station）
G4_JoinAP(WIFI_SSID, WIFI_PASS)        →  AT+CWJAP="ssid","pass"
```

凭据在 `6-串口IAP功能/Hardware/wifi_cfg.h`，经 `onenet_cfg.h` 被 include 进来。

所以 AT 固件里存的旧 AP 配置丢了也无所谓，**STM32 每次上电都会重新下发给它**。

> 🔒 `wifi_cfg.h` 里是 WiFi 明文凭据，而本仓库是 **public**。
> **不要把 SSID/密码抄进任何文档、截图或提交里。**

---

## 五、本次恢复用的已知可用参数（2026-09-26 实机验证，非推测）

来源：`D:\develop\flash_download_tool\logs\94B97E1A5C24.txt`

| 项 | 值 |
|---|---|
| 芯片 | ESP8266，`device_id 0x4016`（4 MB），`manufacturer_id 0x20` |
| MAC | `94:97:1e:1a:5c:24` ← 工具用它命名日志与 dump |
| 工具 | Flash Download Tool **3.9.11** |
| 连接波特率 | 115200 |
| 固件 | **ESP-AT v2.2.2.0**（ESP-WROOM-02 版）|
| `factory_param` | **默认版**：偏移 `0x10` 处 `0F 0D 03 01`，偏移 `0x0C` 处 `0x0001C200` = **115200** ✅ 与项目文档「USART2 PA2/PA3，115200」一致 |

---

## 六、踩坑记录

### 1. 别用 `factory_param_gpio1_3.bin`

本机有两个 `factory_param`，**只差 4 个字节**（偏移 `0x10–0x13`）：

| 文件 | `0x10` 起的 4 字节 |
|---|---|
| `customized_partitions\factory_param.bin`（默认）| `0F 0D 03 01` |
| `factory_param_gpio1_3.bin`（变体）| `01 03 FF FF` |

按文件名推测是 UART 引脚配置，但**语义以 ESP-AT 官方文档为准，别自行推断**。

关键是看**历史记录**：日志显示 `19:14` 刷过一次变体，**`20:01` 又刷回了默认版**，
而项目之后一路做到 10 月初。

→ **默认版才是验证过的状态。** 恢复时复现它，不要拿变体去试。

### 2. 波特率撞车会让现象更隐蔽

如果 D1 Mini 上跑的恰好是 `LedTcpClient`（WpfApp12 那个配套固件），它有两个巧合：

- `Serial.begin(115200)` 与 STM32 的 USART2 **波特率相同**；
- 开机就往外打 `[BOOT]/[WIFI]/[TCP]/[CMD]/[LED]/[ERR]` 日志。

于是 STM32 收到的**不是"无应答"**，而是**一堆对不上的字节** ——
`G4_WaitResp("OK", ...)` 会一直等到超时。

"有数据但全不对" 比 "完全没反应" 难查一个数量级。**知道这个巧合，就能少走几小时弯路。**

### 3. 失败提示本身缺项

`onenet_http.c` 那句 `模组没应答？查 USB 供电、PA2/PA3 接线、PB2 跳线` 已经成了排查入口，
但它**漏了最常见的第 4 种原因**：模组里根本没装 AT 固件。

> 建议（本次未改代码，留作待办）：把这句补上第 4 项，或者直接指向本文档。

### 4. 本机那份 dump 救不了场

`flash_download_tool\flash_dump\94b97e1a5c24_0x000_0x10000_20260926_172551.bin` 只有 **64 KB**，覆盖 `0x0–0x10000`，
**不含 `0x10000` 处的 AT 应用本体**，回滚不了。

真正能救场的是官方 `factory_WROOM-02.bin`（完整 2 MB 镜像）—— 所以那个文件**别删**。

---

## 七、纪律：这块板子的默认状态

> **D1 Mini 的默认状态 = 跑 ESP-AT 固件。**

拿它做任何 Arduino / PlatformIO / MicroPython 实验**之前**，先意识到
「这个项目的 OTA 会因此不可用」；实验做完，按本文档恢复。

更好的做法：**实验前先备份，实验后立刻恢复**，不要等到要用 OTA 时才发现模组不对。

---

## 八、复验记录

### 2026-10-15 —— 重刷后全链路复验通过（`rc=0`）

**起因**：D1 Mini 被 Arduino 烧了 `LedTcpClient.ino`（WpfApp12 配套固件），AT 固件整片被覆盖。

**操作**：按「方式 A」把 `factory_WROOM-02.bin`（2 MB）写到 `0x0`。
Flash Download Tool 的 `DetectedInfo` 回报：
`flash vendor: 20h : XMC` / `devID: 4016h` / `QUAD;4MB` / **`crystal: 26 Mhz`**。

**结果**：全链路通过。

| 阶段 | 实测证据 |
|---|---|
| B 区 BootLoader | `自检: W25Q64 MID=0xEF DID=0x4017` / `自检: AT24C02=OK, OTA_Flag=0x00000000` / `OTA无更新，跳转A区` |
| A 区 App | `APP v1.0.0` / **`Token SelfTest: PASS`** / `48 0 30` |
| **AT 链路** | `[TX] AT+CWMODE=1` → `[RX] AT+CWMODE=1\r\n\r\nOK\r\n` |
| WiFi | `WIFI DISCONNECT` → `WIFI CONNECTED` → `WIFI GOT IP` → `OK` |
| TCP + DNS | `AT+CIPSTART="TCP","iot-api.heclouds.com",80` → `CONNECT` |
| HTTP | `[HTTP] 200 OK -> 200，Ota-Errno=-1，Content-Length=71` |
| 业务 | `[OTA] 版本已上报：s_version=1.0.0` / `[OTA] 无待升级任务（平台返回 code=12012）` / `===== OTA 结束 rc=0 =====` |

#### 本次新增的两条认识（已并入正文）

1. **默认 `factory_param` 的 AT 口不在 GPIO1/GPIO3**，所以「在 USB 口发 AT 看不到回复」是**必然现象，不是故障**。
   本次在这上面绕了不短的时间，而且一度把 ROM 启动日志的乱码误判成"固件没跑起来"。
   这也从实测上印证了 9/26 那次「19:14 刷 `factory_param_gpio1_3.bin` → 20:01 刷回默认」的选择是对的。

2. **ROM 启动日志固定 74880 波特率**，用 115200 读是乱码。
   74880 下能读到干净的 `csum 0x??`，用来判断「模组是否健康启动」。
   注意它的能力边界：`csum` 通过 ≠ AT 通，只能排除"镜像烧坏"。

#### 关于 `Ota-Errno=-1`

两次响应都是 `-1`，**这不是错误**。项目代码自己写明了语义 ——
`onenet_http.h` 里注释：`/* Ota-Errno 头；**-1 表示响应里没有这个头** */`
（`onenet_http.c` 把 `resp->ota_errno` 预置为 `-1`，只有解析到该头才覆盖）。

这两次分别是「版本上报」和「检测任务」，响应里本就不带该头。
**真实下载那一步才应该是 `Ota-Errno=0`**（配 `HTTP 206`）—— 这条尚未单独验证。

> 🔒 本次复验的串口日志原文含 `AT+CWJAP` 的明文凭据，**不抄进本文件**。
