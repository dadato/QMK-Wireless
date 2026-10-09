# CH582F OTA 升级 Web 应用

纯静态网页，浏览器内完成两条 OTA 升级通路（协议与流程见 [SPEC.md](SPEC.md)）：

| 通路 | 目标 | 通道 | 协议 |
| --- | --- | --- | --- |
| A | 接收器 Dongle | WebHID（EP3，32B，usagePage `0xFF60`） | `"OTA"` + SUB 子命令 |
| B | 键盘侧 CH582 模组 | WebSerial（USB-TTL 直连模组 UART1，921600 8N1） | `AA 55` 桥帧 `ENTER_BOOT` → 常驻 BOOT 收 `FW_BEGIN/DATA/END` |

## 运行

无框架、无构建、无网络。任选其一：

- 直接双击 `index.html`（Chrome/Edge 视 `file://` 为可信上下文）；
- 起本地静态服务（推荐，兼容性最好）：

```powershell
# 在 board/ota_web 目录下
python -m http.server 8000
# 浏览器打开 http://localhost:8000/
```

浏览器要求：Chromium 系（Chrome / Edge）。Firefox / Safari 不支持 WebHID / WebSerial。

## 使用

1. **选择固件**：`.bin`（推荐，`make` 产物）或 Intel HEX `.hex`。加载后显示原始长度、4B 补齐后长度与整包 CRC；超过 216KB 直接拒绝。
2. **键盘模组（串口）**：
   - 用 USB-TTL 直连模组 UART1（PA8=RXD / PA9=TXD），**SK32 侧不参与**；
   - 「连接串口」→ 选 COM 口 → 自动发 `GET_VERSION` 确认在线；
   - 「开始升级」→ 先发 `ENTER_BOOT` 让 App 写 `UART_IAP` 标志并复位，等待并**重建串口**连上常驻 BOOT；
   - 随后 `FW_BEGIN → FW_DATA×N → FW_END`（由 BOOT 收包写入 Stage），进度条显示百分比 / 速率 / 预计剩余；
   - `FW_END` 后 BOOT 写 `IAP` 并复位搬运，串口断开属**预期**，界面按成功处理；异常/取消时 BOOT 空闲 60s 自动写 `A` 回原 App。
3. **接收器（USB HID）**：
   - 「选择设备」→ 弹窗中授权 VIA Raw HID 接口（首次需一次用户手势）；
   - 「开始升级」→ 先发 `ENTER_OTA` 让 App 释放残留按键（**方案A：不复位、不切 IAP，FW_* 收包仍在 App 内**）；
   - `FW_END` 后 App 写标志 `IAP` 并复位，常驻 BOOT 搬运 Stage→APP_A 并跳新 App；网页可静默重连校验版本号（用户无感）。

发起升级前状态机自动区分阶段：`GET_INFO.mode` = 0（App）/ 1（IAP）。

## 文件结构

```
ota_web/
├── index.html        双 Tab 界面（固件区 / 通道面板 / 阶段 / 进度 / 日志）
├── css/style.css
├── js/crc16.js       CRC-16/CCITT-FALSE（init 0xFFFF / poly 0x1021）
├── js/frame.js       AA55 桥帧编解码 + 增量解析器 + 错误码表
├── js/serial_ota.js  WebSerial：ACK 等待/重试、事件自动应答、FW_* 全流程
├── js/hid_ota.js     WebHID："OTA"+SUB 命令、EP3 应答解析、静默重连
└── js/app.js         流程编排、固件解析（bin/hex）、UI 状态机
```

## 注意

- 键盘侧固件**无需改动**，协议已实现；Dongle 侧需要 Phase 3 的固件改动（App 增加 `ENTER_OTA`/`GET_INFO` 分派、常驻 BOOT 自带 USB HID BootLoader、216K 链接脚本），否则 HID 通道不可用。
- 首次使用 Dongle 的浏览器 OTA，需先 `make images` + `WCHISPTool` 烧入**一条**整机合并 hex（`build/boot/bridge_dongle_full.hex` = BOOT@0x0 + APP_A@0x2000）。
- 恢复兜底：任何异常都可用 `PB22 → ROM ISP`（WCHISPTool）重烧，无需额外硬件。

## 离线自检

浏览器控制台提供 `window.OtaDebug`：

```js
OtaDebug.crc16(new TextEncoder().encode('123456789')).toString(16)   // "29b1"
OtaDebug.frame.encode(0x21, 1, 1, new Uint8Array([0xe8,3,0,0,0x34,0x12]))
OtaDebug.SerialOta.prepare(new Uint8Array([1,2,3]))                  // { total, crc, data }
```
