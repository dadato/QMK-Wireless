# CH582M 无线桥模组 命令控制协议（Bridge Protocol）

> 适用范围：`wireless/ch582f_fw` 桥固件（键盘侧 / Dongle 侧共用同一套 `core/` 协议栈）。
> 协议权威定义：[`core/bridge_types.h`](ch582f_fw/core/bridge_types.h)、[`core/bridge_proto.c`](ch582f_fw/core/bridge_proto.c)、[`tools/sk32sim/proto.py`](ch582f_fw/tools/sk32sim/proto.py)。
> 本文档为**逐命令详解**：每条命令给出「功能 → 现象/时序 → payload 逐字节与取值含义 → 应答 → 错误码 → 示例帧」。示例帧 CRC 均由 `sk32sim.proto.encode()` 实际计算，可直接回放。

---

## 目录

1. [概览与角色](#1-概览与角色)
2. [物理层](#2-物理层)
3. [帧格式](#3-帧格式)
4. [时序与流量约束](#4-时序与流量约束)
5. [FLAGS 与应答模型](#5-flags-与应答模型)
6. [返回码与错误码](#6-返回码与错误码)
7. [枚举总表](#7-枚举总表)
8. [命令总表](#8-命令总表)
9. [逐命令详解](#9-逐命令详解)
10. [逐事件详解](#10-逐事件详解)
11. [SET_CONFIG 配置键表](#11-set_config-配置键表)
12. [典型交互流程](#12-典型交互流程)

---

## 1. 概览与角色

桥固件是运行在 CH582M 上的「无线桥」：一侧通过 **UART1** 与外挂主控（MCU/SK32 模组宿主，如键盘主控或 PC 上位机）通信，另一侧通过 **BLE / 2.4G** 无线连接受管设备（PC、手机等）。上层通过本协议下发命令，桥负责无线建链、HID 转发、透传、NV 存储、固件升级等。

固件分层：

```
core/            协议与状态机（纯逻辑，PC 可测）
  bridge_types.h   ← 命令/事件/枚举权威定义
  bridge_proto.c   ← 帧分发 + 各命令 handler
  bridge_uart.c    ← 收发/编码/CRC/心跳
  bridge_mode.c    ← 模式状态机（USB/BLE/2.4G/OTA/SLEEP）
  bridge_cfg.c     ← NV 配置
  bridge_hid.c     ← HID 报表转发
  bridge_ota.c     ← 固件升级
  bridge_test.c    ← SRRC 射频测试
port/ch58x/       WCH SDK 适配层（radio / adc / flash / tunnel…）
board/keyboard   键盘侧：无 USB，BLE+2.4G，可睡，UART1 接 SK32
board/dongle     Dongle 侧：USB 接 PC，恒禁睡，默认 2.4G
```

两侧角色差异（影响默认模式与睡眠策略）：

| 项 | 键盘侧（keyboard） | Dongle 侧（dongle） |
| --- | --- | --- |
| `BRIDGE_BOARD_IS_DONGLE` | 0 | 1 |
| 默认模式 `bridge_mode_default()` | `MODE_BLE` (1) | `MODE_2G4` (3) |
| 自动深睡 | 允许（AUTO 策略、空闲 ≥30s） | **禁止**（`#if !BRIDGE_BOARD_IS_DONGLE`） |
| USB 供电检测 | 有 VBUS 逻辑 | 恒在线 |

主频固定 `SetSysClock(CLK_SOURCE_PLL_60MHz)` = **60 MHz**（BLE 库 RF 校准依赖，硬件上限 80MHz 也不超频）。

---

## 2. 物理层

| 项 | 值 | 说明 |
| --- | --- | --- |
| 链路 | **UART1** | 桥 ↔ 外挂主控 |
| 引脚 | `PA8 = RXD1`、`PA9 = TXD1` | 见 [`ch582f_pinmap.md`](ch582f_pinmap.md) D2 |
| 波特率 | **921600**（`BRIDGE_UART_BAUD`） | 8 数据位 / 无校验 / 1 停止位（8N1） |
| 流控 | 无 | 靠协议层 5ms 间隔 + 队列过载保护 |
| 日志口 | UART0（PB4/PB7） | 与协议口分离，日志不影响协议帧 |

> 921600 8N1 → 单字节 ≈ 10.9µs，8 字节帧 ≈ 87µs，72 字节最大帧 ≈ 781µs。

---

## 3. 帧格式

### 3.1 通用帧结构

```
偏移  字段      长度   含义
0     SOF0      1      0xAA
1     SOF1      1      0x55
2     LEN       1      PAYLOAD 字节数（0..64）
3     CMD       1      命令/事件号
4     SEQ       1      序列号（回环，请求方自增）
5     FLAGS     1      帧标志位
6     PAYLOAD   LEN    载荷
6+LEN CRC16     2      LE 小端，覆盖范围见下
```

- 开销 `BRIDGE_FRAME_OVERHEAD = 8`（2 SOF + 4 头 + 2 CRC）。
- 单帧最大 `8 + 64 = 72` 字节。
- `LEN` 超出 `BRIDGE_MAX_PAYLOAD(64)` 的帧在接收端被**丢弃**。

### 3.2 CRC-16/CCITT-FALSE

| 参数 | 值 |
| --- | --- |
| 算法 | CRC-16/CCITT-FALSE |
| Init | `0xFFFF` |
| Poly | `0x1021` |
| RefIn / RefOut | false |
| XorOut | `0x0000` |
| 覆盖范围 | 从 `out[BRIDGE_SOF_LEN]`（即 `LEN` 字节）起 `BRIDGE_HDR_LEN + LEN` 字节，即 **LEN、CMD、SEQ、FLAGS、PAYLOAD**，不含 SOF |

即 CRC 覆盖「去掉 2 字节 SOF、去掉 2 字节 CRC」的中间全部内容，先 `LEN` 后 `CMD/SEQ/FLAGS/PAYLOAD`。

### 3.3 接收状态机容错

`ST_SOF0 → ST_SOF1 → ST_HDR → ST_PAYLOAD → ST_CRC`：

- 兼容 `AA AA 55`（第二个 `0xAA` 被视作噪声，重新进入 `ST_SOF1`）。
- `LEN > 64` 直接丢弃当前帧并复位。
- 半帧超时 `BRIDGE_RX_HALF_FRAME_MS = 5ms`：5ms 内未收齐则复位接收状态。
- CRC 连续错误计数 `BRIDGE_CRC_ERR_MAX = 10`（超阈值触发错误处理/统计）。

---

## 4. 时序与流量约束

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `BRIDGE_FRAME_MIN_GAP_MS` | 5 | 两帧发送最小间隔；不足返回 `GAP` |
| `BRIDGE_PING_PERIOD_MS` | 1000 | 心跳 PING 周期（桥单向发送） |
| `BRIDGE_LINK_LOST_CNT` | 3 | 连续 3 个周期未收到对端**任意合法帧**（含 PONG 应答）→ 判定链路失联 |
| `BRIDGE_ACK_TIMEOUT_MS` | 300 | 需 ACK 帧的超时 |
| `BRIDGE_ACK_MAX_RETRY` | 3 | 需 ACK 帧的最大重传次数 |
| `PENDING_Q_DEPTH` | 8 | TX 待发队列深度 |
| `BRIDGE_RX_HALF_FRAME_MS` | 5 | 半帧超时 |

### 4.1 TX 阻塞与排队

发送路径 `port_uart_tx_ready()` / `port_uart_tx_busy()`：

- 距上帧 < 5ms → 返回 `BRIDGE_GAP(-2)`。
- UART 仍在发送 → 返回 `BRIDGE_BUSY(-1)`。

`proto_tx()` 遇到 `GAP`/`BUSY` 时**不丢弃**，入 8 深待发队列后由 `bridge_proto_poll()` 重试；队列满（`PENDING_Q_DEPTH=8`）时**丢最旧**并累加 `s_q_drop` 计数。因此突发高密度上报时最早的一帧可能被牺牲，属预期行为。

### 4.2 需 ACK 帧的重传

`bridge_proto_poll()` 对「带 `FLAG_ACK_REQ` 且要求应答」的帧（典型为 `EVT_STATE`）做超时重传：每 300ms 重发，最多 3 次；3 次仍无对端 ACK → 触发 `bridge_evt_error(ECODE_ACK_TIMEOUT)`。

---

## 5. FLAGS 与应答模型

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `BRIDGE_FLAG_ACK_REQ` | `0x01` | 请求对端回 ACK |
| `BRIDGE_FLAG_ACK` | `0x02` | 本帧是对端请求的应答 |
| `BRIDGE_FLAG_ERR` | `0x04` | 本帧为错误应答（回带原 CMD） |

两类命令路径：

1. **控制路径（同步应答）**：请求方设 `FLAG_ACK_REQ=0`，桥处理后回一帧 `FLAG_ACK`（`bridge_proto_reply()`），payload 即结果数据；失败则回 `FLAG_ERR`（`bridge_proto_error()`），payload 为 1 字节错误码，且 **CMD 字段回带原命令号**。
2. **快速路径（无应答）**：HID 报表（`0x10~0x15`）与透传（`0x18/0x19`）**不回 ACK**，只按结果静默处理（失败发 `EVT_ERROR` 或直接丢弃），以降低延迟。

> 约定：事件帧由桥主动下发，使用 `seq=0` + `FLAG_ACK_REQ=1`（要求对端回 ACK）；`EVT_STATE` 额外带 300ms×3 重传。

---

## 6. 返回码与错误码

### 6.1 内部返回码（`bridge_ret_t`，`FLAG_ERR` payload / 内部判定）

| 名称 | 值 | 含义 |
| --- | --- | --- |
| `BRIDGE_OK` | 0 | 成功 |
| `BRIDGE_BUSY` | -1 | UART 忙，稍后重试 |
| `BRIDGE_GAP` | -2 | 距上帧不足 5ms |
| `BRIDGE_DUP` | -3 | 重复帧，已丢弃 |
| `BRIDGE_PARAM` | -4 | 参数非法 |
| `BRIDGE_STATE` | -5 | 当前状态不允许 |
| `BRIDGE_VBUS` | -6 | USB 供电状态冲突 |
| `BRIDGE_UNSUPPORTED` | -7 | 不支持 |
| `BRIDGE_IO` | -8 | 底层 IO 失败 |
| `BRIDGE_CRC` | -9 | CRC 校验失败 |

### 6.2 EVT_ERROR 错误码（`0x84` payload[0]）

| 名称 | 值 | 触发场景 |
| --- | --- | --- |
| `ECODE_CRC` | `0x01` | CRC 校验失败 |
| `ECODE_UART_LOST` | `0x02` | UART 链路失联（连续未收到对端任意合法帧） |
| `ECODE_RADIO` | `0x03` | 射频错误 |
| `ECODE_PAIR` | `0x04` | 配对被拒/失败（`PAIR_FAILED`） |
| `ECODE_ACK_TIMEOUT` | `0x05` | ACK 帧 300ms×3 重传耗尽 |
| `ECODE_PARAM` | `0x06` | 参数非法 |
| `ECODE_STATE` | `0x07` | 状态冲突（如 VBUS 冲突转为 STATE） |
| `ECODE_ROLLOVER` | `0x08` | HID 按键超 6 键（6KRO 溢出） |

---

## 7. 枚举总表

### 7.1 模式 `bridge_mode_t`

| 名称 | 值 | 说明 | 是否可持久化 |
| --- | --- | --- | --- |
| `MODE_USB` | 0 | USB 直连（VBUS 供电时） | 是（有 VBUS 时） |
| `MODE_BLE` | 1 | BLE 已连接/常规 | 是 |
| `MODE_BLE_PAIR` | 2 | BLE 配对窗口 | 否（不落盘） |
| `MODE_2G4` | 3 | 2.4G 常规 | 是 |
| `MODE_2G4_PAIR` | 4 | 2.4G 配对窗口 | 否 |
| `MODE_OTA` | 5 | 固件升级中 | 否 |
| `MODE_SLEEP` | 6 | 休眠 | 否 |

> `SET_MODE` 不接受 `MODE_BLE_PAIR(2)` / `MODE_2G4_PAIR(4)`（直接回 `PARAM`）；配对应使用 `PAIR_START (0x05)`。

### 7.2 射频 `bridge_radio_t`

| 名称 | 值 |
| --- | --- |
| `BRIDGE_RADIO_NONE` | 0 |
| `BRIDGE_RADIO_BLE` | 1 |
| `BRIDGE_RADIO_2G4` | 2 |

### 7.3 无线综合状态 `BRIDGE_WL_*`（`GET_WIRELESS_STATE 0x0E` 应答）

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `WL_USB` | USB 模式 |
| 1 | `WL_IDLE` | 空闲（无线未激活） |
| 2 | `WL_BLE_PAIRING` | BLE 配对中 |
| 3 | `WL_2G4_PAIRING` | 2.4G 配对中 |
| 4 | `WL_BLE_LINKED` | BLE 已连接 |
| 5 | `WL_2G4_LINKED` | 2.4G 已连接 |
| 6 | `WL_BLE_NOLINK` | BLE 无连接（重连中） |
| 7 | `WL_2G4_NOLINK` | 2.4G 无连接 |
| 8 | `WL_OTA` | OTA 中 |
| 9 | `WL_SLEEP` | 休眠 |

### 7.4 主机电源 `BRIDGE_HOST_*`

| 名称 | 值 |
| --- | --- |
| `BRIDGE_HOST_AWAKE` | 0 |
| `BRIDGE_HOST_SUSPEND` | 1 |

### 7.5 睡眠

| 名称 | 值 | 说明 |
| --- | --- | --- |
| `BRIDGE_SLEEP_WAKE` | 0 | 唤醒/保持清醒（撤销睡眠许可） |
| `BRIDGE_SLEEP_IDLE` | 1 | 允许浅睡空闲（缺省） |
| `BRIDGE_SLEEP_DEEP` | 2 | 允许深睡 |
| `BRIDGE_SLEEP_POLICY_AUTO` | 0 | 自动策略（缺省，恒许可） |
| `BRIDGE_SLEEP_POLICY_MANUAL` | 1 | 手动策略（看命令许可） |

### 7.6 电池来源 `bridge_batt_src_t`

| 名称 | 值 | 说明 |
| --- | --- | --- |
| `BRIDGE_BATT_SRC_AUTO` | 0 | 自动（缺省） |
| `BRIDGE_BATT_SRC_LOCAL` | 1 | 本地 ADC |
| `BRIDGE_BATT_SRC_EXTERNAL` | 2 | 外部上报（`SET_BATTERY` 优先） |

### 7.7 名称目标 `BRIDGE_NAME_TARGET_*`

| 名称 | 值 | 说明 |
| --- | --- | --- |
| `BRIDGE_NAME_TARGET_BLE` | 0 | BLE 广播名（≤21 字节） |
| `BRIDGE_NAME_TARGET_USB` | 1 | USB 设备名（≤21 字节） |

### 7.8 HID 报表长度

| 命令 | 类 | 长度（字节） |
| --- | --- | --- |
| `0x10` | Keyboard | 8 |
| `0x11` | NKRO | 16 |
| `0x12` | Consumer | 2 |
| `0x13` | Mouse | 4 |
| `0x14` | System | 1 |
| `0x15` | Release | 0 |

NKRO 相关：`BRIDGE_NKRO_USAGE_BASE=4`、`BRIDGE_NKRO_MAX_KEYS=6`（降级 6KRO 上限）、`BRIDGE_HID_ROLLOVER_ERROR=0x01`、`BRIDGE_BOOT_USAGE_MAX=0x65`。

### 7.9 透传上限

| 命令 | 上限 |
| --- | --- |
| `0x18` TUNNEL_USB | `BRIDGE_TUNNEL_USB_LEN = 64` |
| `0x19` TUNNEL_BLE | `BRIDGE_TUNNEL_BLE_LEN = 32` |

> 市售模组指令映射（兼容参考）：`0x88/0x23→TUNNEL_USB`、`0x89/0x24→TUNNEL_BLE`、`0x87→SET_BATTERY`、`0x4A/4B/4C→SLEEP`、`0x4D→GET_ADC`、`0x8A/8B→NV_WRITE/READ`。

---

## 8. 命令总表

### 8.1 上行命令（主机 → 桥）

| CMD | 名称 | 路径 | 应答 |
| --- | --- | --- | --- |
| `0x01` | GET_VERSION | 控制 | ACK |
| `0x02` | GET_STATUS | 控制 | ACK |
| `0x03` | SET_MODE | 控制 | ACK |
| `0x04` | SET_PROFILE | 控制 | ACK |
| `0x05` | PAIR_START | 控制 | ACK |
| `0x06` | UNPAIR | 控制 | ACK |
| `0x07` | RESET | 控制 | 无（复位） |
| `0x08` | ENTER_BOOT | 控制 | ACK 后跳 bootloader |
| `0x09` | SET_CONFIG | 控制 | ACK |
| `0x0A` | GET_MAC | 控制 | ACK |
| `0x0B` | SET_TEST_MODE | 控制 | ACK |
| `0x0C` | GET_MODE | 控制 | ACK |
| `0x0D` | SET_HEARTBEAT | 控制 | ACK |
| `0x0E` | GET_WIRELESS_STATE | 控制 | ACK |
| `0x0F` | GET_BLE_SLOTS | 控制 | ACK |
| `0x10` | HID_KEYBOARD | 快速 | 无 |
| `0x11` | HID_NKRO | 快速 | 无 |
| `0x12` | HID_CONSUMER | 快速 | 无 |
| `0x13` | HID_MOUSE | 快速 | 无 |
| `0x14` | HID_SYSTEM | 快速 | 无 |
| `0x15` | HID_RELEASE | 快速 | 无 |
| `0x18` | TUNNEL_USB | 快速 | 无 |
| `0x19` | TUNNEL_BLE | 快速 | 无 |
| `0x1A` | NV_WRITE | 控制 | ACK |
| `0x1B` | NV_READ | 控制 | ACK |
| `0x1C` | SET_BATTERY | 控制 | ACK |
| `0x1D` | SLEEP | 控制 | ACK |
| `0x1E` | GET_ADC | 控制 | ACK |
| `0x1F` | SET_NAME | 控制 | ACK |
| `0x20` | SET_USB_ID | 控制 | ACK |
| `0x21` | FW_BEGIN | 控制 | ACK |
| `0x22` | FW_DATA | 控制 | ACK |
| `0x23` | FW_END | 控制 | ACK |
| `0x24` | FW_ABORT | 控制 | ACK |
| `0x2A` | GET_DIAG | 控制 | ACK |
| `0x7F` | PING | 控制 | ACK 或 EVT_PONG |

> 未定义命令 → `bridge_proto_error(ECODE_PARAM)`（`FLAG_ERR` + CMD 回带 + payload `0x06`）。

### 8.2 下行事件（桥 → 主机）

| CMD | 名称 | 触发 |
| --- | --- | --- |
| `0x80` | EVT_STATE | 模式/连接/配置变化 |
| `0x81` | EVT_LEDS | 主机 LED 状态 |
| `0x82` | EVT_BATTERY | 电量/电压变化 |
| `0x83` | EVT_PAIR_RESULT | 配对结果 |
| `0x84` | EVT_ERROR | 各类错误 |
| `0x85` | EVT_TUNNEL_USB | USB 透传上行 |
| `0x86` | EVT_TUNNEL_BLE | BLE 透传上行 |
| `0x87` | EVT_HOST_POWER | 主机电源状态变化 |
| `0x7F` | EVT_PONG | 心跳应答 |

---

## 9. 逐命令详解

> 示例帧中 `SEQ` 为示意值；`CRC` 为真实值，可直接回放。所有 hex 均为「空格分隔、大写」。

### 9.1 `0x01` GET_VERSION — 获取固件版本

- **功能**：读取固件版本号。
- **现象/时序**：同步应答，无副作用。
- **请求 payload**：无（`LEN=0`）。
- **应答 payload（3B）**：`[major][minor][patch]`，当前 `BRIDGE_FW_VERSION="0.1.0"` → `00 01 00`。
- **错误码**：无。

```
请求 : AA 55 00 01 01 00 C1 80
应答 : AA 55 03 01 01 02 00 01 00 E4 18
                          └ major=0 minor=1 patch=0
```

---

### 9.2 `0x02` GET_STATUS — 获取综合状态

- **功能**：一次读取模式、连接、配置档、电量、睡眠策略。
- **现象/时序**：同步应答，无副作用。
- **请求 payload**：无。
- **应答 payload（5B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `mode` | `bridge_mode_t`：0=USB，1=BLE，2=BLE_PAIR，3=2G4，4=2G4_PAIR，5=OTA，6=SLEEP |
| 1 | `connected` | 1=无线已连接，0=未连接 |
| 2 | `profile` | 当前设备槽 1..4 |
| 3 | `battery%` | 电量百分比 0..100 |
| 4 | `sleep_policy` | 0=AUTO（自动），1=MANUAL（手动） |

- **错误码**：无。

```
请求 : AA 55 00 02 02 00 C2 8C
应答 : AA 55 05 02 02 02 01 01 01 42 00 C0 8B
                          └ mode=BLE connected=1 profile=1 batt=0x42(66%) policy=0
```

---

### 9.3 `0x03` SET_MODE — 设置工作模式

- **功能**：切换运行模式（USB / BLE / 2.4G / OTA / SLEEP）。
- **现象/时序**：经 `bridge_mode_request()` 完整状态迁移，成功后会：
  1. 释放全部 HID；
  2. 关闭当前射频；
  3. 按目标模式初始化射频（配对窗口→`pair_start`；BLE 常规→`connect_profile`）；
  4. 更新模式并在允许时**落盘** `last_mode`；
  5. 更新 LINK/WAKE GPIO；
  6. 主动下发 `EVT_STATE`。
- **请求 payload（1~2B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `mode` | 目标模式，见下表 |
| 1（可选） | `persist` | 0=不落盘，1=落盘；**缺省 1** |

`mode` 取值明细（`bridge_mode_t`）：

| 值 | 名称 | 含义 | 是否可用 |
| --- | --- | --- | --- |
| `0x00` | `MODE_USB` | USB 直连模式 | 可用（无 VBUS 时允许切换但不落盘） |
| `0x01` | `MODE_BLE` | BLE 常规（已连接/待连） | 可用 |
| `0x02` | `MODE_BLE_PAIR` | BLE 配对窗口 | **不可用** → `PARAM`（须改用 `0x05` PAIR_START） |
| `0x03` | `MODE_2G4` | 2.4G 常规 | 可用 |
| `0x04` | `MODE_2G4_PAIR` | 2.4G 配对窗口 | **不可用** → `PARAM`（须改用 `0x05` PAIR_START） |
| `0x05` | `MODE_OTA` | 固件升级中 | 可用（不落盘） |
| `0x06` | `MODE_SLEEP` | 休眠 | 可用（不落盘） |
| `>0x06` | — | 越界 | → `PARAM` |

- **边界与约束**：
  - `mode > MODE_SLEEP` → `PARAM`。
  - `MODE_BLE_PAIR(2)` / `MODE_2G4_PAIR(4)` → `PARAM`（须改用 `PAIR_START`）。
  - 有 VBUS 且 `mode != MODE_USB` → `BRIDGE_VBUS`，对外报 `ECODE_STATE`。
  - 无 VBUS 且 `mode == MODE_USB`：允许切换但**不落盘**。
  - 目标为 `SLEEP` / `OTA` 时**不落盘**。
  - 与当前相同且处于配对窗口：刷新配对窗口并重新计时。
  - 其他失败 → `ECODE_PARAM`。
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。
- **错误码**：`PARAM` / `STATE`（VBUS 冲突）。

```
请求 BLE,persist=1 : AA 55 02 03 05 00 01 01 D7 F4
应答成功           : AA 55 00 03 05 02 27 02
请求非法模式       : AA 55 01 03 06 04 06 23 3E   (FLAG=0x04 ERR, payload=0x06 PARAM)
```

---

### 9.4 `0x04` SET_PROFILE — 设置多设备配置档

- **功能**：在多设备（1..4）之间切换。
- **现象/时序**：成功后 `bridge_hid_release_all()`；若为 BLE 则 `port_radio_connect_profile(profile)`；先回 ACK，再下发 `EVT_STATE`。
- **请求 payload（0~1B）**：
  - 无 payload：使用**缺省 profile = 1**。
  - 1B：`profile` 取值明细：

| 值 | 含义 |
| --- | --- |
| `0x00` | `PARAM`（无 profile 0） |
| `0x01` | 设备槽 1 |
| `0x02` | 设备槽 2 |
| `0x03` | 设备槽 3 |
| `0x04` | 设备槽 4 |
| `>0x04` | `PARAM`（`BRIDGE_MAX_BLE_PROFILE=4`） |

- **边界**：`profile < 1 || profile > 4` → `PARAM`。
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。
- **错误码**：`PARAM`。

```
请求 profile=3 : AA 55 01 04 07 00 03 5F C4
应答成功       : AA 55 00 04 07 02 D5 E1
```

---

### 9.5 `0x05` PAIR_START — 启动配对

- **功能**：进入配对窗口，按当前射频方向配对。
- **现象/时序**：目标是 `MODE_BLE_PAIR` 或 `MODE_2G4_PAIR`，随后 `bridge_mode_request()` 走状态迁移；配对窗口有 60s 超时（`BRIDGE_PAIR_TIMEOUT_S`）。超时后 `port_radio_pair_stop` + `EVT_PAIR_RESULT(fail)` + 回落（BLE_PAIR→BLE / 2G4_PAIR→2G4）。配对成功由射频事件 `PAIRED` 触发 `EVT_PAIR_RESULT(ok)`。
- **请求 payload**：无。
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。
- **错误码**：有 VBUS → `ECODE_STATE`；其他失败 → `ECODE_PAIR`。

```
请求 : AA 55 00 05 08 00 99 E6
应答 : AA 55 00 05 08 02 DB C6
```

---

### 9.6 `0x06` UNPAIR — 解除绑定

- **功能**：删除已配对设备记录。
- **现象/时序**：按 `proto_radio_intent()` 选定射频（非无线模式退回 `cfg->last_mode`，2.4G 优先，否则 BLE）；先回 ACK，再下发 `EVT_STATE`。
- **请求 payload（0~1B）**：
  - 无 payload：**缺省 0xFF**，清全部 profile。
  - 1B：`profile` 取值明细：

| 值 | 含义 |
| --- | --- |
| `0x01`..`0x04` | 仅清除指定设备槽的绑定 |
| `0xFF` | 清除全部绑定（profile 1..4） |
| 其他 | 按指定 profile 处理（非 0xFF 且越界时清不到有效槽） |
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。
- **错误码**：无专用；失败按通用处理。

```
请求 清全部 : AA 55 01 06 09 00 FF A5 1C
应答        : AA 55 00 06 09 02 BA AC
```

---

### 9.7 `0x07` RESET — 软复位

- **功能**：MCU 软复位。
- **现象/时序**：直接调用 `port_reset()`，**不复位前应答**，设备随即重启。
- **请求 payload**：无。
- **应答**：无。

```
请求 : AA 55 00 07 0B 00 AA DD
```

---

### 9.8 `0x08` ENTER_BOOT — 进入 Bootloader

- **功能**：跳转到 bootloader（用于 IAP/OTA 之外的量产烧录）。
- **现象/时序**：先回 ACK；随后 4 轮 `poll() + port_uart_tx_flush() + delay(6ms)`（合计约 60ms）确保 ACK 发出；再 `bridge_hid_release_all()`；最后 `port_enter_bootloader()`。
- **请求 payload**：无。
- **应答**：`FLAG_ACK`（`LEN=0`），随后设备进入 bootloader。

```
请求 : AA 55 00 08 0C 00 0C 68
应答 : AA 55 00 08 0C 02 4E 48
```

---

### 9.9 `0x09` SET_CONFIG — 写配置项

- **功能**：按键值设置配置（见第 11 节）。
- **现象/时序**：`bridge_cfg_apply(key, value, len)` 处理并落盘。
- **请求 payload（2B+）**：

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `key` | 配置键，见下表 |
| 1.. | `value` | 依 key 而定，长度须匹配 |

`key` 取值明细：

| key | 名称 | value 长度 | value 取值含义 |
| --- | --- | --- | --- |
| `0x01` | REPORT_PERIOD | 2 | 2.4G 上报周期，LE16（ms）；`0` 或 `>100` → `PARAM`；缺省 1 |
| `0x02` | LOG_LEVEL | 1 | 日志等级枚举；`> DEBUG` → `PARAM`；`BRIDGE_LOG_ENABLE=0` 时恒 `UNSUPPORTED` |
| `0x03` | ADV_NAME_MAC | 1 | 广播名是否附 MAC：`0`=否，`1`=是；缺省 1 |
| `0x04` | CONN_INTERVAL | 1 | BLE 连接间隔，直接存（`6` ≈ 7.5ms）；缺省 6 |
| `0x05` | BATTERY_SRC | 1 | `0`=AUTO，`1`=LOCAL，`2`=EXTERNAL；缺省 0 |
| `0x06` | SLEEP_POLICY | 1 | `0`=AUTO，`1`=MANUAL；缺省 0 |
| 其他 | — | — | → `UNSUPPORTED`（对外报 `PARAM`） |

- **边界**：长度不匹配 / key 不识别 / 值非法 → `PARAM`。
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。

```
key=1 REPORT_PERIOD=8ms : AA 55 03 09 0D 00 01 08 00 4D FE
key=4 CONN_INTERVAL=6   : AA 55 02 09 0E 00 04 06 74 23
key=5 BATTERY_SRC=LOCAL : AA 55 02 09 0F 00 05 01 16 16
key=6 SLEEP_POLICY=MAN  : AA 55 02 09 10 00 06 01 0C 8C
应答                    : AA 55 00 09 0D 02 4F 4C
```

---

### 9.10 `0x0A` GET_MAC — 读取本机 MAC

- **功能**：读取 6 字节蓝牙 MAC。
- **现象/时序**：同步应答。
- **请求 payload**：无。
- **应答 payload（6B）**：`[b0..b5]` MAC 地址（LE 排列）。
- **错误码**：底层读取失败 → `STATE`。

```
请求 : AA 55 00 0A 12 00 10 26
应答 : AA 55 06 0A 12 02 11 22 33 44 55 66 8E 37
```

---

### 9.11 `0x0B` SET_TEST_MODE — SRRC 射频测试

- **功能**：进入/退出定频发射测试（SRRC 认证用）。
- **现象/时序**：进入前 `bridge_hid_release_all()`；测试期间射频锁定在指定信道/功率持续发射（BLE 链路层 `LL_SingleChannel`）。退出时 `LL_TestEnd` 并把共享射频归还当前在用射频（2.4G 侧会自动重建，避免接收通路失效）。
- **请求 payload（0~3B）**：

| 偏移 | 字段 | 取值 / 含义 | 缺省 |
| --- | --- | --- | --- |
| 0 | `mode` | `0`=退出定频（未激活时幂等返回 OK）；`1`=进入定频 | 0 |
| 1 | `channel` | BLE 信道 `0..39`（0=2402MHz，步进 2MHz，39=2480MHz） | 0 |
| 2 | `power` | 发射功率档位：`0`=0dBm，`1`=3dBm，`2`=4dBm，`3`=6dBm | 0 |

- **边界**：`mode` 非 0/1 → `PARAM`；`channel > 39` → `PARAM`；`power > 3` → `PARAM`。
- **编译开关**：`BRIDGE_FEATURE_SRRC_TEST` 未开启时恒 `UNSUPPORTED`（本工程已置 1）。
- **应答**：成功 `FLAG_ACK`；失败 `PARAM`；底层 IO/未初始化 → 对外映射 `PARAM`。

```
进入 ch19 pwr2 : AA 55 03 0B 13 00 01 13 02 34 41   (mode=1 ch=0x13=19 pwr=2)
退出           : AA 55 01 0B 14 00 00 E1 3A         (mode=0)
```

---

### 9.12 `0x0C` GET_MODE — 读取模式

- **功能**：读取当前模式、连接、射频。
- **应答 payload（3B）**：`[mode][connected][radio]`。

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `mode` | `bridge_mode_t`：0=USB，1=BLE，2=BLE_PAIR，3=2G4，4=2G4_PAIR，5=OTA，6=SLEEP |
| 1 | `connected` | 1=无线已连接，0=未连接 |
| 2 | `radio` | `bridge_radio_t`：0=NONE，1=BLE，2=2G4 |

```
请求 : AA 55 00 0C 03 00 F2 A4
应答 : AA 55 03 0C 03 02 01 01 01 35 35
                          └ mode=BLE(1) connected=1 radio=BLE(1)
```

---

### 9.13 `0x0D` SET_HEARTBEAT — 开关心跳

- **功能**：启用/关闭桥侧 PING 心跳（单向：桥发 PING，对端回 PONG）。
- **现象/时序**：开启时桥每 1s 发 PING，并以「收到对端任意合法帧（含 PONG）」作为在线判据；关闭后桥不发 PING、`bridge_uart_link_check()` 恒返回「在线」，即不再做失联判定；收到 PING 时若心跳关，则**直接返回 OK 而不回 PONG**。
- **请求 payload（1B）**：`[enable]`。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `enable` | `0`=关闭心跳，`1`=开启心跳 |

- **边界**：`len<1` 或 `payload[0] > 1` → `PARAM`。
- **应答**：成功 `FLAG_ACK`。

```
关闭 : AA 55 01 0D 15 00 00 48 2A
开启 : AA 55 01 0D 16 00 01 39 63
```

---

### 9.14 `0x0E` GET_WIRELESS_STATE — 读取无线综合状态

- **功能**：读取 `BRIDGE_WL_*` 综合状态。
- **应答 payload（1B）**：`[state]`，取值明细：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x00` | `WL_USB` | USB 模式 |
| `0x01` | `WL_IDLE` | 空闲（无线未激活） |
| `0x02` | `WL_BLE_PAIRING` | BLE 配对中 |
| `0x03` | `WL_2G4_PAIRING` | 2.4G 配对中 |
| `0x04` | `WL_BLE_LINKED` | BLE 已连接 |
| `0x05` | `WL_2G4_LINKED` | 2.4G 已连接 |
| `0x06` | `WL_BLE_NOLINK` | BLE 无连接（重连中） |
| `0x07` | `WL_2G4_NOLINK` | 2.4G 无连接 |
| `0x08` | `WL_OTA` | OTA 中 |
| `0x09` | `WL_SLEEP` | 休眠 |

```
请求 : AA 55 00 0E 04 00 05 53
应答 : AA 55 01 0E 04 02 04 21 E3   (state=4 BLE_LINKED)
```

---

### 9.15 `0x0F` GET_BLE_SLOTS — 读取 BLE 多主机槽绑定状态

- **功能**：查询 4 组 BLE 主机 Profile 的绑定占用情况，并返回当前活跃（最近使用）的 Profile，供上位机展示槽位、引导配对或切换主机。
- **请求 payload**：无（`len` 必须为 0）。
- **应答 payload（5B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `s1` | Profile 1 是否已配对：`0`=空 / `1`=已配对 |
| 1 | `s2` | Profile 2 同上 |
| 2 | `s3` | Profile 3 同上 |
| 3 | `s4` | Profile 4 同上 |
| 4 | `last_active` | 最近活跃（当前使用）的 Profile：`1..4`，`0`=无 |

- **现象/时序**：同步取 `bridge_mh_has(1..4)` 与 `bridge_mh_active()`，即时回 `FLAG_ACK`，不切换模式、不触发配对。
- **边界**：仅**键盘侧**支持。Dongle 恒为 2.4G 接收器、不承载 BLE 主机绑定 → 回 `FLAG_ERR`，错误码 `STATE(0x07)`。
- **错误码**：Dongle 侧 → `STATE`；其余无专用（失败按通用处理）。
- **备注**：绑定数据本身由 SDK 闭源 GAPBondMgr 保存在 BLE SNV 区（`BLE_SNV_NUM=4`，库原生记忆 4 个主机绑定）；应用侧仅在**建立连接（标记该槽已配对）**时更新对应位与 `last_active`；该 Profile 被 `UNPAIR(0x06)` 解除后 `s` 归 `0`，若它正是活跃槽则 `last_active` 一并归 `0`（全空表则为 `0`）。

```
请求 : AA 55 00 0F 47 00 AA 3C
应答 : AA 55 05 0F 47 02 01 00 01 00 01 D4 74   (s1=1 s2=0 s3=1 s4=0 last_active=1)
应答 : AA 55 05 0F 47 02 00 00 00 00 00 94 F9   (4 槽全空、无活跃)
```

---

### 9.16 `0x10` HID_KEYBOARD — 键盘报表

- **功能**：转发 8 字节 Boot 键盘报表。
- **现象/时序**：**快速路径，无 ACK**。与上一帧相同 → `DUP` 静默丢弃；射频未连接 → `STATE`；底层发送失败 → `IO`。若 `BRIDGE_FEATURE_NKRO_BLE=0` 且非 2.4G，NKRO 命令会**自动降级**为 6KRO（详见 `0x11`）。
- **payload（必须 8B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `modifier` | 修饰键位图（见下） |
| 1 | `reserved` | 保留，固定 0 |
| 2..7 | `k1..k6` | 6 个 HID usage（按键码），0=无键 |

`modifier` 位图：bit0=LCtrl、bit1=LShift、bit2=LAlt、bit3=LGui、bit4=RCtrl、bit5=RShift、bit6=RAlt、bit7=RGui。

- **边界**：长度 ≠ 8 → `PARAM`。

```
Shift+A : AA 55 08 10 17 00 02 00 04 00 00 00 00 00 07 CB
全释放  : AA 55 08 10 18 00 00 00 00 00 00 00 00 00 B1 14
```

---

### 9.17 `0x11` HID_NKRO — NKRO 报表

- **功能**：转发 16 字节 NKRO 位图报表。
- **现象/时序**：**快速路径，无 ACK**。位图约定：`byte0=修饰键`，从 `byte1` 的 bit0 起为 usage，`usage = 4 + (byte-1)*8 + bit`。若固件未开启 BLE NKRO（`BRIDGE_FEATURE_NKRO_BLE=0`）且当前模式非 `MODE_2G4`，自动降级为 8 字节 6KRO：修饰键沿用 `byte0`，超过 `BRIDGE_BOOT_USAGE_MAX(0x65)` 的 usage 丢弃，超过 6 键则置 `boot8[1]=0x01`（Rollover 错误）并发 `ECODE_ROLLOVER`。
- **payload（必须 16B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `modifier` | 修饰键位图（bit0=LCtrl … bit7=RGui，同 `0x10`） |
| 1..15 | `bitmap` | usage 位图：`usage = BRIDGE_NKRO_USAGE_BASE(4) + (byte-1)*8 + bit`，置 1 表示该 usage 按下 |

- **边界**：长度 ≠ 16 → `PARAM`。

```
全零 : AA 55 10 11 19 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 08 80
```

---

### 9.18 `0x12` HID_CONSUMER — 多媒体键报表

- **功能**：转发 2 字节 Consumer 报表。
- **payload（必须 2B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `usage_lo` | Consumer usage 低字节（LE16） |
| 1 | `usage_hi` | Consumer usage 高字节 |

`usage` 为 16 位 Consumer Page 键码（如 `0x00E9`=音量+、`0x00EA`=音量-、`0x00E2`=静音、`0x00CD`=播放/暂停）；`0x0000`=无键。长度 ≠ 2 → `PARAM`。快速路径无 ACK。

```
全零 : AA 55 02 12 1A 00 00 00 85 B6
```

---

### 9.19 `0x13` HID_MOUSE — 鼠标报表

- **功能**：转发 4 字节鼠标报表。
- **payload（必须 4B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `buttons` | 按键位图：bit0=左键，bit1=右键，bit2=中键，bit3..7=扩展键 |
| 1 | `dx` | X 位移，有符号（-128..127） |
| 2 | `dy` | Y 位移，有符号（-128..127） |
| 3 | `wheel` | 滚轮，有符号（-128..127） |

长度 ≠ 4 → `PARAM`。快速路径无 ACK。

```
全零 : AA 55 04 13 1B 00 00 00 00 00 8C 08
```

---

### 9.20 `0x14` HID_SYSTEM — 系统键报表

- **功能**：转发 1 字节 System 报表。
- **payload（必须 1B）**：`[usage]`，8 位 System Control usage（如 `0x01`=电源、`0x02`=睡眠、`0x03`=唤醒；`0x00`=无键）。长度 ≠ 1 → `PARAM`。快速路径无 ACK。

```
全零 : AA 55 01 14 1C 00 00 09 5C
```

---

### 9.21 `0x15` HID_RELEASE — 释放全部 HID

- **功能**：释放全部已上报的 HID 按键（含消费键/鼠标/系统键）。
- **现象/时序**：`bridge_hid_release_all()`：先清本地缓存；若未连接只清 mask 返回 OK；已连接则对每个 active 类逐类发送全零帧。
- **payload**：无（`LEN=0`）。快速路径无 ACK。

```
请求 : AA 55 00 15 1D 00 7C 59
```

---

### 9.22 `0x18` TUNNEL_USB — USB 透传

- **功能**：向 USB 侧透传原始报文（HID 透传/自定义）。
- **现象/时序**：**快速路径，无 ACK**。交给 `port_tunnel_send`；`len > BRIDGE_TUNNEL_USB_LEN(64)` → `PARAM`；未连接或发送失败静默丢弃。
- **payload（0..64B）**：原始报文字节，协议层不解释内容。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0..`len-1` | `data` | 原样透传的报文字节，长度上限 `BRIDGE_TUNNEL_USB_LEN = 64` |

```
01 02 03 : AA 55 03 18 1E 00 01 02 03 49 94
```

---

### 9.23 `0x19` TUNNEL_BLE — BLE 透传

- **功能**：向 BLE 侧透传原始报文。
- **现象/时序**：**快速路径，无 ACK**。`len > BRIDGE_TUNNEL_BLE_LEN(32)` → `PARAM`；未连接或发送失败静默丢弃。
- **payload（0..32B）**：原始报文字节，协议层不解释内容。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0..`len-1` | `data` | 原样透传的报文字节，长度上限 `BRIDGE_TUNNEL_BLE_LEN = 32` |

```
10 20 : AA 55 02 19 1F 00 10 20 2E C1
```

---

### 9.24 `0x1A` NV_WRITE — 写 NV 用户区

- **功能**：写 32 字节用户 NV 单元。
- **现象/时序**：`len` 必须为 **33**（1 字节地址 + 32 字节数据），且 `payload[0] < 46`（`BRIDGE_NV_USER_UNITS`）。实现为**读改写整槽**：
  - 槽号 `nv_unit_slot(addr) = BRIDGE_NV_USER_BASE(25) + addr/2`；
  - 槽内偏移 `nv_unit_off(addr) = (addr % 2) * 32`。
- **请求 payload（33B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `addr` | 用户 NV 单元号 `0x00..0x2D`（0..45）；`≥46` → `PARAM` |
| 1..32 | `data` | 32 字节数据，整单元写入 |

- **边界**：`len != 33` 或 `addr >= 46` → `PARAM`。
- **应答**：成功 `FLAG_ACK`（`LEN=0`）。

```
请求 addr0 : AA 55 21 1A 20 00 00 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
            10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 57 73
应答       : AA 55 00 1A 20 02 C6 26
```

---

### 9.25 `0x1B` NV_READ — 读 NV 用户区

- **功能**：读回 32 字节用户 NV 单元。
- **请求 payload（1B）**：`[addr]`，用户 NV 单元号 `0x00..0x3D`（0..61）；`≥62` → `PARAM`。
- **应答 payload（33B）**：`[addr][data0..data31]`（回带请求地址 + 32 字节数据）。
- **边界**：`len < 1` 或 `addr` 越界 → `PARAM`；底层读失败 → `STATE`。

```
请求 addr0 : AA 55 01 1B 21 00 00 13 0F
应答       : AA 55 21 1B 21 02 00 00 01 02 ... 1F 48 15   (33B: addr + 32B)
```

---

### 9.26 `0x1C` SET_BATTERY — 外部电量上报

- **功能**：由外部（如键盘主控）上报电量百分比。
- **现象/时序**：`bridge_power_set_external(percent)`；仅当 `battery_src=EXTERNAL` 时优先于本地 ADC；先回 ACK，再下发 `EVT_STATE`。低电告警：`LOW`（≤20%）、`CRIT`（≤5%）。
- **请求 payload（1B）**：`[percent]` 取值明细：

| 值 | 含义 |
| --- | --- |
| `0x00`..`0x64` | 电量百分比 0..100% |
| `>0x64` | 截断为 100% |

- **应答**：`FLAG_ACK`；`len < 1` → `PARAM`。

```
上报 66% : AA 55 01 1C 22 00 42 E8 6F
应答     : AA 55 00 1C 22 02 04 F2
```

---

### 9.27 `0x1D` SLEEP — 睡眠控制

- **功能**：控制睡眠许可与唤醒。
- **现象/时序**：
  - `WAKE(0)`：`bridge_mode_sleep_permitted(permit=false)`，并请求切回 `last_mode`（若上次为 `SLEEP`/`OTA` 则改 `BLE`）。
  - `IDLE(1)`：仅 `permit(true)`（浅睡，保持链路）。
  - `DEEP(2)`：`permit(true)` 并请求 `MODE_SLEEP`。
- **请求 payload（0~1B）**：`[level]`（缺省 `IDLE`），取值明细：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x00` | `BRIDGE_SLEEP_WAKE` | 唤醒/保持清醒（解除睡眠许可，切回 last_mode） |
| `0x01` | `BRIDGE_SLEEP_IDLE` | 允许浅睡（缺省） |
| `0x02` | `BRIDGE_SLEEP_DEEP` | 允许深睡（进 MODE_SLEEP） |
| `>0x02` | — | → `PARAM` |

- **应答**：`FLAG_ACK`。

```
IDLE : AA 55 01 1D 23 00 01 CB 56
DEEP : AA 55 01 1D 24 00 02 38 E3
WAKE : AA 55 01 1D 25 00 00 4A F4
```

---

### 9.28 `0x1E` GET_ADC — 读取电池电压

- **功能**：读取电池 ADC 电压（毫伏）。
- **应答 payload（2B）**：`[mv_lo][mv_hi]`（LE，单位 mV）。

```
请求      : AA 55 00 1E 26 00 E2 70
应答 3900mV: AA 55 02 1E 26 02 3C 0F 02 81   (0x0F3C = 3900)
```

---

### 9.29 `0x1F` SET_NAME — 设置设备名

- **功能**：设置 BLE 广播名 / USB 设备名。
- **现象/时序**：`port_set_name`；若返回 `UNSUPPORTED`（如 Dongle 侧）则经 2.4G **中继**到对端（`relay_cfg`）。失败 → `STATE`。
- **请求 payload（2..22B）**：`[target][name...]`。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `target` | 名称目标 `BRIDGE_NAME_TARGET_*`，见下表 |
| 1.. | `name` | UTF-8/ASCII 名称，字节长度 `len-1` 须 `≤ BRIDGE_NAME_MAX(21)`（无 NUL 结尾，仅按长度写入） |

`target` 取值明细：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x00` | `BRIDGE_NAME_TARGET_BLE` | BLE 广播名（≤21 字节） |
| `0x01` | `BRIDGE_NAME_TARGET_USB` | USB 设备名（≤21 字节） |
| 其他 | — | → `PARAM` |

- **边界**：`len<2`、`target∉{0,1}`、`len-1>21` → `PARAM`；`bridge_cfg_set_name` 失败（落盘 IO 错）→ `STATE`。
- **应答**：`FLAG_ACK`。

```
BLE "SK32"          : AA 55 05 1F 27 00 00 53 4B 33 32 16 B9
USB "SK32 Wireless" : AA 55 0E 1F 28 00 01 53 4B 33 32 20 57 69 72 65 6C 65 73 73 F1 75
```

---

### 9.30 `0x20` SET_USB_ID — 设置 USB VID/PID

- **功能**：设置 USB 设备 VID/PID。
- **请求 payload（必须 4B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `vid_lo` | VID 低字节；`vid = p0 \| (p1<<8)` |
| 1 | `vid_hi` | VID 高字节 |
| 2 | `pid_lo` | PID 低字节；`pid = p2 \| (p3<<8)` |
| 3 | `pid_hi` | PID 高字节 |

缺省 `VID=0x0194 / PID=0x0463`；两值均为 LE16。本地不支持时（如 Dongle 侧）经 2.4G 中继到对端。
- **边界**：`len != 4` → `PARAM`；`vid==0 || pid==0` → `PARAM`（`bridge_cfg_set_usb_ids`）。
- **应答**：`FLAG_ACK`。

```
VID=0x0194 PID=0x0463 : AA 55 04 20 29 00 94 01 63 04 EC 08
```

---

### 9.31 `0x21` FW_BEGIN — 开始固件升级

- **功能**：开启 OTA 会话。
- **现象/时序**：成功后请求 `MODE_OTA`。
- **请求 payload（必须 6B）**：`[total_u32_LE][crc16_LE]`。

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0..3 | `total` | 固件总长度（u32 LE），非 0、≤ `BRIDGE_FW_MAX_LEN(216KB)`、4 字节对齐 |
| 4..5 | `crc_exp` | 预期整包 CRC16 |

- **边界**：`len != 6` → `PARAM`；`total` 为 0 / > 216KB / 非 4 对齐 → `PARAM`；重复 begin 会先 `abort`；`port_fw_begin` 失败 → `ECODE_STATE`。
- **应答 payload（1B）**：`[chunk_max]`（= `BRIDGE_FW_CHUNK_MAX(60)`）。
- **错误码**：`PARAM`（参数）/ `STATE`（底层开启失败）。

```
请求 total=65536 crc=0xABCD : AA 55 06 21 2A 00 00 00 01 00 CD AB 70 26
应答 chunk=60               : AA 55 01 21 2A 02 3C 1D 2A
```

---

### 9.32 `0x22` FW_DATA — 固件数据块

- **功能**：按偏移顺序写入固件分块。
- **请求 payload（5..64B）**：`[off_u32_LE][data...]`。

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0..3 | `off` | 数据块在整包中的偏移（u32 LE） |
| 4.. | `data` | `dlen = len-4`，须 `∈(0, 60]`（`BRIDGE_FW_CHUNK_MAX`）且 4 字节对齐 |

- **幂等**：若 `off + dlen <= written`（完全已写范围）→ 直接 `OK`，**不重复累计 CRC**；`off != written` → `PARAM`；超过声明总长 → `PARAM`。
- **边界**：`len < 5` → `PARAM`。
- **应答**：`FLAG_ACK`（`LEN=0`）。
- **错误码**：参数类 → `ECODE_PARAM`；CRC 失配 → `ECODE_CRC`；其他 → `ECODE_STATE`。

```
off=0 dlen=8 : AA 55 0C 22 2B 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 97 D0
应答         : AA 55 00 22 2B 02 38 96
```

---

### 9.33 `0x23` FW_END — 结束升级并校验

- **功能**：结束会话并校验整包 CRC。
- **现象/时序**：先回 ACK，再 4 轮 `poll()+port_uart_tx_flush()+delay(6ms)`（约 24ms）确保发出；随后 `bridge_ota_apply()`（跳转新固件，**不复返**）。
- **请求 payload**：无（`len` 必须为 0）。
- **边界**：`written != total` → `STATE`；`crc_run != crc_exp` → `CRC`。
- **应答**：`FLAG_ACK`（校验通过后跳转）。

```
请求 : AA 55 00 23 2C 00 DD 18
应答 : AA 55 00 23 2C 02 9F 38
```

---

### 9.34 `0x24` FW_ABORT — 中止升级

- **功能**：中止当前 OTA 会话。
- **现象/时序**：`abort` 后回 ACK，并切回默认模式。
- **请求 payload**：无。
- **应答**：`FLAG_ACK`。

```
请求 : AA 55 00 24 2D 00 7C AE
```

---

### 9.35 `0x2A` GET_DIAG — 读取诊断计数

- **功能**：读取 2.4G 射频诊断统计（24B）。
- **应答 payload（24B）**：

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0 | `up` | 射频已起：`0`=否，`1`=是 |
| 1 | `conn` | 已连接：`0`=否，`1`=是 |
| 2 | `bound` | 已绑定：`0`=否，`1`=是 |
| 3 | `tx_busy` | 发送忙：`0`=否，`1`=是 |
| 4 | `tx_q_cnt` | 发送队列当前深度 |
| 5 | `hb_miss` | 心跳丢失计数 |
| 6..7 | `report_period` | 上报周期（LE16） |
| 8..9 | `tx_submit_ok` | 提交成功 |
| 10..11 | `tx_submit_fail` | 提交失败 |
| 12..13 | `tx_done_ok` | 完成成功 |
| 14..15 | `tx_done_fail` | 完成失败 |
| 16..17 | `tx_drop` | 发送丢弃 |
| 18..19 | `rx_any` | 收到任意包 |
| 20..21 | `rx_pair` | 收到配对包 |
| 22..23 | `rx_data_bad` | 收到坏数据包 |

- **备注**：`[6..23]` 均为 LE16。

```
请求 : AA 55 00 2A 2E 00 2E E0
应答 : AA 55 18 2A 2E 02 00 00 ... 00 36 53   (24B 全 0)
```

---

### 9.36 `0x7F` PING — 心跳

- **功能**：链路心跳保活 / 对端探测。
- **现象/时序**（**单向心跳**）：桥每 `BRIDGE_PING_PERIOD_MS(1000)` 主动发 PING；对端收到后回 `EVT_PONG`（`FLAG_ACK`，`seq` 回环相同值）即可，**无需主动发 PING**。桥收到对端**任意合法完整帧**（含 PONG 应答）即刷新链路活性；连续 `BRIDGE_LINK_LOST_CNT(3)` 个周期未收到才判失联。心跳关闭（`SET_HEARTBEAT(0x0D)` 置 0）时 → 直接 `OK` 但**不回 PONG**，且不做失联判定。
- **请求 payload**：无。

```
请求     : AA 55 00 7F 2F 00 21 66
EVT_PONG : AA 55 00 7F 2F 02 63 46
```

---

## 10. 逐事件详解

事件帧统一 `seq=0` + `FLAG_ACK_REQ=1`。除 `EVT_STATE` 外其余为单次发送。

### 10.1 `0x80` EVT_STATE — 状态变化（需 ACK，带重传）

- **触发**：模式切换完成、连接/断开、profile 切换、电量变化、名称/USB ID 应用等。
- **payload（4B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `mode` | `bridge_mode_t`：0=USB，1=BLE，2=BLE_PAIR，3=2G4，4=2G4_PAIR，5=OTA，6=SLEEP |
| 1 | `connected` | 1=无线已连接，0=未连接 |
| 2 | `profile` | 当前设备槽 1..4 |
| 3 | `battery%` | 0..100 |

- **时序**：带 ACK 追踪，`BRIDGE_ACK_TIMEOUT_MS(300) × BRIDGE_ACK_MAX_RETRY(3)`；耗尽 → `EVT_ERROR(ECODE_ACK_TIMEOUT)`。

```
AA 55 04 80 00 01 01 01 01 42 C8 25
        └ mode=BLE connected=1 profile=1 batt=0x42(66%)
```

### 10.2 `0x81` EVT_LEDS — 主机 LED 状态

- **触发**：来自无线侧（PC）的键盘 LED 状态（Caps/Num/Scroll…）。
- **现象/时序**：先 `bridge_hid_set_leds` 更新本地，再上报。
- **payload（1B）**：LED 位图：bit0=Num Lock，bit1=Caps Lock，bit2=Scroll Lock，bit3=Compose，bit4=Kana，bit5..7 保留。

```
AA 55 01 81 00 01 03 83 13   (leds=0x03 → Num+Caps)
```

### 10.3 `0x82` EVT_BATTERY — 电量上报

- **payload（3B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `mv_lo` | 电压低字节（mV，LE16 低位） |
| 1 | `mv_hi` | 电压高字节（mV，LE16 高位） |
| 2 | `percent` | 电量百分比 0..100 |

```
AA 55 03 82 00 01 3C 0F 42 E4 C8   (0x0F3C=3900mV, 0x42=66%)
```

### 10.4 `0x83` EVT_PAIR_RESULT — 配对结果

- **payload（2B）**：

| 偏移 | 字段 | 取值含义 |
| --- | --- | --- |
| 0 | `result` | `0`=配对成功，`1`=配对失败/超时 |
| 1 | `profile` | 设备槽 1..4 |

```
成功 p1 : AA 55 02 83 00 01 00 01 43 6E
失败 p1 : AA 55 02 83 00 01 01 01 72 5D
```

### 10.5 `0x84` EVT_ERROR — 错误上报

- **payload（1B）**：错误码，取值明细：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x01` | `ECODE_CRC` | CRC 校验失败 |
| `0x02` | `ECODE_UART_LOST` | UART 链路失联 |
| `0x03` | `ECODE_RADIO` | 射频错误 |
| `0x04` | `ECODE_PAIR` | 配对被拒/失败 |
| `0x05` | `ECODE_ACK_TIMEOUT` | ACK 重传耗尽 |
| `0x06` | `ECODE_PARAM` | 参数非法 |
| `0x07` | `ECODE_STATE` | 状态冲突 |
| `0x08` | `ECODE_ROLLOVER` | HID 按键超 6 键 |

```
PARAM       : AA 55 01 84 00 01 06 63 FF
ACK_TIMEOUT : AA 55 01 84 00 01 05 00 CF
```

### 10.6 `0x85` EVT_TUNNEL_USB — USB 透传上行

- **触发**：无线侧（USB 对端）有原始报文上行时，由 `bridge_evt_tunnel(BRIDGE_TUNNEL_USB, …)` 发出。
- **现象/时序**：空 payload 或 `len > BRIDGE_TUNNEL_USB_LEN(64)` → **静默丢弃，不发帧**。
- **payload（1..64B）**：原始报文字节，协议层不解释内容。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0..`len-1` | `data` | 上行原始报文字节，长度上限 `BRIDGE_TUNNEL_USB_LEN = 64` |

```
01 02 : AA 55 02 85 00 01 01 02 94 A0
```

### 10.7 `0x86` EVT_TUNNEL_BLE — BLE 透传上行

- **触发**：无线侧（BLE 对端）有原始报文上行时，由 `bridge_evt_tunnel(BRIDGE_TUNNEL_BLE, …)` 发出。
- **现象/时序**：空 payload 或 `len > BRIDGE_TUNNEL_BLE_LEN(32)` → **静默丢弃，不发帧**。
- **payload（1..32B）**：原始报文字节，协议层不解释内容。

| 偏移 | 字段 | 取值 / 含义 |
| --- | --- | --- |
| 0..`len-1` | `data` | 上行原始报文字节，长度上限 `BRIDGE_TUNNEL_BLE_LEN = 32` |

```
10 20 : AA 55 02 86 00 01 10 20 24 7A
```

### 10.8 `0x87` EVT_HOST_POWER — 主机电源状态

- **触发**：宿主 USB 挂起/唤醒变化（`bridge_mode_set_host_awake()`；awake 时同时 `note_activity()`）。
- **payload（1B）**：取值明细：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0x00` | `BRIDGE_HOST_AWAKE` | 主机唤醒/在线 |
| `0x01` | `BRIDGE_HOST_SUSPEND` | 主机挂起（睡眠许可恒放开） |

```
AWAKE   : AA 55 01 87 00 01 00 79 04
SUSPEND : AA 55 01 87 00 01 01 58 14
```

### 10.9 `0x7F` EVT_PONG — 心跳应答

- **现象/时序**：`FLAG_ACK`，`seq` 与收到的 PING 相同（回环）。

```
AA 55 00 7F 2F 02 63 46
```

---

## 11. SET_CONFIG 配置键表

`SET_CONFIG (0x09)` payload = `[key][value...]`。

| key | 名称 | value 长度 | 取值 / 含义 | 缺省 |
| --- | --- | --- | --- | --- |
| `0x01` | REPORT_PERIOD | 2 | 2.4G 上报周期（LE16，单位 ms）；`0` 或 `>100` → `PARAM` | 1 |
| `0x02` | LOG_LEVEL | 1 | 日志等级；`> DEBUG` → `PARAM`；`BRIDGE_LOG_ENABLE=0` 时恒 `UNSUPPORTED` | `BRIDGE_LOG_LEVEL` |
| `0x03` | ADV_NAME_MAC | 1 | 广播名是否附 MAC：0/1 | 1 |
| `0x04` | CONN_INTERVAL | 1 | 连接间隔（直接存，值 6 ≈ 7.5ms） | 6 |
| `0x05` | BATTERY_SRC | 1 | 0=AUTO / 1=LOCAL / 2=EXTERNAL | 0 (AUTO) |
| `0x06` | SLEEP_POLICY | 1 | 0=AUTO / 1=MANUAL | 0 (AUTO) |

其他关键 `bridge_cfg_t` 字段（由专用命令或默认写入）：

| 字段 | 缺省 | 相关命令 |
| --- | --- | --- |
| `last_mode` | `bridge_mode_default()` | SET_MODE |
| `ble_profile` | 1 | SET_PROFILE |
| `usb_vid / usb_pid` | `0x0194 / 0x0463` | SET_USB_ID |
| BLE 名 / USB 名 | `"SK32 Keyboard"` / `"SK32 Wireless"` | SET_NAME |

名称 NV blob（64B）布局：`[0]=ble_len [1]=usb_len [2..22]=ble [23..43]=usb`。名称未变化时幂等，不重复落盘。

---

## 12. 典型交互流程

### 12.1 上电握手与状态查询

```
主机 → GET_VERSION  (AA 55 00 01 seq 00 …)
桥   → ACK 00 01 00
主机 → GET_STATUS   (AA 55 00 02 seq 00 …)
桥   → ACK mode/connected/profile/batt/policy
主机 → GET_WIRELESS_STATE
桥   → ACK WL_*
```

### 12.2 进入 BLE

```
主机 → SET_MODE [MODE_BLE, persist=1]
桥   → ACK                     (状态迁移：release_all→radio_down→connect_profile→落盘→GPIO→evt_state)
桥   → EVT_STATE [mode=Ble connected=0/1 profile batt]   (需 ACK，最多 3 次重传)
```

### 12.3 配对（BLE / 2.4G）

```
主机 → PAIR_START
桥   → ACK
桥   → EVT_STATE (配对窗口状态)
   ├─ 成功：射频事件 PAIRED → EVT_PAIR_RESULT[0, profile] + 退出配对窗口
   └─ 60s 超时：pair_stop → EVT_PAIR_RESULT[1, profile] → 回落常规模式
```

### 12.4 解除绑定

```
主机 → UNPAIR [0xFF 清全部 / profile]
桥   → ACK
桥   → EVT_STATE
```

### 12.5 睡眠与唤醒

```
主机 → SET_CONFIG [0x06, 1]        (切 MANUAL 手动策略)
主机 → SLEEP  [DEEP]               (permit(true) + 请求 MODE_SLEEP)
   … 休眠 …
主机 → SLEEP  [WAKE]               (permit(false) + 切回 last_mode)
桥   → EVT_STATE
```

> 键盘侧：AUTO 策略且非配对窗口、未连接、空闲 ≥ `BRIDGE_IDLE_TO_SLEEP_S(30s)` 自动 `MODE_SLEEP`；有接收活动后保持清醒 `BRIDGE_RX_HOLD_AWAKE_MS(1500ms)`。Dongle 侧禁自动深睡。

### 12.6 OTA 升级

```
主机 → FW_BEGIN [total_u32 crc16]   → ACK [chunk_max=60]
主机 → FW_DATA  [off_u32 data…]     → ACK          (可重发已写区间，幂等)
…                                                   (off 必须严格递增衔接 written)
主机 → FW_END                        → ACK → ota_apply() 跳转新固件
   （异常：FW_ABORT → ACK → 回默认模式）
```

### 12.7 名称 / USB ID 中继

```
主机 → SET_NAME [target, name…]
   ├─ 本地支持：port_set_name
   └─ 本地 UNSUPPORTED（如 Dongle 侧）：relay_cfg 经 2.4G 中继到对端
桥   → ACK
主机 → SET_USB_ID [vid_lo vid_hi pid_lo pid_hi] → ACK
```

---

## 附：关键宏速查

| 宏 | 值 | 宏 | 值 |
| --- | --- | --- | --- |
| `BRIDGE_UART_BAUD` | 921600 | `BRIDGE_MAX_PAYLOAD` | 64 |
| `BRIDGE_FRAME_MIN_GAP_MS` | 5 | `BRIDGE_PING_PERIOD_MS` | 1000 |
| `BRIDGE_LINK_LOST_CNT` | 3 | `BRIDGE_IDLE_TO_SLEEP_S` | 30 |
| `BRIDGE_PAIR_TIMEOUT_S` | 60 | `BRIDGE_RX_HALF_FRAME_MS` | 5 |
| `BRIDGE_ACK_TIMEOUT_MS` | 300 | `BRIDGE_ACK_MAX_RETRY` | 3 |
| `BRIDGE_NAME_MAX` | 21 | `BRIDGE_NV_UNIT` | 32 |
| `BRIDGE_NV_SLOT_SIZE` | 64 | `BRIDGE_NV_UNITS_PER_SLOT` | 2 |
| `BRIDGE_NV_USER_BASE` | 25 | `BRIDGE_NV_SNV_LO` | 48 |
| `BRIDGE_NV_USER_UNITS` | 46 | `BRIDGE_FW_CHUNK_MAX` | 60 |
| `BRIDGE_FW_MAX_LEN` | 216*1024 | `BRIDGE_RX_HOLD_AWAKE_MS` | 1500 |
| `BRIDGE_POST_SLEEP_LISTEN_MS` | 40 | `BRIDGE_SHALLOW_SLEEP_MS` | 20 |
| `BRIDGE_WDOG_TIMEOUT_MS` | 400 | `BRIDGE_FW_VERSION` | "0.1.0" |
| `BRIDGE_FW_REV` | 0x0001 | `BRIDGE_CFG_MAGIC` | 0x334D ("3M") |
| `BRIDGE_CFG_VERSION` | 3 | `BRIDGE_TX_LATEST_ONLY` | 1 |
| `BRIDGE_HID_DEDUP` | 1 | `BRIDGE_RX_RING_SIZE` | 300 |
| `BRIDGE_TX_RING_SIZE` | 96 | `BRIDGE_MAX_BLE_PROFILE` | 4 |
| `BRIDGE_TEST_CH_MAX` | 39 | `BRIDGE_TEST_POWER_MAX` | 3 |
