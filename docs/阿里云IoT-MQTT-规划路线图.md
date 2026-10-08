# 下一阶段规划 —— 接入阿里云物联网平台（MQTT）

> 2026-09-27 记录。**本阶段尚未开工**，本文只做路线图、验收标准与风险预案。
> 前置阶段见 [`内网OTA-进度与交接.md`](内网OTA-进度与交接.md)。

## 一句话结论

**硬件不用换，现有 ESP8266 D1 Mini 就能做。** 阿里云物联网平台支持 **1883 端口 TCP 非加密接入**
（ClientId 里写明 `securemode=3`），这正是 ESP8266 AT 固件透传模式能做的事 ——
不需要 TLS、不需要换 4G 模块。但有三处必须提前处理，见下节。

---

## 一、关键技术判断（开工前先认下这三条）

### 1. 1883 非加密可用，但有前提

| 项 | 说明 |
|---|---|
| 接入方式 | TCP 直连，`securemode=3`（TLS 是 `securemode=2`，走 8883/443） |
| 适用实例 | **公共实例可用**。⚠️ **新建的企业版实例默认关闭 TCP 非加密接入**，只能走 TLS |
| 风险 | 官方明确"数据不加密，安全级别低"，仅建议测试验证期使用 |
| 影响 | 若控制台里拿到的是企业版实例，本阶段方案作废，要改用带 TLS 的路径 |

**所以第 3 步之前的第一件事，是进控制台确认实例类型。** 这是整条路线的单点前提，五分钟能确认。

### 2. 手写报文 vs ESP-AT 自带 MQTT 指令 —— 建议手写

ESP8266 的 ESP-AT 固件从 **v2.1.0.0** 起自带 MQTT 指令集
（`AT+MQTTUSERCFG` / `AT+MQTTCONN` / `AT+MQTTSUB` / `AT+MQTTPUB`），理论上能让模块自己跑 MQTT，
STM32 只发 AT 指令。**但对本项目不推荐**，理由三条：

| | 手写报文（原规划） | 用 ESP-AT 的 MQTT 指令 |
|---|---|---|
| AT 固件版本要求 | 只要有 TCP 透传就行，**最低** | 必须 ≥ v2.1.0.0，要先刷固件 |
| 凭据长度限制 | 无 | 单条 AT 指令 < 256 字节，username/password 各 ≤ 64 字节，超了要拆成 `AT+MQTTLONGxxx` 系列 |
| 面试可讲深度 | **MQTT 报文逐字节、鉴权签名、状态机切包** | "调了三条 AT 指令" |
| 调试可控性 | 每一字节都能在串口日志里看 | 黑盒，出错只能看 ERROR |

手写报文还有一个隐含好处：**它对 AT 固件版本的要求最低**，不用担心模块固件老旧的兼容问题。
**代价**是要自己实现 SHA1/SHA256 + HMAC + base64 和 MQTT 编解码，这正是本阶段的工作量所在。

### 3. MQTT 报文不能沿用现有的 +IPD 解析

现有 `4G.c` 的 +IPD 状态机是**按信封（`+IPD,<len>:`）切包**的。MQTT 走的是裸 TCP 流，
报文之间**没有任何分隔符**，唯一的边界是固长头里的 **Remaining Length** 字段
（变长编码，最长 4 字节，每字节低 7 位有效、最高位是续接标志）。
TCP 还会粘包/拆包，所以**必须新写一个按剩余长度切包的状态机**，不能复用现有那套。
这是原规划里没写、但不做就必然踩的一步。

---

## 二、规划原文（8 步）+ 每步验收标准

> 原文出自 `连接阿里云物联网平台任务规划.txt`（第 5 步的 CONACK 应为 **CONNACK**）。

| # | 原规划 | 验收标准（做没做到怎么判） | 已知坑 |
|---|---|---|---|
| 1 | 了解 MQTT 各报文的构成 | 能默画 CONNECT 的固长头 + 可变头字段布局 | 见附录速查表 |
| 2 | 注册阿里云账号，建产品和设备 | 拿到三元组 `ProductKey` / `DeviceName` / `DeviceSecret` | 三元组是敏感信息，**不进 git** |
| 3 | 设定模块连阿里云 TCP 服务器 | `AT+CIPSTART` 后返回 `CONNECT OK`，PC 侧可见建连 | 域名形如 `${pk}.iot-as-mqtt.cn-shanghai.aliyuncs.com`，端口 1883 |
| 4 | 构造 CONNECT 报文 | 串口日志打出完整十六进制，**长度字段自洽** | 固长头第 2 字节起是变长 Remaining Length，别按单字节算 |
| 5 | 发 CONNECT 并判 CONNACK | 收到 `20 02 00 00`（返回码 0 = 接受） | 返回码非 0 时对照 `01/02/03/04/05` 定位（协议版本/ClientId/服务不可用/用户名密码错/未授权） |
| 6 | 构造 SUBSCRIBE，订阅 Topic | 收到 `90 xx <packetId> 00`（SUBACK 返回码 0x00 成功） | SUBSCRIBE 固长头的 flags **必须是 0x02**（即 `0x82`），否则服务器直接断连 |
| 7 | 收 PUBLISH，提取命令控制设备 | 控制台"在线调试"下发属性，设备串口打出收到的 JSON 并动作 | 下行 topic 一般是 `/sys/${pk}/${dn}/thing/service/property/set` |
| 8 | 构造 PUBLISH 上报数据 | 控制台"设备详情 → 物模型数据"里能看到数值刷新 | **见下节第 3 条，必须按物模型格式发** |

---

## 三、原规划缺的四件事（必须补，否则做完了不工作）

### 1. 没有 PINGREQ 保活

MQTT 靠心跳维持连接（CONNECT 报文里的 Keep Alive 字段）。**不发 PINGREQ，服务器到点就踢连接**，
表现为"上报了几分钟之后突然不动了"。阿里云 Keep Alive 上限 1200 秒，建议设 **60 秒**，
收到 `D0 00`（PINGRESP）即算正常。

### 2. 没有断线重连

WiFi 抖动、路由器重连、服务器主动断，都会让 TCP 断开而模块不自知。
需要一条重连路径：**检测到 N 次 PINGREQ 无 PINGRESP（或 TCP 断开）→ 重新 `AT+CIPSTART` → 重发 CONNECT → 重订阅**。
`4G.c` 现有的 AT 指令收发框架可以复用来做这件事。

### 3. 第 8 步"网页/APP 上可见"需要走物模型，不是随便发字符串

这是**最容易出现"代码跑通了但页面上什么都没有"**的地方。
阿里云 IoT 的"设备详情 → 物模型数据"、Web 可视化、云产品流转，都只认 **Alink JSON** 格式，
上报 topic 固定为 `/sys/${pk}/${dn}/thing/event/property/post`，载荷形如：

```json
{"id":"1","version":"1.0","params":{"temperature":25.3},"method":"thing.event.property.post"}
```

**前提**：这个 `temperature` 属性必须在控制台的**物模型**里先定义好（标识符、数据类型、取值范围要一致），
否则服务器收下但不入库，页面上看不到。
→ 若只想要"发到云端就行"，可以走自定义 Topic + 云产品流转，但第 8 步原文明确说"显示在网页或 APP 上"，
**所以按物模型走**。

### 4. TCP 粘包 / 按剩余长度切包

见上节第 3 条。补充一点：**一次 `+IPD` 里可能装着半个 MQTT 报文，也可能装着两个半**，
切包状态机要在任意字节边界上都能正确续接。验收方法是 PC 侧脚本喂各种切法的合成流（可扩展
`scripts/test_payload_read.py` 的做法）。

---

## 四、分阶段实施（每阶段可独立验证）

> 原则：**能 PC 侧验证的绝不先上板**。硬件轮次有限，每轮都要带足诊断信息。

### M1 —— PC 侧对标（不碰板子，0 硬件轮次）

1. 控制台建产品/设备，拿到三元组；
2. 用控制台的**"MQTT 连接参数生成"**或自己写 Python 脚本算出 clientId/username/password；
3. 用 MQTT.fx / `paho-mqtt` 在 PC 上先连一遍，确认三元组和 topic 都对；
4. **把这一组参数和 CONNECT 报文的期望十六进制固化下来，作为后续固件的对照基准。**

这一步的产物就是 M2 的"标准答案"。没有它，M2 出问题就分不清是签名错还是网络错。

### M2 —— STM32 侧报文构造与鉴权（可纯串口验证）

1. 实现 SHA1/SHA256 + HMAC + base64（先单独测：喂固定输入，比对标准向量）；
2. 实现 MQTT 编解码 + 剩余长度切包状态机；
3. 组装 CONNECT，**先在串口日志里把十六进制打出来，与 M1 的基准逐字节比对**；
4. 比对通过再发出去。

### M3 —— 端到端

按原规划第 3→5→6→7→8 步走，最后补 PINGREQ 与重连。

---

## 五、容量预算（必须先量，可能要动分区）

MQTT 这段代码进 **A 区 App**：

| 项 | 估算 | 说明 |
|---|---|---|
| SHA256 + HMAC + base64 | ~2–3 KB | 选 `signmethod=hmacsha1` 可省一点（password 40 字符 vs 64 字符） |
| MQTT 编解码 + 切包状态机 | ~1–2 KB | |
| Alink JSON 组装 | ~1–2 KB | 若引 cJSON 会更大，也可以按固定格式手拼字符串 |
| **合计** | **~4–7 KB** | 需实测 |

**当前 A 区余量 15672 字节**（App 用了 13000 / 28672）。按上表估算够用，但**JSON 是变量**，
M2 做完立刻量一次实际占用。若逼近 28 KB 上限，B 区还有 7868 余量可再挤给 A 区 ——
**但改分区要同步四处配置，参考 [`内网OTA-进度与交接.md`](内网OTA-进度与交接.md) 的分区章节，别只改一处。**

---

## 六、开工前确认清单

- [ ] 阿里云实例是**公共实例**（不是企业版，企业版默认关 1883）
- [ ] 三元组拿到，存进 `wifi_cfg.h` 同级的**已 gitignore** 的配置文件，不进仓库
- [ ] 物模型里把要上报的属性（标识符/类型/范围）先定义好
- [ ] ESP8266 的 AT 固件版本确认（`AT+GMR`），确认支持 `AT+CIPMODE=1` 透传
- [ ] 接线沿用 `7-(A区)ESP8266远程控制`：USART2 PA2/PA3 @115200
- [ ] 确认域名解析走的通（`AT+CIPDOMAIN` 或直接 `AT+CIPSTART` 填域名）

---

## 附录：MQTT 3.1.1 报文速查（本阶段只用到这 6 种）

| 报文 | 首字节 | 结构要点 |
|---|---|---|
| CONNECT | `0x10` | 可变头：`00 04 'M' 'Q' 'T' 'T'` + 协议级别 `04` + 连接标志 + Keep Alive(2B)；载荷：ClientId / Username / Password，**每项都是 2 字节长度前缀 + 内容** |
| CONNACK | `0x20` | `20 02 <会话标志> <返回码>`，返回码 `00` = 接受 |
| SUBSCRIBE | `0x82` | **flags 固定 0x02**；`<packetId:2B>` + `[topic 2字节长度 + topic + QoS]`，可带多个 topic |
| SUBACK | `0x90` | `<packetId:2B>` + 每个 topic 一个返回码（`00`=QoS0 授予成功） |
| PUBLISH | `0x30` | 低 4 位是 flags（QoS）；可变头：`[topic 2字节长度 + topic]` + （QoS>0 时）`<packetId:2B>`；之后全是载荷 |
| PINGREQ / PINGRESP | `0xC0` / `0xD0` | `C0 00` / `D0 00`，无载荷 |

**Remaining Length 编码**（所有报文通用，最易写错的地方）：
每字节低 7 位有效，最高位 1 表示"还有下一字节"，从低位到高位拼接。例如长度 13000 需要 2 字节。

---

## 参考资料

- [阿里云 IoT：MQTT-TLS 连接通信](https://www.alibabacloud.com/help/zh/iot/user-guide/establish-mqtt-connections-over-tcp)
- [阿里云 IoT：使用 MQTT.fx 接入物联网平台](https://www.alibabacloud.com/help/zh/iot/getting-started/using-mqtt-fx-to-access-iot-platform)
- [ESP-AT MQTT AT Commands（ESP8266 v2.3.0.0）](https://docs.espressif.com/projects/esp-at/en/release-v2.3.0.0_esp8266/AT_Command_Set/MQTT_AT_Commands.html)
