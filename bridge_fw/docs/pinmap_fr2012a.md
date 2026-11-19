# FR2012A 端口（port/fr2012a）引脚与构建说明

> 芯片：FreqChip FR2012A（FR800x 系列，ARM Cortex-M3）
> 厂商资料：`wireless/vendor/FREQCHIP/FR8000U-SDK-v2.2`（厂商 SDK v2.2 / FR8000U）
> 构建：`make keyboard CHIP=fr2012a` / `make dongle CHIP=fr2012a`
>
> **引脚为初版默认值，投板前须对照 `FR201x-U_GPIO复用功能表` 与 EVB 原理图核对。**

## 1. 端口组成

| 文件 | 作用 |
| --- | --- |
| `bridge_chip.h` | 对 `app/` 暴露的唯一入口（`bridge_chip_boot/poll`） |
| `bridge_fr2012a_boot.c` | jump_table（镜像类型/栈顶）、协议栈初始化、`user_main()` 主循环 |
| `bridge_fr2012a_sdk.c/.h` | 全工程唯一的 SDK 原语层（时钟/UART/GPIO/Flash/MAC/复位） |
| `bridge_ble_fr2012a.c/.h` | BLE 适配层（键盘 HOGP 外设 / 接收器 GATT 主机） |
| `bridge_gatt_fr2012a.c/.h` | 私有桥 GATT 服务 0xFFF0（0xFFF1 写 / 0xFFF2 通知） |
| `bridge_usb_fr2012a.c/.h` | 接收器 USB HID 复合设备封装（LED/挂起回传、报表转发） |
| `bridge_port_fr2012a.c` | `hal/bridge_port.h` 全部实现 + 芯片与 core 的事件桥 |
| `bridge_vendor_compat.c` | 厂商示例主程序里的符号（`master_write_data`/`show_reg`） |
| `board/{keyboard,dongle}/bridge_board.h` | 板级引脚与角色宏 |
| `usb/` | 本工程的复合 HID 描述符与 SET_REPORT 处理（USB 核心栈来自 SDK `driver/usb_core.c`、`usb_dev.c`） |
| `hid/` | HOGP HID 服务（厂商 `ble/profiles/ble_hid/hid_service.c` + 桥接弱钩子） |
| `ld/app.ld` | App 链接脚本（含 jump_table、ram_code、stack_section 放置） |
| `port.mk` | 工具链（`arm-none-eabi-`）、SDK 源集、链接脚本、编译宏 |

## 2. 键盘模组（keyboard）

| 信号 | 引脚 | 方向 | 说明 |
| --- | --- | --- | --- |
| UART0_RXD | PA0 | 输入 | 接 SK32 TX（桥数据面，复用号 0x04） |
| UART0_TXD | PA1 | 输出 | 接 SK32 RX（复用号 0x04） |
| WAKE | PB2 | 输出 | 高有效；唤醒主机/指示模组活动 |
| nRESET | PB3 | 输入上拉 | 由 SK32 控制模组复位 |
| DBG_TX | PA5 | 输出 | UART2_TXD（复用号 0x0B），仅日志发送 |

> PA0/PA1 同时是 ROM 串口烧录口（FREQCHIP_Download TX→PA0、RX→PA1），
> 烧录时需断开与 SK32 的连接。

与 CH58x 版一致：无 MODE_SEL、LINK、BAT_ADC、CHRG_STAT 引脚；上电缺省 USB
（射频关闭），模式由 SK32 经 `SET_MODE` 下发；电量/充电状态只由 `SET_BATTERY` 输入。

## 3. 接收器（dongle）

| 信号 | 引脚 | 方向 | 说明 |
| --- | --- | --- | --- |
| USB_DP | PA4 | 双向 | USB 2.0 Full-Speed |
| USB_DM | PA5 | 双向 | 与 DP 成对，需 `pmu_usb_pad_ctrl(1)` |
| LED_STAT | PB1 | 输出 | 链路指示（`port_gpio_set_link`） |
| PAIR_KEY | PB0 | 输入上拉 | 预留（当前配对由命令触发） |
| DBG_TX | PA1 | 输出 | UART0_TXD，仅日志发送 |

USB 标识（可在运行时改）：`port/fr2012a/board/dongle/bridge_board.h` 里的
`BRIDGE_USB_VID`/`BRIDGE_USB_PID`（默认 `0x0194`/`0x0463`）只是 **NV 首次初始化的默认值**。

SK32 通过桥协议下发后立即生效并重新枚举：

| 命令 | 作用 | 实现 |
| --- | --- | --- |
| `CMD_SET_USB_ID (0x20)` | 改 idVendor/idProduct，2B+2B LE | 改写设备描述符偏移 8/10 → 拉低/拉高 D+ 上拉重枚举 |
| `CMD_SET_NAME (0x1F)` target=USB | 改产品名字符串 | 重建 UTF-16LE 字符串描述符 → 重枚举 |

两条命令都由 core 先写入 NV（掉电保持），再调用 `port_set_usb_ids` / `port_set_name`。
厂商 USB 栈本身没有运行时改 ID 的例程，但它用 `usbdev_get_dev_desc()` /
`usbdev_get_string_Product()` 注册描述符指针——本端口就是借助该入口替换成可写缓冲，
因此不需要改厂商 `usb/usb_hid_user.c` 里的常量描述符。

## 4. 时钟 / 内存 / Flash 分区

| 项 | 取值 | 说明 |
| --- | --- | --- |
| 系统时钟 | 96 MHz | `SYSTEM_CLOCK_SEL=5`（BLE 栈要求） |
| Flash | `0x10000000` | App 区按 256KB 链接（`ld/app.ld`） |
| RAM | `0x11001000` + `0xB000` | `0x1100C000` 以上留给闭源 BLE 栈（`__initial_sp`） |
| 栈 | `.stack_section` | 贴在 RAM 顶端，`jump_table_3.stack_top_address` 指向其末尾 |
| NV | `0x7A000` | 桥配置/名称，4KB 扇区读-改-写 |
| MAC | `0x7B000` | 首次上电由 Flash ID 生成并固化 |
| BLE 绑定 | `0x7D000` / `0x7E000` / `0x7F000` | 绑定信息 / 对端服务表 / 密钥，与厂商默认一致 |

## 5. BLE 拓扑（与 CH58x 的差异）

| 场景 | CH58x | FR2012A |
| --- | --- | --- |
| `BRIDGE_RADIO_BLE` | HOGP 直连主机（HID 报表 notify） | 同左（`hid_service.c`，8B 键盘 / 2B 多媒体 / 4B 鼠标） |
| `BRIDGE_RADIO_2G4` | 厂商 2.4G RF_PHY 私有用例 | **低延迟 BLE 私有链路**（服务 0xFFF0，模组侧广播、接收器扫描连接） |

私有链路下行帧（接收器 → 模组）：`[0x01][LED]`、`[0x02][host_power]`。
上行帧（模组 → 接收器）：直接是 HID 报文本体（长度即类型）。

### 5.1 为什么不是厂商私有 2.4G

本厂商包**不包含**私有 2.4G 物理层实现，证据：

- `components/` 下只有 `ble_simple_central` 一个例程，无 `RF_PHY` / 私有 2.4G 例程；
- 闭源库 `libfr800x_stack.a` 的射频类导出符号只有
  `rf_init_app` / `rf_tx_test_with_payload` / `rf_tx_test_without_payload` /
  `rf_rx_test_config` / `rf_rx_test_stop` / `rf_tx_test_stop`，
  反汇编可见其最终调用 `lld_test_start`，属于 **BLE DTM 认证测试**（PRBS9），
  不能收发任意载荷；
- `gap_api.h` / `gatt_api.h` / `ble_stack.h` 中没有任何私有帧 TX/RX 接口。

因此 `BRIDGE_RADIO_2G4` 按「最低延迟的 2.4GHz 链路」实现：

| 参数 | 取值 | 作用 |
| --- | --- | --- |
| PHY | LE 2M（`gap_conn_phy_update`） | 空中速率翻倍，缩短单包占用 |
| 连接间隔 | 默认 6 × 1.25ms = 7.5ms；启用 double rate 后 1 × 1.25ms | 主/从两侧连上即请求 |
| Slave latency | 0 | 每个连接事件都收发 |
| 超时 | 4s | 掉线快速恢复 |
| 广播/扫描 | 20ms / 30ms | 快速重连 |
| ATT MTU | 128 | 单包容纳 64B 桥载荷 |
| 出站方式 | Notification / Write Without Response | 无 ATT 应答往返 |
| 发射功率 | `system_set_tx_power(RF_TX_POWER_POS_4dBm)` | 按 SDK Guide §4 取最大档 |

2.4G 模式下广播里**只带 0xFFF0**，不宣告 HOGP 0x1812，避免手机/PC 抢连。

#### 厂商射频增强接口（`FR800x-U_SDK_Guide.pdf`）

| 文档章节 | 接口 | SDK v2.2 是否导出 | 处理方式 |
| --- | --- | --- | --- |
| §3 | `set_start_freq_step(start, step)` | 是 | 按 2404.5/2.0 设频点表 |
| §4 | `system_set_tx_power()` / `_v2()` | 是 | 直接用最大档 |
| §9 | `disenable_conn_param_check()` + `double_rate_init()/double_rate_enable()` | 是 | 连接间隔可降到 **1.25ms** |
| §9 | `evt_time_min_slot_enable(true)` | 符号存在但内部引用未导出的 `lld_con_evt_time_update_patch` | **不调用**（否则链接失败） |

`port/fr2012a/bridge_fr2012a_sdk.c` 用弱符号声明这些接口，缺失时自动跳过；
`bridge_ble_fr2012a.c` 会在启用成功后把连接间隔从 7.5ms 压到 1.25ms，并在日志里打印
`fast response: enabled (1.25ms floor)`。

若后续拿到厂商 RF_PHY / 私有 2.4G 库，替换点只有
`port/fr2012a/bridge_ble_fr2012a.c`（链路层）与 `bridge_gatt_fr2012a.c`（承载），
`core/`、`port_radio_*`、`app/` 均不需要改。

## 6. 构建与产物

```bash
make keyboard CHIP=fr2012a       # 模组
make dongle   CHIP=fr2012a       # 接收器
make both     CHIP=fr2012a
make images   CHIP=fr2012a       # v1 无常驻 BOOT：只构建 App
```

| 产物 | 路径 |
| --- | --- |
| 模组 | `build/keyboard-fr2012a/bridge_keyboard.{elf,hex,bin}` |
| 接收器 | `build/dongle-fr2012a/bridge_dongle.{elf,hex,bin}` |

工具链默认 `arm-none-eabi-`（本机 `C:\ChibiStudio\tools\GNU Tools ARM Embedded\11.3 2022.08\bin`），
可用 `CROSS=/path/to/arm-none-eabi-` 覆盖。输出目录带 `-fr2012a` 后缀，
避免与 CH58x 的 `build/<target>/` 目标文件混用。

## 7. 当前能力边界（v1）

- 无 UART OTA / 无 BOOT 双区：`port_fw_*` 返回 `BRIDGE_ERR_UNSUPPORTED`，`make images` 不合并 BOOT。
- 无 BLE 多主机（`port_ble_slots` / `port_radio_connect_profile` 未实现）。
- 无 2.4G 定频/诊断（`port_radio_test_mode` / `port_radio_diag` 返回 `UNSUPPORTED`）。
- **无私有 2.4G 物理层**：`BRIDGE_RADIO_2G4` 由低延迟 BLE 链路承载（见 5.1）。
- 接收器 USB 报表沿用厂商描述符：8B 键盘报文在送入 EP2 前转换为 RID 8 的 15B NKRO 位图；
  多媒体为 RID 2、系统控制为 RID 1；鼠标未实现（与 CH58x BLE 通路一致）。
- 接收器断开后 1s 自动重新扫描；配对（`PAIR_START`）会清绑定并重新扫描/广播。

## 8. 待硬件验证项

1. 引脚分配（PA0/PA1 数据、PB2/PB3 控制、PA5 日志、PA4/PA5 USB）与 EVB 实际走线。
2. USB 标识运行期改写的实测：重枚举后 Windows 是否需要重新绑定驱动/是否保留 HID 句柄。
3. 私有链路实测延迟/丢包：7.5ms 间隔 + 2M PHY 是否稳定，主机侧是否接受该参数。
4. 休眠策略：v1 关闭 MCU 休眠（`system_sleep_disable()`），功耗待评估。
5. UART2（PA5，复用号 0x0B）日志输出的实测；若 EVB 无引出，可改回 UART1(PA2/PA3)。
6. 若确实需要非 BLE 的私有 2.4G 物理层，再索取 RF_PHY 例程/库；替换点同 5.1 末段。
