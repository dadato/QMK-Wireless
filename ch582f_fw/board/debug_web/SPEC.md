# CH582F 键盘模组测试/调试 Web 应用 —— 规格文档

| 项目 | 内容 |
| --- | --- |
| 文档 | 键盘侧 CH582 测试调试 Web 应用规格（含通讯协议） |
| 版本 | v0.2 |
| 日期 | 2026-10-07 |
| 状态 | 待确认（Phase 1） |
| 适用固件 | `wireless/ch582f_fw`（keyboard 目标，CH582F） |
| 参考同类 | `board/ota_web`（OTA 升级，已完成）；`tools/sk32sim`（Python 命令行同源工具） |

---

## 1. 目标与范围

提供一个浏览器内运行的**纯静态 Web 应用**，把 PC 当作 SK32（键盘主控）角色，
经 **PC COM 口（USB-TTL 直连模组 UART1，921600 8N1）** 与**键盘侧 CH582 模组**通讯，
用于开发阶段的**功能测试与调试**：

- **发指令**：全命令表可视化操作（版本/状态/MAC/ADC、模式、配对、配置、NV、睡眠、透传、复位等）；
- **模拟键盘数据流**：构造并注入 HID 报文（键盘 6KRO / NKRO / 多媒体 / 鼠标 / 系统键），
  支持文本打字、宏/序列、压力与速率测试；
- **监视与诊断**：实时帧监视器、事件解码、链路健康度、统计计数、日志导出；
- **回路验证**：经 WebHID（Dongle，RID 0）与 WebHID（模组 BLE，HOGP RID 0 隐式）做 VIA 透传往返，验证
  「网页 → 蓝牙/2.4G → 模组 → UART → SK32」完整回路（§5.8）。

**不在范围内**：OTA 固件升级（见 `board/ota_web`）；SK32/QMK 侧固件改动；
量产烧录（`WCHISPTool`）；射频（2.4G/BLE）端的真实输入验证（本工具只覆盖命令/事件层，
射频输出需 PC/手机侧人工或外部捕获确认）。

> **例外**：§5.8「VIA 测试」Tab 通过 **WebHID**（接收器 Dongle / 键盘模组 BLE 两条通路）
> 对无线透传回路做 VIA 往返验证——它**不改任何固件**，但会触达 Dongle/模组/SK32 的既有透传路径。

> **关键前提**：模组 UART1（PA8=RXD / PA9=TXD）平时与 SK32 相连。
> 使用本工具时须**断开 SK32**，用 USB-TTL 直连模组 UART1（与 OTA 通路 B 同一接法）。

---

## 2. 术语

| 术语 | 含义 |
| --- | --- |
| 桥 / 模组 | 键盘侧 CH582F 无线桥固件（本工具的被测对象） |
| 上位机 / 宿主 | 本 Web 应用，扮演 SK32 角色 |
| 桥帧 | UART1 上的 `AA 55 ...` 协议帧 |
| HID 转发 | `0x10~0x15` 命令：把 HID 报文经射频（BLE/2.4G）送出，无 ACK |
| 快速路径 | 无应答命令（HID 0x10~0x15、TUNNEL 0x18/0x19、RESET） |
| 控制路径 | 有 ACK/ERR 应答的命令（GET_*/SET_*/PAIR/NV/SLEEP/ADC/NAME/USB_ID/PING/FW_*/ENTER_BOOT） |
| DUP 抑制 | 固件对与上次相同的 HID 报文返回 `BRIDGE_ERR_DUP` 并抑制发送（快速路径无 ACK，故**上位机不可直接观测**，仅体现为「射频侧无输出」） |
| VIA 隧道 / 裸透传 | 网页经 WebHID（Dongle / 模组 BLE 两条通路）与真实 SK32 之间的 **32B 裸载荷**透传通道（HID 报文体无 report ID、无 status 字节），对应固件 `TUNNEL_USB(0x18)`/`EVT_TUNNEL_USB(0x85)` 与 `TUNNEL_BLE(0x19)`/`EVT_TUNNEL_BLE(0x86)`（§5.8） |

---

## 3. 系统架构与数据流

```
PC 浏览器 (WebSerial)
   │  USB-TTL (921600 8N1)
   ▼
CH582F 模组 UART1 (PA8=RXD1 / PA9=TXD1)
   │
   ├── 控制路径：命令 → 固件执行 → ACK/ERR 应答（+ 可能的 EVT_*）
   ├── 快速路径：HID 0x10~0x15 / 透传 0x18/0x19 → 经射频送出（无 ACK）
   └── 事件上报：EVT_STATE / LEDS / BATTERY / PAIR_RESULT / ERROR / HOST_POWER / TUNNEL_*（带 ACK_REQ，需回应答）
```

本应用与固件的关系，等价于现有 `tools/sk32sim`（Python CLI）的浏览器版：
同一套帧层、同一套命令语义、同一套时序约束，差别只在 UI 与可视化。

---

## 4. 通讯协议（UART1 桥帧）

来源：[bridge_proto.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_proto.h#L9-L28)、
[bridge_uart.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_uart.c)、
[bridge_types.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_types.h)、
[proto.py](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/proto.py)。

### 4.1 物理层

| 项 | 值 |
| --- | --- |
| 波特率 | 921600（模组默认） |
| 数据/校验/停止 | 8 / N / 1 |
| 流控 | 无 |
| 引脚 | PA8 = RXD1，PA9 = TXD1 |

> **上位机波特率可选**：本 Web 应用连接区提供波特率下拉框（`19200 / 38400 / 43000 / 57600 / 76800 / 115200 / 128000 / 230400 / 460800 / 921600 / 1843200 / 3686400 / 7372800`），
> **默认 921600**。现网模组 UART1 默认 921600，选择其他波特率无法与标准固件通讯（仅用于适配/负向测试）；
> 连接成功后选择器锁定，断开后可改。

### 4.2 帧格式

```
+--------+-----+-----+-----+-------+---------+-----------+
| SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16     |
| AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B (LE)   |
+--------+-----+-----+-----+-------+---------+-----------+
```

- `LEN` = PAYLOAD 字节数，范围 `0..64`（`BRIDGE_MAX_PAYLOAD = 64`）；帧开销 8B，单帧最大 72B。
- `CRC16` 小端写入；**覆盖范围：从 `LEN` 起到 `PAYLOAD` 结束（不含 SOF）**。

### 4.3 CRC

- `CRC-16/CCITT-FALSE`：init `0xFFFF`、poly `0x1021`、不反转、xorout 0。
- 与 [proto.py `crc16_ccitt`](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/proto.py#L244-L247) 及 [ota_web/js/crc16.js](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/board/ota_web/js/crc16.js) 一致，可直接复用。

### 4.4 帧标志（FLAGS）

| 位 | 名称 | 值 | 含义 |
| --- | --- | --- | --- |
| 0 | `BRIDGE_FLAG_ACK_REQ` | `0x01` | 请求对端应答 |
| 1 | `BRIDGE_FLAG_ACK` | `0x02` | 本帧是对某请求的应答 |
| 2 | `BRIDGE_FLAG_ERR` | `0x04` | 错误帧（payload[0] = 错误码） |

### 4.5 时序约束（宿主侧必须遵守）

| 项 | 值 |
| --- | --- |
| 最小出帧间隔 | `max(5ms, 线上时间 + 0.5ms)`（8N1 → 10 bit/byte） |
| 心跳 PING | 桥 → 宿主，每 1000ms（**单向**：宿主只需回 `EVT_PONG`） |
| 收到桥 PING | 回 `EVT_PONG`（`flags=ACK`，seq 取原帧） |
| 收到带 `ACK_REQ` 的事件 | 自动回 ACK（`flags=ACK`，cmd/seq 取原帧） |
| 控制命令 ACK | 按 `(cmd, seq)` 匹配；默认超时 600ms、重试 1 次（幂等命令） |
| 顺序敏感命令 | `FW_DATA`(2s/2 次)、`FW_END`(1.5s/0 次) 由调用方显式指定 |

### 4.6 控制命令（有应答）

| CMD | 值 | 请求 PAYLOAD | 应答 PAYLOAD | 语义 |
| --- | --- | --- | --- | --- |
| `GET_VERSION` | `0x01` | — | `[major u8][minor u8][patch u8]` | 固件版本 |
| `GET_STATUS` | `0x02` | — | `[mode u8][connected u8][profile u8][battery u8][sleep_policy u8]` | 状态快照 |
| `SET_MODE` | `0x03` | `[mode u8][persist u8]` | — | 切模式（persist=1 落盘）；配对窗口 `2`/`4` 被拒 → `ERR(PARAM)`，改用 `PAIR_START` |
| `SET_PROFILE` | `0x04` | `[profile u8]` | — | 切 BLE 主机槽位 1..4 |
| `PAIR_START` | `0x05` | `[duration_s u16 LE]`（可选，缺省=60s） | — | 进入配对窗口（限时 `duration_s` 秒，超时自动停窗口并回 `EVT_PAIR_RESULT(1)`；`duration_s=0`=不自动关闭；payload 非 0/2 字节 → `ERR(PARAM)`） |
| `UNPAIR` | `0x06` | `[profile u8]`（`0xFF`=全清） | —（随后 `EVT_STATE`） | 解绑 |
| `ENTER_BOOT` | `0x08` | — | ACK（随后跳 bootloader） | 进 ROM ISP 量产烧录；先回 ACK，约 60ms 后释放 HID 并 `port_enter_bootloader()` |
| `SET_CONFIG` | `0x09` | `[key u8][value...]` | — | 写配置（见 4.8） |
| `GET_MAC` | `0x0A` | — | `[mac 6B]` | 本机 MAC |
| `SET_TEST_MODE` | `0x0B` | `[mode u8][channel u8][power u8]` | — | SRRC 定频（2.4G 返回 `UNSUPPORTED`） |
| `GET_MODE` | `0x0C` | — | `[mode u8][connected u8][radio u8]` | 轻量模式查询（仅模式三要素，供高频轮询） |
| `SET_HEARTBEAT` | `0x0D` | `[enable u8]`（1=开/0=关） | — | 桥侧心跳周期开关（单向：桥发 PING；关闭后不发 PING/不做失联判定/不回 PONG） |
| `GET_WIRELESS_STATE` | `0x0E` | — | `[wl_state u8]` | 无线综合状态（单枚举，免组合判断）：0=USB/1=空闲/2=BLE 配对中/3=2.4G 配对中/4=BLE 已连/5=2.4G 已连/6=BLE 无连接/7=2.4G 无连接/8=OTA/9=休眠 |
| `GET_BLE_SLOTS` | `0x0F` | — | `[s1 u8][s2 u8][s3 u8][s4 u8][last_active u8]` | BLE 多主机槽绑定状态：`s1..s4`=槽 1..4 是否已绑定（0=空/1=已绑定）；`last_active`=最近活跃槽 1..4，0=无。仅键盘模组支持，Dongle 侧 → `ERR(STATE)` |
| `NV_WRITE` | `0x1A` | `[addr u8][data 32B]` | — | 写用户 NV 单元 |
| `NV_READ` | `0x1B` | `[addr u8]` | `[addr u8][data 32B]` | 读用户 NV 单元 |
| `SET_BATTERY` | `0x1C` | `[percent u8]` | —（随后 `EVT_STATE`） | 注入电量 |
| `SLEEP` | `0x1D` | `[level u8]` | — | `0`=唤醒 / `1`=浅睡 / `2`=深睡 |
| `SLEEP_CFG` | `0x25` | `[idle_s u16 LE][deep_s u16 LE]` | — | 配置 AUTO 策略自动休眠门限（单位 s，落盘）：`idle_s`=距最近串口接收活动达该秒数后允许浅睡（0=禁用自动浅睡）；`deep_s`=未连接且空闲达该秒数后进深睡（0=禁用自动深睡）；`len≠4` → `ERR(PARAM)` |
| `BATTERY_CFG` | `0x26` | `[enable u8][on_change u8][period_s u8]` | — | 配置 `EVT_BATTERY` 的开关与发送机制（落盘）：`enable`=总开关（0=关/1=开）；`on_change`=电量变化时立即上报（0=关/1=开）；`period_s`=定时上报周期（单位 S，0=关闭定时）；`len≠3` → `ERR(PARAM)`；**`EVT_BATTERY` 仅在 `battery_src=LOCAL`（本地 ADC）时才会上报** |
| `GET_ADC` | `0x1E` | — | `[mv u16 LE]` | 采集电压 |
| `SET_NAME` | `0x1F` | `[target u8][name...]` | — | 改名（`target` 0=BLE 广播名 / 1=Dongle USB 名，经 2.4G 转发） |
| `SET_USB_ID` | `0x20` | `[vid u16 LE][pid u16 LE]` | — | 改写 USB 标识（Dongle 侧重枚举） |
| `GET_DIAG` | `0x2A` | — | `[0]up [1]conn [2]bound [3]tx_busy [4]tx_q_cnt [5]hb_miss [6..7]report_period [8..9]tx_submit_ok [10..11]tx_submit_fail [12..13]tx_done_ok [14..15]tx_done_fail [16..17]tx_drop [18..19]rx_any [20..21]rx_pair [22..23]rx_data_bad`（`[6..23]` 均 LE16） | 2.4G 射频诊断快照（24B，排障用） |
| `HID_DIAG` | `0x2B` | — | `[0]reg [1..2]hid_handle [3..4]map_handle [5..6]raw_in_handle [7..8]raw_out_handle [9]report_cnt [10]attr_cnt [11]reset_src [12]stage_prev [13]stage_now [14..17]notify_fail [18..21]notify_retry_ok [22..25]notify_retry_drop`（`[1..8]` 均 LE16；`[14..25]` 均 uint32 LE） | BLE HID 服务注册取证（26B，排障用）：`reg`=`GATTServApp_RegisterService` 返回值（`0`=成功/`0xFF`=未调用）；`raw_in/out_handle`=RID0 IN/OUT 属性 handle；`[14..25]` 为出站 notify 计数（`HidDev_Report` 失败 / 延迟重发成功 / 延迟重发仍失败）；仅键盘模组支持，Dongle 侧 → `ERR(STATE)` |
| `PING` | `0x7F` | — | `EVT_PONG`（`flags=ACK`） | 心跳/往返 |
| `FW_BEGIN/DATA/END/ABORT` | `0x21~0x24` | 见 [ota_web SPEC §4.6](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/board/ota_web/SPEC.md) | — | 仅保留「调试/引用」，升级流程由 `ota_web` 承担 |

### 4.7 快速路径命令（无应答）

| CMD | 值 | PAYLOAD | 语义 |
| --- | --- | --- | --- |
| `HID_KEYBOARD` | `0x10` | 8B：`[modifier][reserved=0][k0..k5]` | 6KRO 键盘报文 |
| `HID_NKRO` | `0x11` | 16B 位图（bit n → usage `n+4`） | NKRO 报文（BLE 侧降级 6KRO） |
| `HID_CONSUMER` | `0x12` | 2B | 多媒体键 |
| `HID_MOUSE` | `0x13` | 4B：`[buttons][dx][dy][wheel]` | 鼠标 |
| `HID_SYSTEM` | `0x14` | 1B | 系统键（电源等） |
| `HID_RELEASE` | `0x15` | — | 全键释放（清去重缓存） |
| `TUNNEL_USB` | `0x18` | ≤64B | 透传到 Dongle/USB（VIA 通道） |
| `TUNNEL_BLE` | `0x19` | ≤32B | 透传到 BLE（手机 APP 通道） |
| `RESET` | `0x07` | — | 立即复位（无应答） |

> 长度必须严格匹配；相同 HID 报文会被固件去重（`BRIDGE_ERR_DUP`），
> 因此「打字」须按 `按下 → 释放` 两帧交替发送（见 §5.5）。
> 注：`ENTER_BOOT(0x08)` 虽有「无后续交互」的表象，但会**先回 ACK 再跳 bootloader**，故归入控制路径（§4.6）。

### 4.8 SET_CONFIG 键

| key | 名称 | value | 说明 |
| --- | --- | --- | --- |
| `0x01` | `REPORT_PERIOD` | `u16 LE`（ms，缺省 1） | 2.4G 上报周期（当前只存值，不参与节流）；`0` 或 `>100` → `ERR(PARAM)` |
| `0x02` | `LOG_LEVEL` | `u8` 0..4 | 日志级别 |
| `0x03` | `ADV_NAME_MAC` | `u8` 0/1 | 广播名是否带 MAC 后缀 |
| `0x04` | `CONN_INTERVAL` | `u8`（×1.25ms） | BLE 连接间隔 |
| `0x05` | `BATTERY_SRC` | `u8` 0=auto/1=local/2=external | 电量来源 |
| `0x06` | `SLEEP_POLICY` | `u8` 0=AUTO/1=MANUAL | 休眠策略（AUTO=自动判闲休眠 / MANUAL=仅 `SLEEP` 指令） |

来源：[bridge_cfg.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_cfg.h#L14-L20)。

### 4.9 事件（桥 → 宿主，均带 `ACK_REQ`）

| EVT | 值 | PAYLOAD | 语义 |
| --- | --- | --- | --- |
| `EVT_STATE` | `0x80` | `[mode u8][connected u8][profile u8][battery u8]` | 状态变更 |
| `EVT_LEDS` | `0x81` | `[led_state u8]` | 主机 LED（Num/Caps/Scroll） |
| `EVT_BATTERY` | `0x82` | `[mv u16 LE][percent u8]` | 电量上报（仅 `battery_src=LOCAL` 时按 `BATTERY_CFG` 开关/变化/定时下发） |
| `EVT_PAIR_RESULT` | `0x83` | `[result u8][profile u8]` | 配对结果（`result` 0=成功 / 1=失败或配对窗口超时） |
| `EVT_ERROR` | `0x84` | `[code u8]` | 错误码（见 4.10） |
| `EVT_TUNNEL_USB` | `0x85` | ≤64B | USB 透传上报 |
| `EVT_TUNNEL_BLE` | `0x86` | ≤32B | BLE 透传上报 |
| `EVT_HOST_POWER` | `0x87` | `[state u8]` | 主机电源状态（`0`=运行/唤醒 `1`=挂起；USB 挂起/恢复 或 BLE HID Control Point） |
| `EVT_PONG` | `0x7F` | — | PING 应答 |

### 4.10 枚举与错误码

- **模式**：`0`=USB / `1`=BLE / `2`=BLE_PAIR / `3`=2G4 / `4`=2G4_PAIR / `5`=OTA / `6`=SLEEP（[bridge_mode.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_mode.h#L12-L20)）。`SET_MODE` 只接受 `0`/`1`/`3`/`5`/`6`，配对窗口 `2`/`4` 一律走 `PAIR_START`。
- **射频通道（`GET_MODE` 的 radio）**：`0`=NONE / `1`=BLE / `2`=2G4。
- **睡眠分级**：`0`=WAKE / `1`=IDLE / `2`=DEEP。
- **休眠策略（`SET_CONFIG` key `0x06`）**：`0`=AUTO / `1`=MANUAL。
- **主机电源（`EVT_HOST_POWER` / `BRIDGE_RADIO_PKT_HOST`）**：`0`=AWAKE（运行/唤醒）/ `1`=SUSPEND（挂起）。
- **名称目标**：`0`=BLE / `1`=USB。
- **EVT_ERROR / ERR 帧码**：`0x01` CRC / `0x02` UART_LOST / `0x03` RADIO / `0x04` PAIR / `0x05` ACK_TIMEOUT / `0x06` PARAM / `0x07` STATE / `0x08` ROLLOVER。
- **返回码**：`-1` BUSY / `-2` GAP / `-3` DUP / `-4` PARAM / `-5` STATE / `-6` VBUS / `-7` UNSUPPORTED / `-8` IO / `-9` CRC。
- **用户 NV**：46 个单元 × 32B（单元号 0..45；越界 → `ERR(PARAM)`）。

### 4.11 报文示例

```
GET_STATUS  请求：AA 55 00 02 05 00                      CRC16
            应答：AA 55 04 02 05 02  03 01 02 3C          CRC16
                  LEN=4 CMD ISSUE FLAGS  mode link prof batt
```

---

## 5. Web 应用功能规格

### 5.1 布局（单页、Tab 切换，共用连接区与日志区）

```
┌──────────────────────────────────────────────────────────────┐
│ [选择串口] COM7 波特率[921600 ▾] 8N1  ●链路正常  版本 0.1.0  [断开] │
├──────────────────────────────────────────────────────────────┤
│ [状态] | [监视器] | [命令台] | [HID 模拟] | [场景] | [VIA 测试] | [日志] │
│  （当前 Tab 内容）                                             │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 连接区（WebSerial）

- 「选择串口」→ `navigator.serial.requestPort()` → 按连接区所选波特率打开（**默认 921600 8N1**，
  可选 19200/38400/43000/57600/76800/115200/128000/230400/460800/921600/1843200/3686400/7372800）；连接后选择器锁定，断开后可改。
- 打开后自动：发 `GET_VERSION` + `GET_STATUS` 快照；**默认不主动发心跳 PING**（单向：桥每 1s 发 PING，本端回 `EVT_PONG` 即保活）。
- 链路健康指示：以「最近收到桥 PING 的时间」判定（窗口 3500ms）。
- 「断开」：停止心跳、关闭端口。

### 5.3 Tab｜状态（仪表盘）

- 版本、MAC、ADC(mV)、模式、连接态、BLE 槽位、电量、LED 状态。
- 统计计数：TX / RX / CRC 错误 / 超时 / ERR / 事件 / PONG（心跳应答）。
  > 不设 DUP 计数：HID 去重发生在快速路径（无 ACK/ERR），上位机无法观测。
- 「刷新」按钮 + 事件驱动自动更新（`EVT_STATE`/`EVT_BATTERY`/`EVT_LEDS`）。

### 5.4 Tab｜监视器

- 实时帧列表（最新在下）：时间戳(ms)、方向（`→` TX / `←` RX）、类型（TX/RX/ACK/ERR/EVT）、CMD 名、seq、flags、payload hex、解码摘要。
- 支持：暂停/继续、清空、按类型与关键字过滤、导出（TXT/JSON）、点击行展开 loader 帧级详情。
- CRC 错误行以醒目色标出（含原始字节）。

### 5.5 Tab｜命令台

- 按组呈现全命令表（§4.6 / §4.7）的可视化表单：
  - **基础**：GET_VERSION / GET_STATUS / GET_MODE / GET_WIRELESS_STATE / GET_BLE_SLOTS / GET_MAC / GET_ADC / GET_DIAG / HID_DIAG / PING
  - **模式与配对**：SET_MODE(下拉，不含配对窗口) / SET_PROFILE / PAIR_START / UNPAIR
  - **配置**：SET_CONFIG 六个键（含便捷项 REPORT_PERIOD / LOG_LEVEL / ADV_NAME_MAC / CONN_INTERVAL / BATTERY_SRC / SLEEP_POLICY）+ SET_HEARTBEAT
  - **电源与其他**：SET_BATTERY / SLEEP / NV_READ / NV_WRITE(hex) / SET_NAME / SET_USB_ID / SET_TEST_MODE
  - **透传**：TUNNEL_USB(hex) / TUNNEL_BLE(hex)
  - **HID 直发**：见 §5.6
  - **危险操作**（二次确认）：RESET / ENTER_BOOT / UNPAIR(0xFF)
- **本机心跳开关**（命令台顶部勾选项「停发本机心跳」，**默认勾选**）：控制 Web 侧是否每 1000ms 主动发 `PING`
  （`link.setLocalHeartbeat(false/true)`）。单向机制下默认停发：桥每 1s 发 PING，本端回 `EVT_PONG` 即保活，无需本端主动发心跳。
  仅影响本端发心跳，**不改变桥侧 `SET_HEARTBEAT(0x0D)` 状态**（桥侧心跳需另发 `SET_HEARTBEAT`）。
- **原始帧编辑器**：任意 `CMD/SEQ/FLAGS/PAYLOAD(hex)` 组帧发送（用于非常规/负向测试），
  可选择是否等待应答（按 (cmd,seq) 匹配）。
- 每条命令回显：组出的原始帧 hex、应答帧 hex、解码结果或错误码名。

### 5.6 Tab｜HID 数据流模拟

四个子模式，共用「发送速率 / 重复次数 / 是否等链路就绪」参数：

1. **文本打字**：输入文本 → ASCII→HID usage 映射（镜像 [cli.py `_CHAR_MAP`](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/cli.py#L29-L50)，
   含 Shift 组合）→ 按「按下→释放」两帧交替发送；可选字符/秒速率；不支持的字符高亮跳过。
2. **HID 报文构造器**：
   - 键盘：修饰键多选 + 最多 6 个按键（usage 选择器/直接输入）；
   - NKRO：16B 位图勾选；
   - 多媒体 / 系统键 / 鼠标：对应字段表单；
   - 一键 `HID_RELEASE`。
3. **宏 / 序列**：行式脚本（如 `tap 0x04 20ms` / `down 0xE1` / `up 0xE1` / `delay 100ms` / `release`），
   按顺序执行，支持循环次数。
4. **压力测试**：指定按键与目标速率，持续 N 秒或 N 次，统计实际 keys/s、
   发送帧数、收到的 ERR（尤其 `ROLLOVER`/`RADIO`）。

> 说明：去重约束下，同一「按住」状态重复上报会被抑制；压力测试用 `按下→释放` 交替或改变键值规避。

### 5.7 Tab｜场景用例（镜像 `scenarios.py`）

可在 UI 一键运行、逐项显示 PASS/FAIL 与明细，并可「全部运行（非破坏性）」：

| 场景 | 覆盖 |
| --- | --- |
| `smoke` | 版本/状态/MAC/ADC/PING + HID 五类 + 透传 + 越界参数 → ERR(PARAM) |
| `nv` | 32B 单元写-读回（0/1/30/61）+ 越界 ERR(PARAM) |
| `config` | SET_CONFIG 五项（跑完恢复默认）+ 未知键 ERR(PARAM) |
| `battery` | SET_BATTERY(66) → EVT_STATE + GET_STATUS 一致 |
| `sleep` | IDLE/DEEP/WAKE 分级行为 |
| `pair_2g4` | UNPAIR→SET_MODE(2G4)→PAIR_START→EVT_PAIR_RESULT→EVT_STATE（需 Dongle） |
| `typing_2g4` | 2.4G 打字流（需已配对；PC 端人工确认字符） |
| `ble` | 切 BLE + 广播名（打字需 PC/手机蓝牙配对后验证） |
| `srrc` | BLE 定频开/关 + 信道越界 ERR(PARAM) |
| `ota` | 整包升级（转发到 `ota_web` 或内嵌最小实现；见 §7 决策） |
| `unpair_all` | 清除全部绑定（破坏性，默认不跑） |

### 5.8 Tab｜VIA 测试（网页 → 蓝牙/2.4G → 模组 → UART → SK32 回路）

用于验证**完整无线路由回路**：网页作为主机侧，经 **WebHID**（2.4G 接收器 Dongle，Report ID 0）
或 **WebHID**（键盘侧模组 BLE 的 HOGP vendor 集合，Report ID 5）把 VIA 报文送入模组，
再由模组经 UART 转发给 **真实 SK32**（QMK/VIA），并原路收回。

> **与 §5.5 的区别**：§5.5 的 `TUNNEL_USB(0x18)`/`TUNNEL_BLE(0x19)` 是**经串口直连模组、由本工具扮演 SK32**
> 的透传命令；本 Tab 则**不占用串口**，由**真实 SK32** 承担 VIA 应答方，网页扮演主机侧，覆盖端到端的无线回路。

**被测链路（两条通路）**

| 通路 | 下行 | 上行 |
| --- | --- | --- |
| A｜WebHID → Dongle → 2.4G | 网页写 Dongle IF2 `EP3 OUT`(32B, RID 0) → Dongle → 2.4G → 键盘模组 → `UART EVT_TUNNEL_USB(0x85)` → SK32 `raw_hid_receive` | SK32 `raw_hid_send` → UART `CMD_TUNNEL_USB(0x18)` → 模组 → 2.4G → Dongle `EP3 IN`(恒 32B) |
| B｜WebHID → 模组 BLE | 网页写 HOGP vendor `OUT`(32B, RID 0 隐式, Usage Page `0xFF60`/Usage `0x61`) → 模组 `sdk_ble_on_raw_report` → `UART EVT_TUNNEL_BLE(0x86)` → SK32 | SK32 `raw_hid_send` → UART `CMD_TUNNEL_BLE(0x19)` → 模组 → HOGP vendor `IN`(Notify, RID 0 隐式) → 网页 |

**报文约定（关键，与 `ota_web` 的 OTA 报文语义不同）**

- 全程 **32 字节裸透传**：HID 报文体**无 report ID 前缀、无 status 字节**，`payload[0]` 即 VIA 命令 ID。
  （report ID 只体现在 HID 传输层；WebHID 的 `sendReport`/`inputreport` 已将其与载荷分离。）
- 两条通路 Report ID 语义不同，须按各自编号过滤：
  - 通路 A（Dongle 复合 HID，IF2 无编号）：`reportId===0 && byteLength===32`。
  - 通路 B（键盘模组 BLE 复合 HID，vendor 报表）：`reportId===5 && byteLength===32`。
- **两条通路的 HID 设备须按 VID/PID 区分选择**：本机同时存在**两个** `usagePage 0xFF60` / `usage 0x61` 的 HID 集合
  （Dongle 与键盘模组 BLE HOGP），仅凭 `usagePage` 过滤会让选择框列出同名两者、极易选错（选错后写/读静默失败，
  表现为「连接成功却超时」）。故两条通路的 `requestDevice()` 各自叠加 VID/PID 精确筛选：
  - 通路 A（Dongle）：`{ usagePage: 0xFF60, vendorId: 0x0194, productId: 0x0463 }`（`core/bridge_config.h` `BRIDGE_USB_VID/PID`）。
  - 通路 B（BLE HOGP）：`{ usagePage: 0xFF60, vendorId: 0x07D7, productId: 0x0000 }`（SDK `devinfoservice.c` 的 `devInfoPnpId` 默认值，本工程不可配）。
  - 取设备时**优先复用本 origin 已授权设备**（`navigator.hid.getDevices()` 命中即用，免弹框）；未命中才弹选择框，且报错文案给出本通路应有的 VID/PID。
- Dongle 下行首次 3 字节若为 `"OTA"`（`0x4F 0x54 0x41`）会被 OTA 分支拦截/丢弃 → 自检报文首字节取 `0x01` 天然避开。
- 通路 B 前置条件：键盘须处于 **BLE 模式** 且在**系统蓝牙中已配对**（Windows 会枚举出 HOGP HID 设备，
  WebHID 才能按 `usagePage 0xFF60` 叠加 VID `0x07D7` / PID `0x0000` 命中其 vendor 集合）。

**自检报文与判定**

- 发送 32B：`[0x01, 0x00, 0x0D, …×29]`（VIA `id_get_protocol_version = 0x01`；byte1/byte2 = 协议版本 `0x000D`，与模组 `tools/ble_via_probe.py` 一致）。
- 期望回包（`quantum/via.c` 原样回发 32B）：`byte0=0x01`、`byte1=0x00`、`byte2=0x0D`
  （`VIA_PROTOCOL_VERSION = 0x000D`）→ 判定 **PASS**；否则 **FAIL** 并列出实际 id/ver。

**UI**

- 共享参数：自检报文 TX 预览（只读十六进制）、超时（默认 **3000ms**）、重试次数（默认 **2**）。
  - 默认值对齐 `tools/ble_via_probe.py`：**2.4G 上行回程有延迟且易失——单次 OUT 写（即便超时拉到 8s）取不到回包，
    需后续 OUT 写触发/命中上行**，故需「≥2 次写、每次 ≥3s 窗口」。BLE 通路首写即回（~50ms），提高默认值对其无副作用。
- 通路 A 卡片：`连接 Dongle` / `断开` + `运行自检`；状态徽标（未连接 / 已连接 / 已断开）。
- 通路 B 卡片：`连接 BLE 模组` / `断开` + `运行自检`；描述中提示前置条件（键盘处于 BLE 模式且系统蓝牙已配对）。
- 结果区回显 TX/RX 十六进制、PASS/FAIL 判定与明细、耗时(ms)；同时写入统一日志（方向标记 `VIA`）。
- 能力检测：两条通路均依赖 `navigator.hid`，缺失时对应通路按钮禁用并提示。

### 5.9 Tab｜日志

- 统一日志：分级 `INFO/OK/WARN/ERR`、方向标记、时间戳；可清空/复制/下载。

### 5.10 参数

| 参数 | 默认 | 可调 |
| --- | --- | --- |
| 串口波特率 | 921600（默认；可选 19200/38400/43000/57600/76800/115200/128000/230400/460800/921600/1843200/3686400/7372800） | 是（仅连接前） |
| 本机心跳间隔 | 1000ms（默认停发） | 是（高级） |
| 命令 ACK 超时 / 重试 | 600ms / 1 | 是（高级） |
| VIA 自检超时 / 重试 | 3000ms / 2 | 是（VIA 测试 Tab） |
| 打字速率 | 12 字符/秒 | 是 |
| 按键保持时长 | 15ms | 是 |

---

## 6. 非功能需求

| 项 | 要求 |
| --- | --- |
| 浏览器 | Chromium 系（Chrome/Edge）；WebSerial 不支持 Firefox/Safari |
| 上下文 | 安全上下文：`localhost` 或 `file://`（Chrome 视为可信）；WebHID（两条通路）亦要求安全上下文与用户手势触发 |
| 依赖 | 无框架、无构建、无网络；纯静态，固件/文本均本地处理 |
| 兼容检测 | 启动时检测 `navigator.serial`，缺失则禁用连接并提示；VIA 测试 Tab 另检测 `navigator.hid`（两条通路共用），缺失则禁用对应通路 |
| 性能 | 帧监视器需支撑 1000+ 帧/次会话而不卡顿（列表上限 + 虚拟化/截断） |
| 可移植 | 帧层（crc16/frame）与 `ota_web` 一致，便于两应用交叉核对 |

---

## 7. 固件需求

**键盘侧 CH582 固件无需改动**：本工具使用 `wireless/ch582f_fw/core` 中已实现并共用的
协议与命令表（`bridge_proto.c` / `bridge_hid.c` / `bridge_cfg.c` 等），行为与 `tools/sk32sim` 完全一致。

### 已确认决策

| # | 决策项 | 结论 |
| --- | --- | --- |
| 1 | 应用目录名 | **`board/debug_web`** |
| 2 | OTA 是否纳入本工具 | **不纳入**：`ota` 场景仅提示并跳转 `board/ota_web`，不在本应用内跑 `FW_*` |
| 3 | 帧层复用方式 | **复制 `ota_web` 的 `crc16.js`/`frame.js` 到 `debug_web/js/` 并扩展**（全命令/事件/错误表），两应用各自独立、无跨目录耦合 |
| 4 | 组帧方式 | 自动递增 SEQ；原始帧编辑器允许手动指定 SEQ |

---

## 8. 测试与验收

| 阶段 | 用例 | 通过标准 |
| --- | --- | --- |
| 单元自检 | `crc16` 对照 `proto.py`；帧编解码往返；解析器容错（AA AA 55 / LEN>64 / CRC 错重同步） | 逐字节一致 |
| 离线回环 | 内置虚拟链路（可选，供无硬件自检）跑 `smoke/nv/config/battery` | 全 PASS |
| 真机-基础 | USB-TTL 直连模组 UART1 → `info` 等价操作 | 版本/状态/MAC/ADC 正确 |
| 真机-监视 | 触发事件/心跳，监视器完整呈现且 CRC 错为 0 | 无丢帧、无未处理帧 |
| 真机-HID | 2.4G 已配对 / BLE 已配对下打字与各 HID 类 | PC/手机端出现对应输入 |
| 真机-场景 | §5.7 非破坏性集合 | 全 PASS，无残留副作用 |
| 真机-VIA 回路 | §5.8 通路 A：WebHID→Dongle→2.4G→模组→UART→SK32 的 `GET_PROTOCOL_VERSION` 往返；通路 B：WebHID→模组 BLE(HOGP RID 0)→UART→SK32 | PASS（回包 `id=01 ver=0x000D`）；通路 B 需键盘处于 BLE 模式且已配对。另：BLE HID 服务注册可经 `HID_DIAG(0x2B)` 取证（`reg=0`、RID0 IN/OUT handle 非零） |
| 异常 | 拔线、错误 CRC、越界参数、重复报文抑制、链路断开 | 提示正确、可恢复、不卡键 |

---

## 9. 已知约束与风险

| # | 项 | 说明 / 处置 |
| --- | --- | --- |
| 1 | UART1 与 SK32 共用 | 使用时须断开 SK32，USB-TTL 直连模组（同 OTA 通路 B） |
| 2 | 射频侧验证不闭环 | 本工具只覆盖命令/事件层；2.4G/BLE 真实输入需 Dongle/主机侧人工确认 |
| 3 | `SET_TEST_MODE` 2.4G 不支持 | UI 明确提示（返回 `UNSUPPORTED`） |
| 4 | 睡眠唤醒源未接 | `DEEP` 后串口唤醒可能不可靠（无 UART 唤醒位）；场景按容错处理 |
| 5 | HID 去重 | 相同报文被抑制；打字/压力须交替状态 |
| 6 | BLE 多主机未真机验证 | `SET_PROFILE` 相关场景标注「待验证」 |
| 7 | WebSerial 特性限制 | 仅 Chromium；端口独占（须关闭其他串口工具） |
| 8 | `FW_END` 后设备复位 | 若纳入 OTA，串口断开属预期（同 ota_web） |
| 9 | VIA 测试·通路 B 前置条件 | 通路 B 走键盘模组 BLE 的 HOGP vendor 集合（Usage Page `0xFF60` / Usage `0x61`，**Report ID 5**，32B IN+OUT）。两侧固件均已就绪：模组侧 vendor OUT/IN 已接入隧道，SK32 侧 `bridge.c` 已处理 `BR_EVT_TUNNEL_BLE(0x86)`、`bridge_host.c` 的 `br_raw()` 已按模式分流（BLE→`0x19`，2.4G→`0x18`）。真机验证须**键盘切到 BLE 模式并在系统蓝牙中完成配对**，使 Windows 枚举出该 HOGP HID 设备，WebHID 方能按 `usagePage 0xFF60` 命中其 vendor 集合 |

---

## 10. 实施阶段（Roadmap）

| 阶段 | 内容 | 产出 |
| --- | --- | --- |
| **Phase 1** | 本规格文档评审确认（含 §7 决策项） | `SPEC.md`（本文） |
| **Phase 2** | 帧层 + 链路层 + 连接区 + 监视器 | `js/crc16.js` `js/frame.js` `js/link.js` `js/monitor.js` + `index.html`/`css` |
| **Phase 3** | 命令台 + 状态仪表盘 + 事件解码 | `js/commands.js` `js/decode.js` |
| **Phase 4** | HID 数据流模拟 + 场景用例 | `js/hid_sim.js` `js/scenarios.js` |
| **Phase 5** | VIA 隧道回路测试 Tab（两条通路均为 WebHID：Dongle RID 0 / 模组 BLE RID 0） | `js/via_test.js` + `index.html`/`app.js` |
| **Phase 6** | 真机测试与验收（§8） | 测试记录 / 验收报告 |

---

## 附：参考来源

- 协议与命令：[bridge_proto.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_proto.h)、[bridge_types.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_types.h)、[bridge_cfg.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_cfg.h)、[bridge_mode.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_mode.h)、[bridge_hid.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_hid.h)
- 同源参考实现：[tools/sk32sim](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim)（`proto.py` / `link.py` / `client.py` / `cli.py` / `scenarios.py`）
- 同类 Web 应用：[ota_web](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/board/ota_web)（`index.html` / `js/frame.js` / `js/serial_ota.js`）
- VIA 测试（§5.8）：本应用 [js/via_test.js](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/board/debug_web/js/via_test.js)；
  Dongle HID 描述符 [bridge_usb_ch58x.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_usb_ch58x.c)；
  模组 GATT 透传 [bridge_ch58x_sdk.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_ch58x_sdk.c)；
  SK32 桥 [bridge.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/keyboards/sk32/ld7_oled/bridge.c) / [bridge_host.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/keyboards/sk32/ld7_oled/bridge_host.c)；
  VIA 协议 [via.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/quantum/via.c) / [via.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/quantum/via.h)
