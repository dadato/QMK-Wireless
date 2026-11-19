# CH582F 管脚分配与设计决策（v1.4）

> 配套文档：[bridge_fw_spec.md](./bridge_fw_spec.md) · [bridge_impl_checklist.md](./bridge_impl_checklist.md)
>
> 本文档记录已拍板的设计决策，并给出**可直接用于 PCB 设计的管脚分配表**。
> 引脚号依据官方《CH583/CH582/CH581 数据手册》V1.8 第 1.2 节引脚描述表。

---

## 0. 设计决策记录（已定）

| # | 事项 | 决策 |
|---|---|---|
| D1 | 芯片封装 | **CH582F（QFN28_4x4，20 GPIO）**，键盘侧与 Dongle 侧同型号 |
| D2 | 桥↔SK32 串口 | **UART1：PA8=RXD1 / PA9=TXD1** |
| D3 | NKRO over BLE | **禁用** |
| D4 | NKRO over 2.4G | **保留**（自有协议，16B 直传） |
| D5 | 2.4G 上报率 | **1000 Hz（1 ms）** |
| D6 | BLE 广播名 | **`SK32-3M-XXXX`**（XXXX = MAC 低 16 位十六进制大写） |
| D7 | Dongle 主控 | **CH582F** |
| D8 | 外挂 Flash | **不使用**，升级仅靠片内 448KB CodeFlash |
| D9 | SRRC | **需支持**，固件须提供定频测试模式 |
| D10 | 触摸按键 | **不使用** |
| D11 | 2.4G 实现 | **直接采用厂商 2.4G 用例**，不自研射频私有协议；Dongle 用配套接收端用例 |
| D12 | OTA 方案 | **常驻 BOOT + Stage 暂存**（不划分 A/B 双应用区）：`BOOT@0x0(8K) + APP_A@0x2000(216K) + Stage@0x38000(216K)`；APP_A 搬运前不动，可自动重试/回退；ROM ISP 兜底 |
| D13 | printf 日志 | **可配置开关**，输出走 UART0（PB4/PB7），量产默认关闭 |
| D14 | 桥串口波特率 | **921600**（8N1；原 38400，后统一提升）；见规格 4.1：键盘帧占线 ≈0.17ms，HID 仍事件驱动、帧间隔 ≥5ms |
| D15 | 32K 低速时钟源 | **外部 32.768kHz LSE 晶振**（PA10=X32KI / PA11=X32KO，**已实装**）。固件以 `CLK_OSC32K=0` 选外部 LSE，作为 RTC / TMOS 时基 / WCH 硬件跳频同步的基准（**更正**：早期文档误记该晶振为「可选未接」） |

---

## 1. CH582F 可用资源速查

| 项 | 值 |
|---|---|
| 封装 | QFN28_4x4（+ 底部散热焊盘 EPAD） |
| GPIO | **20 个**：PA8、PA9、PB15、PB14、PB13、PB12、PB11、PB10、PB7、PB4、PB23、PB22、PA4、PA5、PA15、PA14、PA13、PA12、PA11、PA10 |
| Flash | 448KB CodeFlash + 32KB DataFlash + 24KB BootLoader + 8KB InfoFlash |
| SRAM | 30KB + 2KB（睡眠保持） |
| ADC | 8 路外部通道（可用 AIN：AIN0/1/2/3/4/5/12/13） |
| UART | 可用 UART0（PB4/PB7）、UART1（PA8/PA9）、UART3（PA4/PA5）；UART2 只能走映射脚（PB22/PB23） |
| USB | USB1（PB10/PB11）、USB2（PB12/PB13） |
| 最低电压 | 2.3V |
| QFN28 上**不存在** | PA0~PA3、PA6、PA7、PB0~PB3、PB5、PB6、PB8、PB9、PB16~PB21 |

---

## 2. UART 选型说明（D2）

QFN28 上可用的串口逐一排除：

| 候选 | 引脚 | 结论 |
|---|---|---|
| **UART1** | PA8 / PA9（pin 6 / 7） | **采纳**。两脚均为纯 GPIO+UART，不复用复位/USB/晶振，可整组占用 |
| UART0 | PB4 / PB7（pin 15 / 14） | 保留作调试 printf（D13，921600） |
| UART2 | 仅映射脚 PB22 / PB23（pin 17 / 16） | **排除**：PB23 是 `RST#`（外部复位输入），不可占用 |
| UART3 | PA4 / PA5（pin 23 / 24） | 备选。若 UART1 需让位可整体切到此处 |

结论：**桥↔SK32 链路走 UART1（PA8/PA9），921600 8N1，无流控**（D14）。
注意带宽/帧间隔约束：HID 报文必须事件驱动发送、两帧间隔 ≥5ms，详见规格 4.1 与 6.3。

---

## 3. 键盘侧（桥）管脚分配

### 3.1 信号分配表

| 功能 | 网络名 | 引脚 | 复用功能 | QFN28 Pin | 方向（相对桥） |
|---|---|---|---|---|---|
| 串口接收 | BR_RX | PA8 / RXD1 | UART1 | **6** | 输入（来自 SK32 TX） |
| 串口发送 | BR_TX | PA9 / TXD1 | UART1 | **7** | 输出（送往 SK32 RX） |
| 唤醒请求 | WAKE | PA5 | GPIO | **24** | 输出，高有效 |
| 硬复位 | nRESET | PB23 | **RST#** | **16** | 输入，低有效，内置上拉 |
| 调试时钟 | TCK | PB15 | 两线仿真 | **8** | — |
| 调试数据 | TIO | PB14 | 两线仿真 | **9** | — |
| 量产 ISP 配置 | ISP_CFG | PB22 | GPIO | **17** | 输入，**10k 上拉** |
| 调试串口（printf 日志，仅 TX） | DBG_TX | PB7 | UART0 | **14** | 日志输出（D13），量产固件可关闭日志但**建议保留焊盘** |

> **v1.4 已裁剪**：`MODE_SEL0/1`（PB12/PB13）、`LINK`（PA13）、`BAT_ADC`（PA14）、
> `CHRG_STAT`（PA15）、`DBG_RX`（PB4）全部取消。工作模式上电缺省 USB，由 SK32 经
> `SET_MODE` 命令下发；电量与充电状态一律由 `SET_BATTERY(0x1C)` 命令输入，模组不做本地采样。

### 3.2 专用引脚（非 GPIO）

| 功能 | 引脚 | QFN28 Pin | 说明 |
|---|---|---|---|
| 32MHz 晶振 | X32MO / X32MI | **18 / 19** | 必需，±10 ppm |
| 32.768kHz 晶振（LSE，**已实装**） | PA11=X32KO / PA10=X32KI | **1 / 2** | **必需**：RTC / TMOS 时基 / WCH 硬件跳频同步基准；固件用 `CLK_OSC32K=0` 选外部 LSE（D15） |
| 天线 | ANT | **21** | 50Ω 直连天线 |
| 电源 | VDCID / VSW / VDD33+VIO33 | **3 / 4 / 5** | 见 3.3 |
| 模拟电源 | VINTA / VDCIA | **20 / 22** | 退耦电容必需 |
| 地 | GND / EPAD | **0** | 底部焊盘必须接地 |

### 3.3 电源引脚外围（依据数据手册 1.2 节）

| 引脚 | 外围 |
|---|---|
| VDCID（pin 3） | 启用 DC-DC 时 2.2µF（0.47~4.7µF）；不启用时不小于 0.1µF |
| VSW（pin 4） | 启用 DC-DC 时贴近引脚串 **10µH** 电感（3.3~33µH）到 VDCID；不启用时直连 VDCID |
| VDD33 + VIO33（pin 5） | 贴合引脚退耦，启用 DC-DC 时 2.2µF 或 1µF，可共用同一电容 |
| VINTA（pin 20） | 启用 DC-DC 时 1µF（0.47~2.2µF） |
| VDCIA（pin 22） | ≥0.1µF，建议直连 VDCID |

> 官方社区反馈：DC-DC 输出电容（VIO33、VSW 的 2.2µF）**缺任一个都可能导致蓝牙频繁掉线**，不可省略。

### 3.4 引脚占用统计

| 类别 | 数量 |
|---|---|
| 已分配 GPIO | 7（BR_RX/TX、WAKE、nRESET、TCK、TIO、DBG_TX） |
| 保留（ISP） | 1（ISP_CFG） |
| 专用（非 GPIO，已实装） | 2（PA10/PA11，32.768kHz LSE 晶振，D15） |
| 空闲（统一推挽输出低） | **9**（PA12、PA13、PA14、PA15、PB4、PB10、PB11、PB12、PB13；PA10/PA11 归 LSE） |

---

## 4. Dongle 侧（CH582F）管脚分配

| 功能 | 网络名 | 引脚 | 复用 | QFN28 Pin | 说明 |
|---|---|---|---|---|---|
| USB 数据负 | USB_DM | PB10 / UD- | USB1 | **13** | 差分对 90Ω |
| USB 数据正 | USB_DP | PB11 / UD+ | USB1 | **12** | 芯片内置 FS 上拉 |
| 状态指示 | LED_STAT | PA4 | GPIO | **23** | 配对/连接指示 |
| 配对按键（可选） | PAIR_KEY | PA5 | GPIO | **24** | 长按重新配对 |
| 调试 | TCK / TIO | PB15 / PB14 | 两线仿真 | **8 / 9** | 预留测试点 |
| 调试串口 | DBG_TX | PB7 | UART0 | **14** | printf 日志（仅 TX） |
| 量产 ISP 配置 | ISP_CFG | PB22 | GPIO | **17** | 10k 上拉 |
| 32MHz 晶振 | X32MO / X32MI | — | — | **18 / 19** | ±10 ppm |
| 天线 | ANT | — | — | **21** | 50Ω |
| 电源 | VDCID / VSW / VDD33+VIO33 | — | — | **3 / 4 / 5** | 同 3.3 |
| 模拟电源 | VINTA / VDCIA | — | — | **20 / 22** | 同 3.3 |
| 地 | GND / EPAD | — | — | **0** | — |

Dongle 侧 GPIO 占用极少，未使用引脚（含 PB4/PA13/PA14/PA15/PB12/PB13）统一配置为推挽输出低
（PA10/PA11 已被 32.768kHz LSE 占用，见 D15）。

---

## 5. SRRC 认证设计要求（D9）

### 5.1 频段与功率

- 工作频段：**2400–2483.5 MHz**（BLE 与 2.4G 私有模式均在此段）
- 芯片 TX 功率最大可编程 **+6 dBm**
- 产品实际功率档位须固定，并满足现行《微功率短距离无线电发射设备目录和技术要求》对 2.4GHz 设备的 EIRP 限值（通常 ≤ 10 dBm 量级）——**最终限值以现行法规与认证机构确认为准**
- 软件**不得**开放超出申报值的功率档

### 5.2 固件必须新增：定频测试模式

SRRC/CE 认证测试要求设备能在固定频率、固定功率下持续发射。桥固件**必须**提供：

| CMD | 名称 | Payload | 说明 |
|---|---|---|---|
| `0x0A` | `GET_MAC` | — | 读芯片唯一 MAC（用于广播名生成与配对） |
| `0x0B` | `SET_TEST_MODE` | mode(1B), channel(1B), power(1B) | **认证专用**：mode=1 定频连续发射，mode=0 退出；channel 指定信道，power 指定功率档 |

Dongle 侧同理，需能进入定频模式（键盘与 Dongle 组成一套系统申报）。

> **与 D11 的关系**：2.4G 链路直接采用厂商用例（D11），因此 2.4G 侧的定频能力**取决于厂商用例是否暴露该接口**。
> BLE 侧定频可用 SDK 标准测试接口实现；若厂商 2.4G 用例不支持定频，认证阶段的 2.4G 发射测试需由 WCH 产测固件配合（见规格 8.5）。

### 5.3 射频硬件要求

| 项 | 要求 |
|---|---|
| 天线 | PCB 板载 IFA/倒 F 或 IPEX；**50Ω 阻抗控制**；天线净空区无铜、无走线、无器件（建议 ≥5mm） |
| 天线增益 | PCB 天线通常 0~2 dBi，须在申报材料中说明 |
| 32MHz 晶振 | **±10 ppm**（频率容限测试的关键），负载电容按晶振规格匹配 |
| 电源退耦 | VDD33 / VIO33 / VINTA / VDCID 退耦电容必须齐全，否则蓝牙易掉线并影响测试稳定性 |
| DC-DC | 10µH 电感；开关噪声不得落入 2.4GHz 接收带，布局远离 RF 通路 |
| 布局 | RF 走线尽量短，天线下方完整地平面，避免跨分割；UART 等数字走线远离 ANT 与晶振 |

### 5.4 认证范围

- SRRC 为**整机型号核准**；键盘与 Dongle 均为 2.4G 发射设备，须一并纳入申报（可作一套系统申报，具体以认证机构要求为准）
- **BLE 与 2.4G 私有两种模式都要覆盖测试**

---

## 6. 未使用引脚处理

- 未使用 GPIO 一律配置为**推挽输出低**（由 `port_gpio_init()` 统一处理），避免悬空输入漏电、受干扰影响待机电流
- **键盘侧**未使用清单：PA12、PA13、PA14、PA15、PB4、PB10、PB11、PB12、PB13
  （其中 PB12/PB13 为原 MODE_SEL、PA13 原 LINK、PA14 原 BAT_ADC、PA15 原 CHRG_STAT、PB4 原 UART0 RX）
- **Dongle 侧**未使用清单：PA8、PA9（无 UART1 数据面）、PA12、PA13、PA14、PA15、PB4、PB12、PB13
- `ISP_CFG`（PB22）必须 **10k 上拉**，确保上电不误入 BootLoader；量产治具/恢复模式下拉至低
- TCK/TIO（PB14/PB15）为两线仿真（SDI 的 TIO/TCK）调试口，**严禁改写为输出**，保持默认
- PB23 为 `RST#`（外部复位输入），非 GPIO，**不得驱动**
- 32.768kHz 晶振（LSE）**已实装**：PA10/PA11 固定接 X32KI/X32KO，固件以 `CLK_OSC32K=0` 选外部 LSE，**严禁**再配置为 GPIO 输出

---

## 7. 与 SK32 的连线表

### 7.1 SK32 侧可用引脚核实（已核对现有板级配置）

SK32F077 的 ChibiOS 串口 LLD 只实现了 **USART1 / USART2**（见 `hal_serial_lld.h`），端口方面有 GPIOA/B/C/D + GPIOF。两份现存板级配置已占用：

| 板 | 已占用引脚 | 来源 |
|---|---|---|
| `keyboards/sk32/kb17` | PB0 PB1 PB2 PB10 PB11 PB12 PB13 / PC0 PC4 PC5 / PA11 PA12（USB）/ PA13 PA14（SWD） | [config.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/keyboards/sk32/kb17/config.h#L7-L14) |
| `keyboards/sk32/ld7_oled` | PB0 PB1 PB2 PB10 PB11 PB12 / PC0 PC5 / **PA2 PA3（OLED I2C1）** / PA11 PA12（USB）/ PA13 PA14（SWD） | [config.h](file:///c:/Users/Administrator/Desktop/qmk/qmk_firmware/keyboards/sk32/ld7_oled/config.h#L7-L23) |

结论：

- **USART2 排除**：其默认脚 PA2/PA3 在 `ld7_oled` 上被 OLED I2C1 占用
- **USART1 采纳**：默认脚 **PA9（TX）/ PA10（RX）** 在两块板上均空闲，且不与 USB（PA11/PA12）、SWD（PA13/PA14）、矩阵、RGB（PC0）、OLED（PA2/PA3）冲突

### 7.2 连线表

| 网络 | SK32 侧 | 桥侧（CH582F） | 方向 | 电平 |
|---|---|---|---|---|
| BR_TX | **PA9 / USART1_TX** | PA8 / RXD1（pin 6） | SK32 → 桥 | 3.3V |
| BR_RX | **PA10 / USART1_RX** | PA9 / TXD1（pin 7） | 桥 → SK32 | 3.3V |
| WAKE | SK32 EXTI 输入（建议 PB6） | PA5（pin 24） | 桥 → SK32 | 3.3V，高有效 |
| nRESET | SK32 GPIO（建议 PB8） | PB23（pin 16） | SK32 → 桥 | 3.3V，低有效 |
| VBUS_DET | SK32 检测 | — | — | 桥不参与 |

> 说明：工作模式不再使用硬件 `MODE_SEL`，桥侧上电缺省 USB，由 SK32 经 UART `SET_MODE` 命令下发；
> 电量/充电状态经 `SET_BATTERY` 命令下发。WAKE/nRESET 用的是 PA9/PA10 之外的**空闲 GPIO 建议值**，
> 投板前须按最终矩阵/扩展器件再复核。
> 若 SK32 侧另需 SWD 调试，注意 PA13/PA14 不要改作他用（现有两块板即如此）。

### 7.3 SK32 侧固件改动（QMK）

- 启用 `SK32_SERIAL_USE_USART1 = TRUE`（`mcuconf.h`），**921600 8N1**（D14），并确认 PCLK 满足该波特率精度
- 新增自定义 `host_driver_t`，经 USART1 把 `send_keyboard` / `send_nkro` / `send_consumer` / `send_mouse` / `send_system` 映射为 0x10~0x14 报文（模板见 `keyboards/annepro2/annepro2_ble.c` 的 `host_set_driver()` 用法）
- **链路强制约束**：QMK 侧必须做「相同报文抑制 + 发送串行化 + 两帧最小间隔 ≥5ms」；QMK 默认已在键值变化时才调用 `send_keyboard`，但**层切换/修饰键同步**可能产生连发，需自行做 last-report 比较与节流
- 无线模式下必须真正断开 USB，避免主机枚举出两个 HID 键盘

> 波特率取舍说明：曾选用 38400 换取更好的抗干扰与走线容忍度，但单帧占线 4.17ms、带宽仅 3840 B/s。
> 现统一提升至 **921600**（单帧占线 ≤0.26ms、带宽 92160 B/s），抗干扰需求靠 PCB 走线保证；
> 仍保留「事件驱动 + 两帧间隔 ≥5ms」约束。


---

## 8. 因决策变更的固件需求

| 变更 | 影响 |
|---|---|
| D3 禁用 NKRO over BLE | 桥在 BLE 模式收到 16B 报文时**降级为 6KRO**；2.4G 模式直传 16B |
| D5 上报率 1ms | `BRIDGE_RF24_REPORT_HZ = 1000`；功耗预算按 1ms 上报重算 |
| D6 广播名规则 | 首次上电读 MAC，生成 `SK32-3M-XXXX`；需新增 `GET_MAC` 命令 |
| D8 + D12 无外挂 Flash、常驻 BOOT OTA | 升级区**不划分 A/B 双应用区**（`BOOT 8K + APP_A 216K + Stage 216K`），应用可用 **216KB**；APP_A 搬运前不动、断电自动重试、取消/超时可回退；ROM ISP（`ENTER_BOOTLOADER`/PB22 + WCHISPTool）为最终兜底 |
| D9 SRRC | 新增 `SET_TEST_MODE` 定频命令；功率档位固定，不得软件超频 |
| D10 无触摸 | 移除 TouchKey 相关初始化与引脚规划 |
| D11 2.4G 用厂商用例 | `bridge_rf24.c` 退化为**厂商链路的薄封装**（init / send / 回调转事件），不自研协议；上报周期与定频能力受厂商用例约束 |
| D13 printf 日志开关 | 新增 `bridge_log.c/h`；日志走 UART0（PB4/PB7），**禁止**占用 UART1；产出 debug / release 两个构建目标 |
