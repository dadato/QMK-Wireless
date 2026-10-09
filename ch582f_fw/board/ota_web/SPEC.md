# CH582F OTA 升级 Web 应用 —— 规格文档

| 项目 | 内容 |
| --- | --- |
| 文档 | OTA 升级 Web 应用规格（含通讯协议） |
| 版本 | v0.2 |
| 日期 | 2026-10-03 |
| 状态 | 已确认（Phase 1 通过） |
| 适用固件 | `wireless/ch582f_fw`（keyboard / dongle 两个目标，CH582F，CodeFlash 448KB） |

---

## 1. 目标与范围

提供一个在浏览器中运行的**纯静态 Web 应用**，完成两条 OTA 升级通路：

- **通路 A｜接收器 Dongle**：通过 **USB HID**（WebHID）升级。
  采用与键盘侧同源的 WCH IAP 分区方案，由**常驻 IAP 内的 USB HID BootLoader** 收包升级。
- **通路 B｜键盘侧 CH582 模组**：通过 **PC COM 口**（WebSerial，USB-TTL 直连模组 UART1）升级。
  复用固件中**已实现**的 UART OTA 协议（无需改固件）。

**不在范围内**：SK32/QMK 侧固件功能、替代 WCH 官方 ISP 工具（`WCHISPTool`）的量产烧录、BLE/2.4G 业务逻辑。

---

## 2. 术语

| 术语 | 含义 |
| --- | --- |
| App / APP_A | 应用镜像，链接于 `0x00002000`，最大 216KB |
| Stage | 暂存镜像区，`0x00038000`，最大 216KB |
| BOOT | 常驻引导，`0x00000000`，8K；见标志 `UART_IAP`/`USB_IAP` 即驻留**收包模式**（键盘 UART / Dongle USB HID）收 `FW_*`，见 `IAP` 则复验并搬运 Stage→APP_A → 写 `A` → 擦 Stage → 跳 APP_A |
| 桥帧 | 键盘侧 UART 协议帧（`AA 55 ...`） |
| HID OTA 报告 | Dongle 侧 32B USB HID 报文（本规格新增） |
| ENTER_OTA / ENTER_BOOT | 通知 App 进入 OTA（写标志并复位进 BOOT）的指令；Dongle 走 HID `ENTER_OTA`，键盘走桥帧 `CMD_ENTER_BOOT(0x08)` |
| 重连 | 设备复位后 USB 重新枚举，WebHID 句柄失效需重开 |

---

## 3. 系统架构与数据流

### 3.1 通路 B：键盘侧模组（串口）

```
PC 浏览器 (WebSerial)
   │  USB-TTL (921600 8N1)
   ▼
CH582F 模组 UART1 (PA8=RXD1 / PA9=TXD1)
   │
   ▼ ① App 运行中收 CMD_ENTER_BOOT(0x08) → 释放按键 + 写标志 UART_IAP → 软复位
BOOT(0x0) 见标志 UART_IAP → 驻留 UART 收包模式（iap_uart_ota_run，轮询、无无线栈）
   │
   ▼ ② 收 FW_BEGIN/DATA/END → 写 Stage → CRC 校验通过 → 写标志 IAP → 复位
BOOT(0x0) 见标志 IAP → 复验 Stage CRC → 搬运 Stage→APP_A（1KB 分块）
   │
   ▼ ③ 写标志 A → 擦 Stage → 跳 0x2000 新 App
```

> 注意：模组 UART1 平时与键盘主控 SK32 相连。OTA 时须由 USB-TTL **直连模组 UART1**（SK32 侧不参与）。
> 模组侧 `BRIDGE_BOARD_HAS_USB = 0`，无 USB，故只能用串口。
> BOOT 驻留期间不发周期心跳，仅应答 `CMD_PING`（回 `EVT_PONG`）；不支持的 CMD 忽略不应答。
> 取消 / 失败 / 空闲超时（60s）时 BOOT 写标志 `A` 回原 App（APP_A 自始未动）。

### 3.2 通路 A：接收器 Dongle（USB HID）

```
PC 浏览器 (WebHID, EP3 32B)
   │
   ▼
Dongle App（运行中）-- 收到 ENTER_OTA --> 释放按键 + 写 DataFlash 标志 + 复位
   │  （USB 断开、重新枚举）
   ▼
Dongle BOOT（8K）-- 自带 USB HID 设备栈 --> 收 FW_BEGIN/DATA/END
   │  写 Stage → CRC 复验 → 搬运 Stage→APP_A → 标志=A → 擦 Stage → 跳 App
   ▼
Dongle App 重新枚举 （Web 侧重连并校验版本）
```

> 与键盘侧的**唯一差异**：键盘侧由「App 收包、BOOT 仅搬运」；Dongle 侧由「App 仅触发、BOOT 收包+搬运」。
> 分区布局、标志记录、搬运语义两板**完全一致**。
> Dongle 的 USB 会在「进 BOOT」与「升完回 App」时各断开重连一次，全程 **2 次重连**。
> 重连由 Web 端**自动完成**：IAP(BOOT) 与 App 的设备/配置/报表描述符**逐字节一致**
> （iSerial 为空时 Chrome 以接口路径 `mi_XX` 记忆授权；仅 VID/PID/usage 相同不够，
> 必须连接口布局——复合 3 接口、Raw HID 在 IF2——也一致），首次授权对同一 origin 持续有效，
> 重连改用 `navigator.hid.getDevices()` + `open()` 静默重开，**用户无感**（仅首次配对需一次用户手势）。

---

## 4. 通讯协议 A：键盘侧串口（UART1 桥帧）

来源：[bridge_proto.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_proto.h#L1-L28)、[bridge_uart.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_uart.c#L90-L118)、[bridge_types.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_types.h#L40-L48)。

### 4.1 物理层

| 项 | 值 |
| --- | --- |
| 波特率 | 921600 |
| 数据/校验/停止 | 8 / N / 1 |
| 流控 | 无 |
| 引脚 | PA8 = RXD1，PA9 = TXD1 |

### 4.2 帧格式

```
+--------+-----+-----+-----+-------+---------+-----------+
| SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16     |
| AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B (LE)   |
+--------+-----+-----+-----+-------+---------+-----------+
```

- `SOF` = `0xAA 0x55`（固定）。
- `LEN` = PAYLOAD 字节数，范围 `0..64`（`BRIDGE_MAX_PAYLOAD = 64`）。
- `CRC16` 小端写入帧尾。
- 帧开销 = SOF(2) + 头(4) + CRC(2) = **8B**；单帧最大 = 8 + 64 = **72B**。

### 4.3 CRC

- 使用工程的 `crc16_ccitt`（与 [proto.py](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/proto.py) 的 `crc16_ccitt` 一致；CRC-16/CCITT-FALSE 族：init `0xFFFF`、poly `0x1021`、无反转、无异或输出）。
- **覆盖范围：从 `LEN` 开始、到 `PAYLOAD` 结束（不含 SOF）**——见 [bridge_uart.c L114-L115](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_uart.c#L114-L115)。

### 4.4 帧标志（FLAGS）

| 位 | 名称 | 值 | 含义 |
| --- | --- | --- | --- |
| 0 | `BRIDGE_FLAG_ACK_REQ` | `0x01` | 请求对端应答 |
| 1 | `BRIDGE_FLAG_ACK` | `0x02` | 本帧是对某请求的应答 |
| 2 | `BRIDGE_FLAG_ERR` | `0x04` | 错误帧（配合错误码） |

### 4.5 时序约束

| 项 | 值 |
| --- | --- |
| 最小帧间隔 | 5ms |
| ACK 超时 | 300ms |
| 最大重试 | 3 次 |
| 心跳 | 1000ms（OTA 期间无需关注） |

### 4.6 OTA 命令

来源：[bridge_types.h L40-L48](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_types.h#L40-L48)、[client.py L142-L191](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/client.py#L142-L191)。

| CMD | 值 | 请求 PAYLOAD | 应答 | 语义 |
| --- | --- | --- | --- | --- |
| `CMD_ENTER_BOOT` | `0x08` | 无 | 无（设备随后复位） | App：释放按键 + 写标志 `UART_IAP` + 软复位 → 进 BOOT 收包模式 |
| `CMD_FW_BEGIN` | `0x21` | `[total_len u32 LE][crc16 u16 LE]` | `[chunk_max u8]` | 声明总长+整包 CRC；分配暂存、进入 OTA 态 |
| `CMD_FW_DATA` | `0x22` | `[offset u32 LE][data ≤60B]` | 空 ACK | 写入分片；`offset` 必须 = 已写入量、**4 字节对齐**；重复帧幂等 |
| `CMD_FW_END` | `0x23` | 无 | 空 ACK | 校验 CRC → 写标志 `IAP` → 复位（ACK 先于复位发出，可能丢失，见 4.8） |
| `CMD_FW_ABORT` | `0x24` | 无 | 空 ACK | 放弃本次传输，恢复默认无线模式 |

- `chunk_max`：固件回填，`BRIDGE_FW_CHUNK_MAX = 60`。
- 镜像最大长度：`BRIDGE_FW_MAX_LEN = 216KB`。
- 应答帧：`CMD` = 请求命令回显，`FLAGS` 置 `BRIDGE_FLAG_ACK`；`FW_BEGIN` 的应答 PAYLOAD 首字节为 `chunk_max`。
- **收包方**：Web 端现用 `CMD_ENTER_BOOT` 先让 App 复位进 BOOT，随后 `FW_*` 由**驻留 BOOT** 处理（`iap_uart_ota_run`）。App 侧仍保留 legacy 直收 `FW_*` 路径（`bridge_proto.c` 的 `handle_fw_*`），但 Web 流程不再使用。
- `CMD_ENTER_BOOT` 无应答属**预期**：App 收到即复位，且不发心跳；Web 端按写超时（800ms）后等待串口重连。

### 4.7 升级序列（Web 端）

1. 打开串口 `921600 8N1` → 发 `CMD_ENTER_BOOT(0x08)`（写超时 800ms，**不等 ACK**）→ App 复位。
2. 等待设备重启（约 400ms）→ **重建串口连接**（同一 COM 口重新 `open()`）→ 确认 BOOT 在线。
3. 读 `.bin`（或 `.hex` 转 bin）→ 取原始长度 `raw_len`。
4. `pad = (4 - raw_len % 4) % 4`，尾部补 `0xFF`；`total = raw_len + pad`。
5. `crc = crc16_ccitt(data[0..total])`。
6. 发 `FW_BEGIN(total, crc)` → 取应答 `chunk_max`；为 0 表示拒绝（错误码见 `EVT_ERROR`）。
7. `n = min(chunk_max, 60)` 向下取 4 的倍数（默认 60）。
8. 对 `off = 0, n, 2n, ...` 循环发 `FW_DATA(off, data[off:off+n])`，每帧等 ACK（超时 2s、重试 2 次）。
9. 发 `FW_END`；若超时且设备已复位，视为成功（见 4.8）。
10. 设备复位搬运并重启回新 App，串口断开；UI 提示升级成功。

### 4.8 异常与恢复

| 场景 | 处理 |
| --- | --- |
| `CMD_ENTER_BOOT` 无应答 | **预期**：App 随即复位，不发心跳 → 不视为错误，转而等待串口重连 |
| `FW_BEGIN` 应答 `chunk_max = 0` | 中止，读 `EVT_ERROR` 错误码并提示 |
| `FW_DATA` ACK 超时 | 重发同 `offset`（幂等）；连续失败则 `FW_ABORT` |
| `FW_END` 无 ACK | **预期**：BOOT 先 ACK 再写标志并复位，ACK 可能因复位而丢失 → 以「设备已复位/串口断开」视为成功 |
| 升级中断电 | 标志保持 `IAP`，下次上电自动重试搬运（Stage 未被擦） |
| 取消 / 失败 / 空闲超时 | BOOT 写标志 `A` 回原 App（APP_A 自始未动，非「单区无回滚」） |

### 4.9 报文示例（`FW_BEGIN`，total=1000、crc=0x1234）

```
请求: AA 55 06 21 01 01   E8 03 00 00  34 12    CRC16(2B)
      SOF   LEN CMD SEQ FLAGS  total_len   crc
应答: AA 55 01 21 01 02   3C       CRC16(2B)      （chunk_max=0x3C=60）
```

---

## 5. 通讯协议 B：Dongle USB HID（EP3）

来源（现状）：[bridge_usb_ch58x.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_usb_ch58x.c#L1-L45)。

### 5.1 设备标识与接口

| 项 | 值 |
| --- | --- |
| VID / PID | `0x0194` / `0x0463`（运行期可经 `CMD_SET_USB_ID` 改写） |
| 接口数 | 3（IF0 键盘 / IF1 辅助键 / IF2 VIA Raw HID） |
| OTA 使用接口 | IF2，`usage page 0xFF60 / usage 0x61` |
| 端点 | EP1 = 键盘 IN；EP2 = 辅助键 IN；**EP3 = IN + OUT，32B，1ms** |

> **IAP(BOOT) 与 App 的描述符逐字节一致**：相同的设备描述符（VID/PID、bcdDevice、iSerial=NULL）、
> 相同接口布局（复合 3 接口，Raw HID 在 IF2）、相同 `usage page 0xFF60 / usage 0x61` 与相同 EP3(32B)，
> 阶段可经 `GET_INFO.mode` 区分（`0`=App / `1`=IAP）。方案A 下 Web 全程在 App 内收包，
> 仅在 `FW_END` 后复位一次，故重连要求由「两次」降为「一次」。
> ⚠️ 仅 VID/PID/usage 相同**不够**：Chrome 的 WebHID 授权按设备实例（iSerial 为空时即接口路径
> `mi_XX`）记忆，接口布局不同会让 App 与 BOOT 成为两个实例，导致静默重连失败（表现为「要连两次」
> 或「升完回 App 检测超时误报失败」）。BOOT 侧（`board/iap/iap_usb_main.c`）与 App 侧
> （`port/ch58x/bridge_usb_ch58x.c`）的描述符数组必须保持镜像一致。

### 5.2 报告长度

- EP3 IN / OUT 均为 **32 字节**单包。
- WebHID 以 `reportId = 0`、`data.length = 32` 发送/接收。

### 5.3 报文分派（兼容现有行为）

OUT 报告首字节 `b0` 分派（[现有实现 L533-L552](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_usb_ch58x.c#L533-L552)）：

| 判定 | 行为 |
| --- | --- |
| `b0 == 0x00` | 原 VIA 透传：`sdk_rf24_send_tunnel(buf, len)`（**保持不变**） |
| `buf[0..3] == "OTA!"`（`4F 54 41 21`） | 原进 ISP：释放按键 + `port_enter_bootloader()`（PB22 拉低 + 复位 → ROM ISP）（**保持不变**） |
| `buf[0..2] == "OTA"` 且 `buf[3] ∈ 0x01..0x0F` | **新增** HID OTA 命令（本规格） |

### 5.4 HID OTA 命令集（新增）

OUT 报告布局：

```
byte0..2 = 'O','T','A' (0x4F,0x54,0x41)
byte3    = SUB
byte4..31= ARGS (28B)
```

| SUB | 名称 | ARGS | 应答 DATA | 语义 |
| --- | --- | --- | --- | --- |
| `0x01` | `ENTER_OTA` | 无 | 无（不应答） | App：释放残留按键；方案A **不复位、不切 IAP**，FW_* 收包仍由 App 完成 |
| `0x02` | `FW_BEGIN` | `[total_len u32 LE][crc16 u16 LE]` | `[chunk_max u8]` | 声明总长+CRC，进入 OTA 暂存态（App 与 BOOT 实现一致） |
| `0x03` | `FW_DATA` | `[offset u32 LE][data ≤24B，4 对齐]` | 无 | 写分片；`offset` 必须顺序追加、4 对齐；幂等 |
| `0x04` | `FW_END` | 无 | 无 | CRC 复验 → 写标志 `IAP` → 复位；BOOT 随后搬运 Stage→APP_A → 写 `A` → 擦 Stage → 跳 App |
| `0x05` | `FW_ABORT` | 无 | 无 | 放弃本次传输 |
| `0x06` | `GET_INFO` | 无 | `[mode u8][fw_rev u16 LE][vid u16 LE][pid u16 LE]` | `mode`：`0`=App，`1`=IAP/OTA |

> HID 单包 32B，`ARGS`=28B；`FW_DATA` 去掉 offset(4B) 后有效数据 **≤24B**（`chunk_max = 24`）。
> 该子命令集对「App 实现」与「IAP 实现」**完全一致**；Web 端只需按 `GET_INFO.mode` 判断当前所处阶段。

### 5.5 应答格式（EP3 IN，32B）

```
byte0    = status   （0 = OK；非 0 = 错误码，取 BRIDGE_ERR_* 的负值取反或 ABS，见 §7.6）
byte1..31= data
```

### 5.6 升级序列（Web 端）

1. WebHID 选择设备（过滤 `usagePage 0xFF60`）→ 打开 → `GET_INFO`，期望 `mode = 0`（App）。
2. 发 `ENTER_OTA` → App 释放残留按键（**方案A：不复位、不应答**，收包仍在 App 内完成）。
3. 读 `.bin` → 按 24B 补齐 `0xFF` → 算 CRC → `FW_BEGIN` → 取 `chunk_max`（应答即证明 App 就绪）。
4. 循环 `FW_DATA`（≤24B/帧，4 对齐，offset 顺序递增）→ 每帧等应答。
5. `FW_END` → App 复验 CRC → 写标志 `IAP` 并复位（WebHID 触发 `disconnect`）。
6. 重启后常驻 BOOT 复验 Stage 并搬运 Stage→APP_A → 写 `A` → 擦 Stage → 跳 App。
7. 设备重新出现 → 重新 `open()`（首次授权后**静默重连**即可）→ `GET_INFO`，期望 `mode = 0` 且版本号更新 → 成功。

### 5.7 异常与恢复

| 场景 | 处理 |
| --- | --- |
| `GET_INFO` 长时间停留在 `mode = 1` | 提示 BOOT 内升级失败；可重发 `FW_ABORT` 或断电重启 |
| `FW_BEGIN` 返回 `chunk_max = 0` | 中止并提示 |
| 传输中断（拔线/关页） | App 保持运行（未写标志）；重连后重新 `ENTER_OTA` 从头再传（Stage 幂等覆盖） |
| 设备卡在 BOOT 无法回 App | 引导断电重启；恢复路径复用 **PB22 → ROM ISP**（无需额外硬件兜底） |

### 5.8 报文示例（`FW_BEGIN`，total=1000、crc=0x1234）

```
OUT: 4F 54 41 02   E8 03 00 00  34 12   00*22
     O  T  A  SUB    total_len     crc    padding
IN : 00 18 00*30            （status=0，chunk_max=0x18=24）
```

---

## 6. 镜像格式

| 项 | 规格 |
| --- | --- |
| 主用格式 | `.bin`（`make` 产物，App 链接于 `0x2000`，直接作为整包） |
| 可选格式 | Intel HEX（`.hex`）→ 解析为 bin（按地址升序拼接，空隙填 `0xFF`） |
| 对齐 | 尾部补 `0xFF` 至 4 字节对齐 |
| 整包 CRC | `crc16_ccitt` 覆盖**补齐后**全部字节 |
| 最大长度 | 216KB（`0x36000`）；超出直接拒绝 |
| 产线烧录 | `make images` → `build/boot/bridge_boot_*.bin`（→0x0，BOOT）与整机合并 hex `bridge_<t>_full.hex`（BOOT+APP_A，一条烧完） |

---

## 7. Web 应用功能规格

### 7.1 布局

单页，两个通道以 Tab 切换，共用固件区与日志区：

```
┌───────────────────────────────────────────────┐
│ 固件: [选择文件] 文件名  原始 101376 B → 补齐 101376 B  CRC 0xABCD │
├───────────────────────────────────────────────┤
│ [键盘模组 · 串口] | [接收器 · USB HID]          │
│  （当前通道的连接/操作面板 + 进度条 + 阶段指示）  │
├───────────────────────────────────────────────┤
│ 日志（带时间戳，可清空/复制）                     │
└───────────────────────────────────────────────┘
```

### 7.2 通道一：键盘模组（WebSerial）

- 连接：选择 COM 口（`navigator.serial.requestPort()`）→ 打开 `921600 8N1`。
- 可选：发 `CMD_GET_VERSION` 显示模组版本，确认在线。
- 状态机：`IDLE → PORT_OPEN → BEGIN → TRANSFERRING → END → DONE`。
- 帧收发：实现 §4 编解码、CRC 校验、ACK 等待（300ms/重试）。
- 关键点：**过滤并忽略**无关帧（如 `EVT_STATE`、`EVT_PONG`、心跳）；只对匹配 `SEQ/CMD` 的 ACK 计数。

### 7.3 通道二：接收器（WebHID）

- 连接：`navigator.hid.requestDevice({filters:[{usagePage:0xFF60}]})`。
- 方案A：收包全程留在 App（`ENTER_OTA` 不复位），仅 `FW_END` 后设备复位一次；重连用 `navigator.hid.getDevices()` 静默重开，**不再弹选择框**（首次授权后用户无感）。
- 状态机：`IDLE → APP_OPEN → ENTER_OTA → WAIT_IAP → IAP_OPEN → BEGIN → TRANSFERRING → END → WAIT_APP → VERIFY → DONE`。
- 关键点：**静默重连**——首次 `requestDevice` 授权后，用 `getDevices()` 取回同一设备并 `open()`，无需再次用户手势；仅在设备长时间未出现（超出等待窗口）时才提示用户手动选择。

### 7.4 参数

| 参数 | 默认 | 可调 |
| --- | --- | --- |
| 串口波特率 | 921600（默认；可选 19200/38400/43000/57600/76800/115200/128000/230400/460800/921600/1843200/3686400/7372800） | 是（仅连接前） |
| 串口分片 | 60B | 是（4 对齐，≤chunk_max） |
| HID 分片 | 24B | 否（受 32B 报告限制） |
| ACK 超时/重试 | 300ms / 3 | 高级项可调 |

### 7.5 进度与日志

- 进度 = `已发送字节 / total`，显示百分比 + 速率（B/s）+ 预计剩余。
- 日志分级：`INFO / OK / WARN / ERR`，含方向标记（`→` 发送 / `←` 接收）。

### 7.6 错误处理

| 错误 | 提示 | 处置 |
| --- | --- | --- |
| 用户取消选择设备 | 未选择设备 | 留在 `IDLE` |
| 串口打开失败/被占用 | COM 口被占用 | 提示关闭其他工具 |
| 固件超长 | 超过 216KB | 拒绝加载 |
| `FW_BEGIN` 拒绝 | 设备状态不允许 | 尝试 `ENTER_OTA`/复位后重试 |
| 分片 ACK 超时 | 传输超时 | 重发；连续失败则中止 |
| CRC 失败（`BRIDGE_ERR_CRC = -9`） | 镜像校验失败 | 重新加载固件再传 |
| HID 断开未在预期阶段 | 设备异常断连 | 引导重新连接 |
| 设备停留在 IAP | 未能回到 App | 引导断电重启；或用 PB22 进 ROM ISP 重烧 |

### 7.7 取消

- 提供「中止」：串口通道发 `FW_ABORT`；HID 通道发 `FW_ABORT`。随后回到 `IDLE`。
- 关闭页面/断开连接视为异常中断（§4.8 / §5.7 的恢复路径兜底）。

---

## 8. 非功能需求

| 项 | 要求 |
| --- | --- |
| 浏览器 | Chromium 系（Chrome / Edge）；WebHID + WebSerial 均不支持 Firefox/Safari |
| 上下文 | 安全上下文：`localhost` 或 `file://`（Chrome 视为可信） |
| 依赖 | 无框架、无构建、无网络；固件文件本地读取，不上传 |
| 兼容性检测 | 启动时检测 `navigator.hid` / `navigator.serial`，缺失则禁用对应 Tab 并提示 |
| 性能参考 | 键盘 216KB ≈ 60–90s；Dongle 216KB ≈ 10–25s（受包间隔与 ACK 影响） |

---

## 9. 固件需求（差异清单）

### 9.1 Dongle 侧（需改动）

1. **链接脚本**：新增 `board/dongle/Link_App216K.ld`（复制键盘版，FLASH `0x2000`/216K），[Makefile](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/Makefile#L104-L108) 的 dongle 分支改用它。
2. **App**：`DevEP3_OUT_Deal` 新增 §5.4 的 `ENTER_OTA`(+`GET_INFO`) 分派；写 DataFlash 标志（`BRIDGE_OTA_FLAG_USB_IAP`）后复位。
3. **BOOT**：`board/iap/iap_usb_main.c`——自带 USB HID 设备栈 + EP3 收发，实现 §5.4 的 `FW_BEGIN/DATA/END/ABORT/GET_INFO`，写 Stage → CRC 复验 → 搬运 Stage→APP_A（复用 iap_common.c 共享搬运逻辑）。App 无效（空片）时也进收包模式，故工厂只烧 BOOT 即可完成首次 OTA。
   > 体积参考：本工程 BOOT 分区 8K；Dongle BOOT 实测 5.6K，充裕（以实测为准）。
4. **标志**：复用 [bridge_ota_layout.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_ota_layout.h) 的记录（`0x03 = IAP` 暂存就绪、`0x04 = USB_IAP` 请求 USB 收包）。
5. **无需额外硬件兜底**：恢复路径复用既有的 `PB22 → ROM ISP`（`port_enter_bootloader()`）。
6. **首次量产烧录**：`make images` → 用 `WCHISPTool` 烧**一条**合并 hex（BOOT@0x0 + APP_A@0x2000）：
   键盘 `build/boot/bridge_keyboard_full.hex`、接收器 `build/boot/bridge_dongle_full.hex`。旧三镜像布局需经 ISP 重烧一次新布局，此后浏览器 OTA 才生效。

### 9.2 键盘侧（无需改动）

固件已完整实现 §4；仅需 Web 端对齐其行为。

---

## 10. 测试与验收

| 阶段 | 用例 | 通过标准 |
| --- | --- | --- |
| 协议单元 | `crc16_ccitt` 对照 `proto.py`；帧编解码往返 | 与 Python 参考逐字节一致 |
| 离线联调 | 用 `tools/sk32sim` 客户端发 `FW_*`，Web 端同流程 | 等价行为、序列一致 |
| 键盘真机 | USB-TTL 直连模组 UART1 → Web 升级 | 版本号更新、可正常建链 |
| Dongle 真机 | `make images` 首次 ISP 烧录 → Web 升级 | 单次复位后重连成功、版本号更新 |
| 异常 | 拔线、错误 CRC、超长镜像、ACL 拒绝 | 提示正确、可恢复、无变砖 |
| 回归 | 升级后 VIA 透传/HID 业务 | 功能正常 |

---

## 11. 已确认决策与风险

| # | 决策项 | 结论 |
| --- | --- | --- |
| 1 | Dongle 收包位置 | **IAP 自带 USB HID BootLoader**（App 仅触发；App 损坏仍可救） |
| 2 | 硬件兜底引脚 | **不需要**；恢复路径复用 `PB22 → ROM ISP` |
| 3 | IAP(BOOT) 的设备/配置/报表描述符 | **与 App 逐字节一致**（VID/PID、iSerial、复合 3 接口、Raw HID=IF2、EP3 均同） |
| 4 | IAP 的 HID usage | **沿用 App 的 `0xFF60 / 0x61` 且接口布局一致**，仅靠 `GET_INFO.mode` 区分阶段 |
| 5 | Web 端重连体验 | **自动静默重连，用户无感**（首次授权后经 `getDevices()` 重开） |

**剩余风险**：Dongle 切到 216K 分区后固件体积是否超限（Phase 3 构建时核实）。

---

## 12. 实施阶段（Roadmap）

| 阶段 | 内容 | 产出 |
| --- | --- | --- |
| **Phase 1** | 本规格文档（含协议）评审确认 | `SPEC.md`（本文） |
| **Phase 2** | 设计并实现 Web 应用 | `board/ota_web/`（`index.html` + `js/*`） |
| **Phase 3** | Dongle 固件改动（LD / App 分派 / IAP USB HID / Makefile） | 固件源码 + `make images` 产物 |
| **Phase 4** | 测试与验证（§10 全表） | 测试记录 / 验收报告 |

---

## 附：参考来源

- 桥协议与命令：[bridge_proto.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_proto.h)、[bridge_types.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_types.h)、[bridge_config.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/core/bridge_config.h)
- 参考实现：[client.py](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/tools/sk32sim/client.py)
- 分区布局：[bridge_ota_layout.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_ota_layout.h)、[iap_main.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/board/iap/iap_main.c)
- Dongle USB：[bridge_usb_ch58x.c](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/port/ch58x/bridge_usb_ch58x.c)
- 构建：[Makefile](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/ch582f_fw/Makefile)
- WCH 参考：[USB_IAP](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/IAP/USB_IAP/src/Main.c)
