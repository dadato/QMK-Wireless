# CH582F 透传桥固件功能规格（v0.4）

> 目标产品：SK32F077 + CH582F 双芯片无线三模键盘
> 本文档描述 **CH582F 侧固件**的功能规格。
> 该固件是**独立工程**（MounRiver Studio / WCH SDK），不属于 QMK 仓库，也不链接进 QMK。
> 引脚与封装决策以 [ch582f_pinmap.md](./ch582f_pinmap.md) 第 0 章 D1~D14 为准；实现细节见 [ch582m_bridge_impl_checklist.md](./ch582m_bridge_impl_checklist.md)。
> SK32 侧 QMK 变更另见第 13 章。

---

## 0. 文档信息

| 项 | 内容 |
|---|---|
| 版本 | v0.4（草案，已同步 D1~D14 决策） |
| 状态 | 待评审 |
| 适用芯片 | **CH582F（QFN28，20 GPIO）** —— 键盘侧与 Dongle 侧同型号；CH582M（QFN48）为备选 |
| 依赖 SDK | WCH CH583/CH582 EVT（`openwch/ch583`） |
| 开发环境 | MounRiver Studio + WCHISPTool |
| 参考工程 | 官方 `HID_Keyboard` 例程 + **官方 2.4G 用例（D11，直接并入）**；社区「CH582M 三模丐 PAD」为整体方案参考 |

---

## 1. 背景与定位

CH582M 在本方案中作为**无线协处理器（Wireless Co-Processor）**，不承担键盘主控职责。

**做什么：**

- 接收 SK32 通过 UART 送来的 HID 报文，按当前模式通过 BLE 或 2.4G 转发给主机
- 管理 BLE 多主机配对信息与切换
- 管理 2.4G 私有协议连接与配对（配合 Dongle）
- 上报连接状态、HID LED 状态（Caps/Num/Scroll）、电池电量给 SK32
- 电源管理：空闲休眠、射频开关、电量采集

**不做什么（明确排除）：**

- 矩阵扫描
- 键位映射、分层、宏、Tap-Hold 等 QMK 逻辑
- 有线 USB HID（由 SK32 原生 USB 负责）
- RGB 灯效运算（SK32 负责，CH582M 仅可选提供 PWM/SPI 辅助，v0.1 不实现）

**设计原则：**

1. CH582M 是**无状态转发器**，键值语义全部来自 SK32，不做二次解释
2. 转发路径**零缓存、零批处理**，收到即发，优先保证延迟
3. 任何情况下不得因 CH582M 侧异常导致 SK32 卡键

---

## 2. 术语

| 术语 | 说明 |
|---|---|
| 桥（Bridge） | 本文档描述的 CH582M 固件 |
| 主控 | SK32F077，运行 QMK |
| Dongle | 2.4G 接收器，插在主机 USB 口 |
| Profile | BLE 已配对主机槽位，1~4 |
| 上行 | SK32 → CH582M → 主机 |
| 下行 | 主机 → CH582M → SK32 |

---

## 3. 系统架构与职责划分

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│  SK32F077  （主控，QMK）      │         │  CH582M  （桥，WCH SDK）      │
│                              │  UART   │                              │
│  · 矩阵扫描 / 消抖            │◄───────►│  · BLE HID（4 主机）          │
│  · 层 / 宏 / 键位映射         │ 1~2Mbps │  · 2.4G 私有 HID              │
│  · RGB / OLED                │         │  · 配对信息管理               │
│  · VBUS 检测 / 模式状态机     │  GPIO   │  · 电量采集                   │
│  · 有线 USB HID              │◄───────►│  · 低功耗 / 射频管理          │
│  · 无线模式：HID 报文 → UART  │         │  · LED 状态回传               │
└──────────┬───────────────────┘         └──────────┬───────────────────┘
           │ USB FS（仅有线模式）                    │ 2.4GHz RF
           ▼                                        ▼
      ┌─────────┐                              ┌─────────┐
      │  PC     │                              │ Dongle  │──► PC
      └─────────┘                              └─────────┘
```

---

## 4. 硬件接口规格

### 4.1 UART（数据面）

| 项 | 规格 |
|---|---|
| 实例 | **UART1**：PA8=RXD1（pin 6）/ PA9=TXD1（pin 7）——**已定**（D2） |
| 波特率 | **921600**（8N1）——**已定**（D14；原 38400，后统一提升） |
| 格式 | 8 数据位 / 无校验 / 1 停止位 / 无流控 |
| 方向 | 全双工 |
| FIFO | 使用芯片内置 8 级 FIFO |
| 中断优先级 | **最高**（高于射频与非易失存储操作） |

> 选型依据：UART0（PB4/PB7）留作调试 printf；UART2 只能走映射脚 PB22/PB23，而 PB23 为 `RST#`，**不可占用**；UART3（PA4/PA5）为备选，若 UART1 需让位可整组切换。详见 [ch582f_pinmap.md](./ch582f_pinmap.md) 第 2 章。

**921600 的带宽/延迟余量（D14）**

921600 bps 8N1 = 10 bit/字节 → **92160 字节/秒**。本协议帧开销固定：

| 报文 | 帧长 | 占线时间 @921600 |
|---|---|---|
| `HID_KEYBOARD`（8B payload） | 16 B | **0.17 ms** |
| `HID_NKRO`（16B payload） | 24 B | **0.26 ms** |
| `HID_CONSUMER`（2B payload） | 10 B | 0.11 ms |
| 控制类命令（≤8B payload） | ≤16 B | ≤0.17 ms |

> 说明：D14 早期曾定 38400（键盘帧占线 4.17ms），后**统一提升至 921600**，单帧占线降至约 1/24，
> 串口带宽不再构成瓶颈。以下两条设计约束**仍然保留**（成本极低，且有利于链路与射频占用）：

1. **HID 报文仍然事件驱动**（仅在键值/状态变化时发送），**不**改为按 1ms 周期无条件上报。
2. **两帧最小间隔 ≥5ms** 保留（见 6.3）：这是兼容低速主控/帧间处理开销的保守上界，不随波特率下调。

> 说明：D5 的「2.4G 上报率 1 ms」指的是**射频侧轮询间隔**，与串口波特率无关，仍然成立；
> 串口侧当前按事件驱动送帧，单帧占线 ≤0.26ms，端到端延迟目标（见第 14 章）有充足余量。


### 4.2 控制 GPIO

| 信号 | 桥侧引脚（QFN28） | 方向 | 电平语义 | 用途 |
|---|---|---|---|---|
| `MODE_SEL[1:0]` | PB12（pin 11）/ PB13（pin 10） | SK32 → CH582 | 00=USB, 01=BLE, 10=2.4G, 11=保留 | 快速模式指示（上电采样 + 运行期变化触发中断） |
| `WAKE` | PA5（pin 24） | CH582 → SK32 | 高有效（可配） | 请求 SK32 从 STOP 模式唤醒 |
| `nRESET` | PB23（pin 16，`RST#`） | SK32 → CH582 | 低有效 | 硬复位桥（内置上拉） |
| `LINK` | PA13（pin 27） | CH582 → SK32 | 高=已连接 | 连接状态硬件指示（可选，亦可纯软件上报） |

> 模式同时支持 **GPIO 硬线**与 **UART 命令**两种设定方式；UART 命令为准，GPIO 用于上电初值与异常恢复场景。

### 4.3 电源与检测

| 项 | 规格 |
|---|---|
| 供电 | 3.3V（由板级 DC-DC 提供） |
| 电池采集 | **PA14（pin 26，AIN4 / ADC ch4）**，分压采电池电压 |
| 充电状态 | **PA15（pin 25）** GPIO 输入，读取充电 IC 的 `CHRG` / `STDBY` |
| VBUS 检测 | 由 SK32 侧完成，经 UART 命令同步给桥 |
| 休眠控制 | 桥自行进入 Halt / Sleep，唤醒源见 9.3 |
| DC-DC 外围 | VDCID/VSW 需 2.2µF + 10µH 电感，VINTA 1µF；缺任一个可能导致蓝牙频繁掉线（见 pinmap 3.3） |

### 4.4 引脚分配约束（已定）

- **ISP 下载配置引脚 = PB22（pin 17）**，必须 **10k 上拉**，上电不得被拉低；**不得**用该脚承载 UART 或任何上电可能为低的信号。
- **USB 引脚**：PB10（pin 13，UD-）/ PB11（pin 12，UD+）。键盘侧桥不做 USB，留作备用；**Dongle 侧必须使用**（D7）。
- **PA10/PA11** 为 32.768kHz 低速晶振脚（X32KI/X32KO，LSE，**已实装**）：固件以 `CLK_OSC32K=0` 选外部 LSE（D15），作为 RTC / TMOS 时基 / WCH 硬件跳频同步基准；**严禁**再配置为 GPIO。
- QFN28（CH582F）可用 20 个 GPIO，本方案已占用 12 个，余 3~5 个备用（见 pinmap 第 3.4 节）。`LINK` 可软件化、`MODE_SEL` 可降到 1 根以进一步释放。
- 完整管脚表见 [ch582f_pinmap.md](./ch582f_pinmap.md) 第 3 章（键盘侧）与第 4 章（Dongle 侧）。

---

## 5. 工作模式与状态机

### 5.1 模式定义

| 模式 | 说明 | 射频 | 上报通道 |
|---|---|---|---|
| `MODE_USB` | 透传休眠。SK32 走原生 USB | 关闭 | 不使用（仅监听 UART） |
| `MODE_BLE` | BLE HID 已连接某 Profile | BLE 5.x | HOGP |
| `MODE_BLE_PAIR` | BLE 广播中，等待主机配对 | BLE 5.x | — |
| `MODE_2G4` | 2.4G 私有协议已连 Dongle | 2.4G | 私有协议 |
| `MODE_2G4_PAIR` | 2.4G 配对窗口开启 | 2.4G | — |
| `MODE_OTA` | 固件升级 | 视通道而定 | — |
| `MODE_SLEEP` | 深度睡眠（无连接且无活动） | 关闭 | — |

> `MODE_BLE_PAIR` / `MODE_2G4_PAIR` 是**只读的运行态**（桥进入配对窗口后自身持有的状态），**不能**由 `SET_MODE(0x03)` 直接指定；上位机要发起配对一律用 `PAIR_START(0x05)`。查询当前是否处于配对态用 `GET_MODE(0x0C)`。

### 5.2 模式优先级

```
VBUS 插入  >  SK32 显式 SET_MODE  >  上次记忆模式
```

- VBUS 插入时，桥**无条件**切 `MODE_USB` 并关闭射频
- VBUS 拔出后，恢复 EEPROM/DataFlash 中记忆的上次无线模式
- 记忆模式需掉电保持

### 5.3 状态迁移

| 当前 | 事件 | 目标 | 动作 |
|---|---|---|---|
| 任意 | 上电 / 复位 | 按 `MODE_SEL` 初值 | 读配置、初始化射频 |
| `MODE_USB` | VBUS 移除 | 记忆模式 | 初始化对应射频栈 |
| `MODE_BLE` | `SET_MODE(2.4G)` | `MODE_2G4` | 断开 BLE、启动 2.4G |
| `MODE_BLE` | `PAIR_START` | `MODE_BLE_PAIR` | 进入可发现广播 |
| `MODE_2G4` | 链路丢失 > 3s | `MODE_2G4_PAIR` | 重新配对窗口 |
| `MODE_BLE_PAIR` / `MODE_2G4_PAIR` | 配对超时 > 60s 未配对成功 | `MODE_BLE` / `MODE_2G4` | 停配对窗口（BLE 关广播 / 2.4G 停 `PAIR_REQ` 重发）、回 `EVT_PAIR_RESULT(1)`、解除深睡门闩 |
| 任意 | 无活动 > T_idle | `MODE_SLEEP` | 关射频、进 Sleep |
| `MODE_SLEEP` | 按键 / UART / 射频事件 | 原模式 | 快速恢复 |

> **切换前必须执行 `HID_RELEASE_ALL`（0x15）**，向主机发送全 0 释放报文，防止卡键。

---

## 6. UART 应用层协议

### 6.1 帧格式

```
+---------+-----+-----+-----+-------+---------+-------+
|  SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16 |
| AA 55   | 1B  | 1B  | 1B  |  1B   |  N B    |  2B   |
+---------+-----+-----+-----+-------+---------+-------+
```

| 字段 | 说明 |
|---|---|
| SOF | 固定 `0xAA 0x55` |
| LEN | `PAYLOAD` 字节数，0~64 |
| CMD | 命令码 |
| SEQ | 序号，0~255 循环，用于应答匹配 |
| FLAGS | bit0=需应答, bit1=应答帧, bit2=错误帧 |
| CRC16 | CRC-16/CCITT-FALSE，覆盖 `LEN` 起至 `PAYLOAD` 末 |

> 载荷内含 `0xAA 0x55` 时无需转义：接收端按 `LEN` 定长读取，仅在失步时重新搜索 SOF。

### 6.2 命令集

**上行（SK32 → 桥）**

| CMD | 名称 | Payload | 说明 |
|---|---|---|---|
| `0x01` | `GET_VERSION` | — | 查询固件版本 |
| `0x02` | `GET_STATUS` | — | 查询模式/连接/电量 |
| `0x03` | `SET_MODE` | mode(1B), persist(1B) | 切换模式；**不接受配对模式**（`MODE_BLE_PAIR`/`MODE_2G4_PAIR`，回 `PARAM` 错误），配对一律走 `PAIR_START` |
| `0x04` | `SET_PROFILE` | profile(1B, 1~4) | 切换 BLE 主机 |
| `0x05` | `PAIR_START` | — | 进入配对窗口 |
| `0x06` | `UNPAIR` | profile(1B, 0xFF=全部) | 删除配对信息 |
| `0x07` | `RESET` | — | 桥软复位 |
| `0x08` | `ENTER_BOOTLOADER`（实现名 `CMD_ENTER_BOOT`） | — | 进常驻 BOOT（OTA 收包模式），非 ROM ISP；见 10.1 |
| `0x09` | `SET_CONFIG` | key(1B), value(NB) | 配置项（上报率、连接间隔、广播名、**日志等级 `LOG_LEVEL`**，见 11.1） |
| `0x0A` | `GET_MAC` | — | 读芯片唯一 MAC（6B），用于广播名生成与配对标识 |
| `0x0B` | `SET_TEST_MODE` | mode(1B), channel(1B), power(1B) | **SRRC 认证专用**：mode=1 定频连续发射，mode=0 退出；功率档固定，不得超出申报值 |
| `0x0C` | `GET_MODE` | — | 模式状态查询：应答 3B `[mode][connected][radio]`（`radio` 0=无 / 1=BLE / 2=2.4G），比 `GET_STATUS` 更轻量，便于高频轮询 |
| `0x0D` | `SET_HEARTBEAT` | enable(1B, 0/1) | 心跳周期开关：1=开（默认）/0=关；**关闭后停止一切心跳相关功能**（不发 `PING`、不做失联判定、不回 `PONG`）。该设置不落盘，复位后恢复默认开 |
| `0x0E` | `GET_WIRELESS_STATE` | — | 无线综合状态查询：应答 1B `[wl_state]`（`BRIDGE_WL_*`，0=USB / 1=空闲 / 2=BLE 配对中 / 3=2.4G 配对中 / 4=BLE 已连 / 5=2.4G 已连 / 6=BLE 无连接(重连中) / 7=2.4G 无连接 / 8=OTA / 9=休眠）。已把 `[mode][connected][radio]` 折叠为单枚举，SK32 查表即得、免组合判断 |
| `0x10` | `HID_KEYBOARD` | 8B | 标准键盘报文 |
| `0x11` | `HID_NKRO` | 16B | NKRO 报文（BLE 模式下由桥降级为 6KRO） |
| `0x12` | `HID_CONSUMER` | 2B | 多媒体键 |
| `0x13` | `HID_MOUSE` | 4B | 鼠标 |
| `0x14` | `HID_SYSTEM` | 1B | 系统键（电源等） |
| `0x15` | `HID_RELEASE_ALL` | — | 全键释放 |
| `0x18` | `TUNNEL_USB` | ≤64B | 透传到 Dongle/USB（VIA Raw HID 通道），无 ACK |
| `0x19` | `TUNNEL_BLE` | ≤32B | 透传到 BLE（手机 APP 通道），无 ACK |
| `0x1A` | `NV_WRITE` | addr(1B), data(32B) | 写用户 NV 单元（每单元 32B，`addr` 越界回 `PARAM`） |
| `0x1B` | `NV_READ` | addr(1B) | 读用户 NV 单元，应答 addr(1B)+data(32B) |
| `0x1C` | `SET_BATTERY` | percent(1B, 0~100) | SK32 上报电量，优先于本地 ADC；随后回 `EVT_STATE` |
| `0x1D` | `SLEEP` | level(1B) | 睡眠分级：0=唤醒 / 1=浅睡 / 2=深睡（配 `SET_CONFIG` key `SLEEP_POLICY`） |
| `0x1E` | `GET_ADC` | — | 读本地 ADC 电压，应答 mv(2B 小端) |
| `0x1F` | `SET_NAME` | target(1B), name... | 运行时改标识：target 0=BLE 广播名 / 1=USB(Dongle) 名（另一侧经 2.4G 中继） |
| `0x20` | `SET_USB_ID` | vid(2B LE), pid(2B LE) | 运行时改 USB VID/PID（Dongle 侧重枚举） |
| `0x21`~`0x24` | `FW_BEGIN` / `FW_DATA` / `FW_END` / `FW_ABORT` | 见 §10 | 键盘侧 UART OTA 帧；升级流程由 `board/ota_web` 承担 |
| `0x7F` | `PING` | — | 心跳 |

> `0x18`~`0x24` 为借鉴市售模组指令集新增的扩展命令，完整 payload 与应答格式见
> [debug_web/SPEC.md](./ch582f_fw/board/debug_web/SPEC.md) §4.6 / §4.7；`0x0F`、`0x16`、`0x17` 保留未用。

**下行（桥 → SK32）**

| CMD | 名称 | Payload | 说明 |
|---|---|---|---|
| `0x80` | `EVT_STATE` | mode, link, profile, battery | 状态变化主动上报 |
| `0x81` | `EVT_LEDS` | led_state(1B) | Caps/Num/Scroll，映射 `keyboard_leds()` |
| `0x82` | `EVT_BATTERY` | mv(2B), percent(1B) | 周期上报 |
| `0x83` | `EVT_PAIR_RESULT` | result(1B), profile(1B) | 配对结果；`result=0` 成功、`=1` 失败/超时（配对窗口超过 `BRIDGE_PAIR_TIMEOUT_S` 未成功时发一次瞬时事件） |
| `0x84` | `EVT_ERROR` | code(1B) | 错误码 |
| `0x85` | `EVT_TUNNEL_USB` | ≤64B | USB/Dongle 透传上报（VIA Raw HID 通道），无应答 |
| `0x86` | `EVT_TUNNEL_BLE` | ≤32B | BLE 透传上报（手机 APP 通道），无应答 |
| `0x87` | `EVT_HOST_POWER` | state(1B) | 主机电源状态变化（0=运行/唤醒，1=挂起） |
| `0x7F` | `PONG` | — | 心跳应答 |

### 6.3 时序与流控

- 桥收到 `HID_*` 报文后**立即转发**，不缓存、不合帧
- 桥→SK32 的下行事件与 SK32→桥 的上行报文**逻辑独立**，全双工并行
- 高频上行（按键）**不要求应答**（FLAGS.bit0=0），避免回程开销
- 低频控制命令**要求应答**，未应答 3 次触发 `EVT_ERROR`

**波特率统一为 921600 后的强制要求（D14）：**

- **事件驱动发送**：只在键值/修饰键/指示灯等状态**发生变化**时发送 `HID_*` 帧；相同状态重复上报必须抑制（发送前做 last-report 比较）
- **两帧间最小间隔 ≥ 5 ms**（保守上界，兼容低速主控与帧间处理开销），SK32 侧发送队列必须串行化，不得背靠背连发
- **背压保护**：桥侧 UART TX 忙时不缓存新 HID 帧，仅保留**最新一帧**（覆盖旧值），避免按键积压造成"延迟输入"
- 控制类命令（`PAIR_START` 等）与 HID 帧**不得在同一时刻竞争发送**：HID 优先，控制命令排队
- 断链检测周期不变（1 s `PING`），但 `PING` 帧应与 HID 帧错开，避免占用已被按键占满的串口时间

### 6.4 断链检测

- SK32 每 **1s** 发 `PING`，桥回 `PONG`
- 桥连续 **3 次**未收到 `PING`：判定 UART 失联 → 释放当前所有按键 → 关闭射频 → 进 `MODE_SLEEP`（等待 UART 唤醒）
- 桥侧 UART 接收超时（帧内字节间隔 > 5ms）→ 丢弃半帧，重新搜索 SOF
- 心跳周期可用 `SET_HEARTBEAT(0x0D)` 整体关闭；关闭后桥**不发 `PING` 也不做失联判定**（计数保持清零、链路始终视为有效），适用于 SK32 不参与心跳的台架/直连场景。该开关不落盘，复位后恢复默认开启

---

## 7. BLE 模式功能规格

### 7.1 角色与协议

| 项 | 规格 |
|---|---|
| 角色 | GATT Server / HID Peripheral |
| 协议 | HID over GATT（HOGP） |
| 蓝牙版本 | 按 SDK 实际支持（数据手册 V1.8 标 BLE 5.0，官网方案页标 5.3，以 SDK 为准） |
| PHY | 2M（优先）/ 1M |
| 广播名 | `SK32-3M-XXXX`，XXXX = 芯片 MAC 低 16 位十六进制大写（已定） |

### 7.2 Report Map

| Report | 长度 | 说明 |
|---|---|---|
| Keyboard | 8B | Boot 键盘报文，必选 |
| Consumer | 2B | 多媒体，必选 |
| System | 1B | 可选 |
| Mouse | 4B | 可选（若键盘带指点杆/旋钮） |
| NKRO | 16B | **BLE 模式下禁用**（已定）；仅 2.4G 模式直传 16B |

> 主机下发的 **HID Output Report（LED）** 必须解析并转 `EVT_LEDS` 上报给 SK32。

### 7.3 多主机管理

- 支持存储 **4 组**绑定信息（bonding），存于 DataFlash
- 切换方式：`SET_PROFILE(1~4)`；未配对的 Profile 自动进入配对流程
- 切换时：断开当前连接 → `HID_RELEASE_ALL` → 重连目标 Profile（或进入配对）
- 配对信息结构与擦写需做**掉电保护**（双备份 + 校验）

### 7.4 配对与解除

| 操作 | 触发 | 表现 |
|---|---|---|
| 配对 | `PAIR_START` | 进入可发现广播，限时 60s |
| 解除单槽 | `UNPAIR(profile)` | 删除该槽绑定信息 |
| 解除全部 | `UNPAIR(0xFF)` | 清空并回 `MODE_BLE_PAIR` |
| 配对成功/失败 | — | 回 `EVT_PAIR_RESULT(result, profile)` |
| 配对超时 | 配对窗口开启 > `BRIDGE_PAIR_TIMEOUT_S`（60s）未配对成功 | 停止配对动作（BLE 关广播 / 2.4G 停 `PAIR_REQ` 周期重发），回落对应非配对无线模式（`MODE_BLE` / `MODE_2G4`），并回一次**瞬时** `EVT_PAIR_RESULT(1, profile)` |

### 7.5 连接参数

| 参数 | 目标值 |
|---|---|
| Connection Interval | 最小允许值（7.5ms） |
| Slave Latency | 0（追求低延迟） |
| Supervision Timeout | ≥ 2s |
| 重连策略 | 断连后自动重连，指数退避，上限 3s 间隔 |

### 7.6 LED 状态回传

主机 LED 状态经 HID Output Report 到达桥后：

1. 桥缓存 `led_state`（bit0=Caps, bit1=Num, bit2=Scroll）
2. 通过 `EVT_LEDS` 上报 SK32
3. SK32 在 `keyboard_leds()` 回调中返回该缓存值

> 这是 annepro2 未实现（`return 0`）的部分，必须补齐，否则无线模式下指示灯失效。

---

## 8. 2.4G 模式功能规格

> **已定（D11）**：2.4G 私有链路**直接采用厂商提供的 2.4G 用例**（WCH EVT 中的 2.4G 例程，即丐 PAD 同源方案），
> 不自行设计射频私有协议、不自行实现跳频/重传/配对算法。
> 桥固件在 2.4G 侧只做三件事：**初始化厂商链路 → 把 HID 报文交给厂商发送接口 → 把厂商回调转成 `EVT_*` 事件**。
> 因此 8.1 / 8.3 的具体机制**以厂商用例的实现为准**，本文只约定对外契约。

### 8.1 配对机制（沿用厂商用例）

- 基于芯片**全球唯一 MAC** 配对（`GET_MAC` 可读）
- 首次使用：插入 Dongle 后自动进入配对，或 `PAIR_START` 手动触发
- 配对信息写入 DataFlash 掉电保存；键/接收地址由厂商用例管理，桥不解析其内部结构
- Dongle 侧同样直接使用**配套的厂商接收端用例**（D7：Dongle 也是 CH582F）

### 8.2 上报率与延迟

| 项 | 目标 |
|---|---|
| 射频上报率 | **1000 Hz（1ms）**（D5）；可选降档以省电 |
| 串口送帧 | **事件驱动**，两帧间隔 ≥5ms（D14，见 6.3） |
| 端到端延迟 | **≤ 8 ms**（D14 目标；原始 38400 下单帧占线即 4.17ms，提升到 921600 后串口仅 ≈0.26ms，余量充足） |
| 丢包重传 | 私有协议自带，窗口内重传 |

> 1ms 上报需要厂商用例支持该上报周期配置（连接参数 / 发送间隔）。
> 若厂商用例固定为更低速率，须先向 WCH FAE 确认可配置项，**不得**自行改射频时序。
> 注意区分：**射频 1ms 轮询**与**串口送帧时刻**是两件事，后者受 D14 约束为事件驱动。

### 8.3 跳频与抗干扰（厂商实现）

- 采用厂商私有自适应跳频，避开拥挤信道
- 干扰导致的连续丢包 → 由厂商用例上报链路状态，桥据此触发 `EVT_ERROR` 并可回退到重配对

> **as-built 修订（结论2/3/4/5 落地后）**：上述"私有自适应跳频"经核对厂商例程**不成立**，
> 实为固定信道，抗干扰在本工程按以下方式实现（详见 checklist §12.6）：
> - **WCH 硬件跳频已启用**（`BRIDGE_FEATURE_RF24_HOP=1`，对照参照工程 3modekbd）：厂商
>   `RF_FrequencyHopping{Tx,Rx}` 骨架不带应用载荷、TX/RX 非对称，本桥在其上叠加自实现的载荷
>   承载——键盘=initiator 只发（空闲 10ms 轮询），Dongle=responder 只收、下行经 `RF_Rx` 的
>   自动 ACK 载荷回传。跳频同步以**外部 32.768kHz LSE**（Makefile `CLK_OSC32K=0`，D15）为基准，
>   双芯同源不失步。（早期文档误记 LSE「可选未接」，据此得出的「硬件跳频不可行」结论**作废**。）
> - **链路层有界重传**：未收 ACK 即重发同一帧（`RF24_TX_RETRY_MAX=4`）。
> - **帧级序号去重（结论3）**：帧头 `[type][link_id LE(2)][seq]`，收侧按 `seq` 去重，
>   消除重传导致的重复上报（对照参照工程 `num + magicword`）。
> - **绑定派生 access address（结论4）**：两端由绑定信息派生唯一 AA，降低同频互扰。
> - **PHY 保持 1M（结论5）**：soak 10000 实测 2M PHY 丢包 1.67%（固定信道下灵敏度降约 3dB，
>   负优化），回退 1M 后 0.85%；跳频启用后仍保留 1M，2M 留待后续再评估。

### 8.4 Dongle 端契约

- Dongle 固件与键盘固件**必须配套**（配对参数、协议版本一致），且**同源于同一份厂商用例**
- Dongle 通过 USB HID 向主机上报
- 协议版本号需在握手阶段比对，不一致则拒绝连接

#### 8.4.1 USB 复合设备接口布局（as-built）

Dongle 以**复合设备**枚举（3 接口 / EP1~EP3），使主机侧无需驱动即可同时获得
键盘、辅助键与 VIA 配置通道（实现见 `port/ch58x/bridge_usb_ch58x.c`）：

| 接口 | 用途 | 端点 | 报文 |
|---|---|---|---|
| IF0 | 键盘（Boot） | EP1 IN 8B | 修饰键+保留+6 键位；LED 经 EP0 `SET_REPORT` 下发 |
| IF1 | 辅助键 | EP2 IN 32B | Report ID 区分：鼠标 4B / 系统 1B / 多媒体 2B / NKRO 16B |
| IF2 | VIA Raw HID | EP3 IN+OUT 32B | usage page `0xFF60` / usage `0x61`（QMK `RAW_EPSIZE`） |

- 报文字节口径以对端 SK32 的 QMK `tmk_core/protocol/usb_descriptor.c` 为准
- **VID/PID 必须与目标键盘的 `via.json` 一致**（VIA 据此匹配键盘定义）；
  当前默认 `0x0194:0x0463`（KB17），配套 LD7_OLED 时在 `board/dongle/bridge_board.h`
  覆盖为 `0x0194:0x0464`
- 数据方向：键盘经 2.4G 上报的 HID/透传 → `sdk_usb_hid_send()` / `sdk_usb_tunnel_send()`；
  主机下发的 Raw HID（EP3 OUT）与 LED（EP0 `SET_REPORT`）→ 回传键盘

### 8.5 与 SRRC 的关系（重要）

- 因 2.4G 链路采用厂商用例，**定频测试模式（`0x0B`）的实现能力取决于厂商用例是否暴露接口**
- 认证前需向 WCH FAE 确认：厂商用例是否提供定频发射（固定信道 + 固定功率）入口
- 若厂商用例不提供，则由 WCH 提供产测固件配合认证测试，键盘/Dongle 量产固件仅保留 BLE 侧定频


---

## 9. 电源管理

### 9.1 电流预算（目标值，需实测标定）

| 状态 | 目标电流 |
|---|---|
| 有线模式（射频关，MCU Halt） | < 1 mA |
| BLE 已连接空闲 | ≤ 1 mA（平均） |
| 2.4G 已连接空闲（1kHz） | ≤ 2 mA（平均） |
| 无连接待机 | < 10 µA |
| 深度睡眠 | 最低档（芯片 Sleep 档 0.7~2.8 µA） |
| 射频发射峰值 | 数十 mA（芯片 TX 最大 +6 dBm） |

### 9.2 空闲与睡眠策略

- 无 UART 活动且无射频事件持续 `T_idle`（默认 30s）→ 进 `MODE_SLEEP`
- 进入前：确保绑定信息已落盘，`HID_RELEASE_ALL` 已发送
- 处于配对窗口（`MODE_BLE_PAIR` / `MODE_2G4_PAIR`）时**不入睡**；2.4G 断链进入配对窗口后，若
  `BRIDGE_PAIR_TIMEOUT_S`（60s）内未配对成功则超时回落 `MODE_2G4`，此后无活动即按 `T_idle` 正常入睡
  （修复：原实现停在 `MODE_2G4_PAIR` 会令自动深睡永不触发）

### 9.3 唤醒源

| 源 | 说明 |
|---|---|
| UART RX | SK32 有报文 |
| `MODE_SEL` 变化 | 用户切换模式 |
| 射频事件 | 连接请求 / 主机下行数据 |
| RTC | 周期唤醒（心跳、电量采样） |

> 桥被唤醒后需拉 `WAKE` 通知 SK32（若 SK32 处于 STOP）。

### 9.4 电量采集与告警

- ADC 周期采样电池电压（默认 10s 一次，`MODE_SLEEP` 时降频）
- 分压系数与电池曲线在 `InfoFlash`/配置区标定
- 阈值：低于 **20%** 首次告警一次，低于 **5%** 重复告警

### 9.5 充电时行为

- 检测到充电（`CHRG` 有效）→ 建议切 `MODE_USB`（由 SK32 决策）
- 充电中不禁止无线，但需在异常排查时提供"充电强制有线"开关

---

## 10. 固件升级

### 10.1 OTA（**常驻 BOOT + Stage 暂存**，已定 D12）

- 采用「**一个常驻 BOOT**」方案：不划分 A/B 双应用区；APP_A 为唯一应用区，升级镜像先写入 **Stage 暂存区**，再由 BOOT 搬运覆盖 APP_A。
- 分区（见 `port/ch58x/bridge_ota_layout.h`）：`BOOT@0x0(8K)` + `APP_A@0x2000(216K)` + `Stage@0x38000(216K)`。
- 升级流程（键盘 UART / Dongle USB HID 两通路一致，两阶段标志驱动）：
  1. App 收「进入 OTA」请求（键盘 `CMD_ENTER_BOOT(0x08)` / Dongle HID `ENTER_OTA`）→ 释放按键 + 写标志 `UART_IAP`(键盘) 或 `USB_IAP`(Dongle) → 软复位；
  2. BOOT 见该标志即驻留**收包模式**（键盘 `iap_uart_ota_run` / Dongle `iap_usb_main`），收 `FW_BEGIN/DATA/END` → 写 Stage → CRC 校验通过 → 写标志 `IAP` → 复位；
  3. BOOT 见标志 `IAP`：复验 Stage CRC → 按记录长度将 Stage 搬到 APP_A（1KB 分块）→ 写标志 `A` → 擦 Stage → 跳 `0x2000` 新 App。
- **断电安全 / 可回退**：先写 `IAP` 再搬运，搬运未完则下次上电自动重试；Stage 仅在标志置 `A` 之后才擦；取消/失败/空闲超时（60s）写 `A` 回原 App（APP_A 自始未动）——**非「单区无回滚」**。
- 升级期间禁止转发 HID，需先 `bridge_hid_release_all()`，并禁止进入 `MODE_SLEEP`。
- BOOT 重跑期间不发周期心跳，仅应答 `PING`（回 `PONG`）；不支持的 CMD 忽略不应答。
- 触发方式：键盘 UART `CMD_ENTER_BOOT`；Dongle HID `ENTER_OTA`（进常驻 BOOT）或 ASCII `"OTA!"`（进 ROM ISP，见 10.2）。

### 10.2 有线 ISP / 恢复通道

两条触发路径，按硬件形态区分——**键盘侧模组无 USB，只能走 A；Dongle 无 SK32，只能走 B**：

| 路径 | 触发方 | 通道 | 触发命令 | 去向 |
|---|---|---|---|---|
| A | SK32（QMK）/ PC 串口 OTA | UART1 | `CMD_ENTER_BOOT (0x08)` | 常驻 BOOT（UART 收包，非 ROM ISP） |
| B | PC OTA 工具 | USB EP3 OUT | 首 4 字节 ASCII `"OTA!"` | ROM ISP（真正有线 ISP 兜底） |

**路径 A（键盘侧模组）**：SK32 或 PC 串口工具下发 `CMD_ENTER_BOOT`，桥先 `bridge_hid_release_all()` 再 `port_enter_bootloader()`；键盘侧 `port_enter_bootloader()` 只写标志 `UART_IAP` + 软复位，**进入本工程的常驻 BOOT**（`iap_uart_ota_run`）而非 ROM ISP——键盘无 USB，恢复依赖常驻 BOOT 自身。
实现见 `core/bridge_proto.c` 的 `CMD_ENTER_BOOT` 分支与 `port/ch58x/bridge_port_ch58x.c`。

**路径 B（Dongle，as-built）**：

- 通道：USB EP3 OUT（VIA Raw HID，32B，usage page `0xFF60` / usage `0x61`，见 8.4.1）
- 触发报文：**首 4 字节 = `4F 54 41 21`（ASCII `"OTA!"`）**，其余字节忽略
- 命中后动作：`bridge_hid_release_all()` 松开所有按键 → `port_enter_bootloader()`
  （`BRIDGE_PIN_ISP_CFG = PB22` 拉低 2ms + 软复位）→ **进 ROM ISP，函数不返回**
- 未命中：整帧（含 Report ID）按原有 VIA 隧道行为经 2.4G 转发给键盘 SK32，行为不变
- 魔数选型理由：VIA 隧道报文首字节是 Report ID（常为 `0x00`），不会与 `"OTA!"` 误撞
- **主机时序**：PC 工具发出 `"OTA!"` 后 USB 立即断开并以 ISP 设备重新枚举（VID/PID 与运行时不同），
  PC 工具须**等待重新枚举完成**再用 WCHISPTool / WCH-LinkUtility 烧写
- 安全定位：ROM ISP 是**应用+常驻 BOOT 均损坏**时的最终兜底手段，量产固件必须保留该通道；
  不设鉴权（信任模型与 SK32 的 DFU boot 键同级：需物理接触 USB）
- **待核实**：`port_enter_bootloader()` 的 ISP 采样时机（PB22 拉低须覆盖复位瞬间），实现处自带 `TODO(核实)`
- **待落地**：PC 工具侧「发魔数 → 等枚举 → 烧写」的自动化流程

量产烧录用 WCHISPTool。

### 10.3 与 SK32 DFU 的关系

- 两套升级通道**完全独立**：SK32 走自身 DFU（`dfu-util -a 2`），桥走 WCH ISP/OTA
- 无线模式下 SK32 不暴露 USB，故 SK32 升级需先切有线模式

### 10.4 Flash 布局约束（常驻 BOOT）

| 区域 | 大小 | 说明 |
|---|---|---|
| BOOT | 8 KB (`0x0..0x1FFF`) | 本工程常驻引导（含 UART/USB 收包 + Stage→APP_A 搬运），不可经 OTA 覆盖 |
| APP_A | 216 KB (`0x2000..0x37FFF`) | 唯一应用区，OTA 目标；由 BOOT 搬运覆盖 |
| Stage | 216 KB (`0x38000..0x6DFFF`) | 暂存区，收到的整包镜像先写到这里 |
| 顶部余量 | 8 KB (`0x6E000..0x6FFFF`) | 未分配 |
| DataFlash | 32 KB (`0x70000..0x77FFF`) | 配对信息 / 配置 / OTA 标志（`0x77200`，NV 预留槽 8..11） |
| InfoFlash | 8 KB | 芯片配置 / 校准 |

> **已定（D12）**：不使用外挂 Flash；OTA 采用「常驻 BOOT + Stage 暂存」方案，不划分 A/B 双应用区。
> 应用可用空间为 **216 KB**（非旧稿的「约 300KB」），代价是升级期间需暂存一份镜像；
> 但 APP_A 在搬运完成前始终未动，搬运中断可自动重试、取消/失败可回退，**并非无回滚**。
> 投板前须实测应用占用（≤ 216KB），确认余量充足（建议保留 ≥ 15% 供后续功能迭代）。


---

## 11. 异常处理与可靠性

| 异常 | 处理 |
|---|---|
| UART 帧 CRC 错 | 丢弃该帧，计数；连续 10 次触发 `EVT_ERROR` |
| UART 失联 | 见 6.4 |
| 射频断连 | 自动重连 + 退避 |
| 主机长时间无响应 | 断开并回退到配对态 |
| 看门狗 | 启用独立看门狗，超时复位桥 |
| 桥复位 | 复位后必须向 SK32 发 `EVT_STATE`，并执行 `HID_RELEASE_ALL` 防卡键 |
| OTA 失败/中断 | 标志保持 `IAP`，下次上电自动重试搬运；取消/超时写 `A` 回原 App（可回退）。仅当应用+BOOT 均损坏才需 ROM ISP（见 10.1 / 10.2） |

### 11.1 调试日志（printf 开关，已定 D13）

固件必须支持**可配置的 printf 日志**，用于开发调试与量产问题定位，但**不得影响桥链路与上报时序**。

**硬约束：**

| 约束 | 说明 |
|---|---|
| 输出通道 | **只能走调试串口 UART0（PB4=RX / PB7=TX）**，**严禁**使用 UART1（PA8/PA9，桥↔SK32 数据面） |
| 与桥协议隔离 | 日志为纯文本流，与 6.1 的二进制帧**物理上不同串口**，不得混流 |
| 高频路径 | `bridge_hid` 转发路径（0x10~0x15）**禁止**任何日志输出，避免破坏 1ms 上报时序 |
| 中断上下文 | ISR 内禁止调用阻塞式 printf；需要时只置标志，由任务侧打印 |
| 量产默认 | **默认关闭** |

**两级开关：**

| 层级 | 开关 | 说明 |
|---|---|---|
| 编译期 | `BRIDGE_LOG_ENABLE`（0/1） | 置 0 时全部日志代码被预处理器移除，**零 Flash / 零 CPU 开销**；量产固件置 0 |
| 编译期 | `BRIDGE_LOG_LEVEL`（0~4） | 0=off / 1=ERROR / 2=WARN / 3=INFO / 4=DEBUG，编译期裁剪等级以下代码 |
| 运行期 | `SET_CONFIG` key = `LOG_LEVEL` | 复用 0x09 命令，运行期改等级并掉电保存；**仅在编译期 `BRIDGE_LOG_ENABLE=1` 时有效** |

**日志格式（建议）：**

```
[<uptime_ms>][<LEVEL>][<模块>] message\r\n
例: [00012345][WARN][uart] crc error cnt=3
```

- 模块名固定短标签：`uart` / `proto` / `ble` / `rf24` / `mode` / `hid` / `pwr` / `test`
- 波特率独立于桥串口，默认 **921600** 8N1（可配）
- 运行期改 `LOG_LEVEL` 需**立即生效**，不必复位（`BRIDGE_LOG_ENABLE=0` 时该命令返回「不支持」错误码）

**交付要求：** 需提供**两个构建目标**——`debug`（日志开、等级 4）与 `release`（日志关），可用同一份源码通过编译开关切换。


---

## 12. 资源预算

| 资源 | 总量 | 预留/占用 | 可用 |
|---|---|---|---|
| CodeFlash | 448 KB | BOOT 8K + APP_A 216K + Stage 216K + 顶部余 8K（**常驻 BOOT，应用可用 216K**） | 应用 ≤ 216 KB，需实测 |
| DataFlash | 32 KB | 4 组绑定 + 配置 + 日志等级 | 充裕 |
| SRAM | 30 KB + 2 KB | 射频栈 + 协议缓冲 + 日志缓冲（约 256~512 B） | 需实测 |
| GPIO | **20（QFN28）** | UART 2 + 控制 5 + ADC 1 + 充电 1 + ISP 1 + 调试 2 = 12 | 余 3~5，见 pinmap 3.4 |

> 日志关闭时（`BRIDGE_LOG_ENABLE=0`）不占用 SRAM 与 CodeFlash；调试构建约增加 4~8 KB CodeFlash 与 0.5 KB SRAM。


---

## 13. 与 SK32（QMK）侧的接口契约

CH582M 侧需 SK32 侧配合完成以下改动（**不在本文件实现范围**）：

### 13.1 需要 SK32 侧实现

1. **自定义 `host_driver_t`**：实现 6 个回调，映射到本协议：
   | QMK 回调 | 协议命令 |
   |---|---|
   | `send_keyboard` | `0x10 HID_KEYBOARD` |
   | `send_nkro` | `0x11 HID_NKRO` |
   | `send_mouse` | `0x13 HID_MOUSE` |
   | `send_consumer` | `0x12 HID_CONSUMER` |
   | `send_system` | `0x14 HID_SYSTEM` |
   | `keyboard_leds` | 返回 `EVT_LEDS` 缓存值 |
2. **驱动切换**：仿照 `keyboards/annepro2/annepro2_ble.c` 的 `host_set_driver()` 模式，切换前 `clear_keyboard()`
3. **NKRO 处理**：切无线时 `keymap_config.nkro = false`
4. **模式状态机与 VBUS 检测**：决策模式并通过 `SET_MODE` 下发
5. **心跳**：1s 周期 `PING`
6. **唤醒联动**：STOP 模式下响应桥的 `WAKE`（复用 LD7/KB17 现有 EXTI 唤醒逻辑）

### 13.2 有线/无线互斥

无线模式下 SK32 **必须真正断开 USB**（不能只是不发报文），否则主机会枚举出两个 HID 键盘。

---

## 14. 验收指标（KPI）

| 指标 | 目标 |
|---|---|
| 2.4G 端到端按键延迟 | **≤ 8 ms**（D14 调整） |
| BLE 端到端按键延迟 | **≤ 25 ms**（D14 目标；含串口 ≈0.26 ms + BLE 连接间隔） |
| 串口单帧占线时间 | 键盘帧 ≤ 0.3 ms（@921600） |
| 连续按键吞吐 | ≥ 50 次/秒（每次 1 帧键盘报文，含 5ms 间隔余量） |
| 冷启动到可连接 | < 500 ms |
| 模式切换完成 | < 200 ms（不含串口排队） |
| 断线自动重连 | < 3 s |
| 双设备枚举 | 无线模式下主机仅见 1 个 HID |
| 续航（1000 mAh，关灯，8h/天） | ≥ 30 天（目标，需实测） |
| 卡键 | 0 次（模式切换/断连/复位场景全覆盖） |
| SRRC 定频发射 | `SET_TEST_MODE` 可指定信道与功率档，频谱仪实测符合申报值 |

---

## 15. 设计决策与遗留事项

### 15.1 已定决策（详见 [ch582f_pinmap.md](./ch582f_pinmap.md) 第 0 章）

| # | 事项 | 决策 |
|---|---|---|
| D1 | 芯片封装 | CH582F（QFN28，20 GPIO），键盘侧与 Dongle 侧同型号 |
| D2 | 桥↔SK32 串口 | **UART1：PA8=RXD1 / PA9=TXD1**，2 Mbps |
| D3 | NKRO over BLE | 禁用 |
| D4 | NKRO over 2.4G | 保留（16B 直传） |
| D5 | 2.4G 上报率 | 1000 Hz（1 ms） |
| D6 | BLE 广播名 | `SK32-3M-XXXX`（XXXX = MAC 低 16 位） |
| D7 | Dongle 主控 | CH582F |
| D8 | 外挂 Flash | 不使用 |
| D9 | SRRC | 需支持，须加定频测试模式 |
| D10 | 触摸按键 | 不使用 |
| D11 | 2.4G 实现 | **直接采用厂商 2.4G 用例**（同源丐 PAD 方案），不自研射频私有协议；Dongle 用配套接收端用例 |
| D12 | OTA 方案 | **常驻 BOOT + Stage 暂存**（不划分 A/B 双应用区）：BOOT 8K + APP_A 216K + Stage 216K；APP_A 搬运前不动，可自动重试/回退；恢复靠常驻 BOOT，ROM ISP 兜底 |
| D13 | printf 日志 | **可配置开关**：编译期 `BRIDGE_LOG_ENABLE`/`BRIDGE_LOG_LEVEL` + 运行期 `SET_CONFIG(LOG_LEVEL)`；输出走 UART0，量产默认关 |
| D14 | 桥串口波特率 | **921600**（8N1；原 38400，后统一提升）。单帧占线 ≤0.26ms；**HID 仍事件驱动、两帧间隔 ≥5ms**；端到端目标 2.4G ≤8ms / BLE ≤25ms（余量充足） |
| D15 | 32K 低速时钟源 | **外部 32.768kHz LSE 晶振**（PA10=X32KI / PA11=X32KO，**已实装**）。固件以 `CLK_OSC32K=0` 选外部 LSE，作为 RTC / TMOS 时基 / WCH 硬件跳频同步的基准（**更正**：早期文档误记该晶振为「可选未接」） |

### 15.2 遗留待确认

1. **SK32 侧 GPIO 可用性**：已核实串口走 USART1（PA9/PA10）；MODE_SEL0/1、WAKE、LINK、nRESET 的 SK32 侧引脚为 PB3/PB4/PB6~PB8 建议值，须按最终矩阵/扩展器件复核（见 pinmap 第 7 章）
2. **ISP 下载配置引脚**：已定 PB22（pin 17，10k 上拉），但 WCHISPTool 实际可选项有限，投板前需在工具中确认 PB22 可选；若不可选须整体改板
3. **应用空间实测**：需实测应用占用（≤ 216KB，见 10.4），确认 ≥15% 余量；Stage 区随 APP_A 固定 216KB（不占应用可见空间）
4. 认证范围：SRRC 之外是否还需 CE / FCC
5. 是否采用已认证模组替代自研射频板（可大幅降低认证成本）
6. **厂商 2.4G 用例的可配置项**：上报周期能否配到 1ms（D5）、是否暴露定频发射接口（D9/D11，见 8.5）——须向 WCH FAE 确认
