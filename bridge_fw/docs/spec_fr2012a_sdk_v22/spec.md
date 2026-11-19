# FR2012A 迁移到厂商 SDK v2.2（FR8000U）—— 键盘模组 + 接收器

## 1. 目标与范围

把 `wireless/bridge_fw/port/fr2012a` 从旧 SDK（`Dongle-TGK621-V1.0.0`）整体迁移到新 SDK
`FR2012资料/sdk_v2.2/fr8000u-customer`，重写 FR201x-U 的键盘模组与接收器固件。

范围内：
- 删除旧 SDK（目录 + zip），拷入新 SDK，改 `port.mk`。
- 按 `board/keyboard/bridge_board.h` 顶部新注释重排键盘侧引脚，并改实现。
- 改用新 SDK 的 USB 栈、UART/Flash 驱动与 BLE 头文件；处理 API 破坏性变更。
- 同步 `docs/pinmap_fr2012a.md` 与 `.trae/skills/fr2012a-dev/` 中过时描述。

不做：模组 OTA、GUI/RTOS、私有 2.4G 物理层。

## 2. 已确认的事实（依据）

### 2.1 新 SDK 结构

`C:\Users\Administrator\Desktop\qmk\FR2012资料\sdk_v2.2\fr8000u-customer`
- `components/driver/`：`driver_pmu.c`、`driver_gpio.c`、`driver_uart_ex.c`、`driver_flash.c`、
  `usb_core.c`、`usb_dev.c`、`usb_hid.c` + `include/`、`drv_common/`、`components/`
- `components/driver_enhance/`：ADC10/TIMER/RGB/`driver_uart_ex.h`（含 `Uart2` 句柄）
- `components/ble/library/{libfr800x_stack.a,syscall_gcc.txt}`、`components/ble/profiles/ble_hid/hid_service.c`
- `components/toolchain/gcc/{boot_vectors.c,ldscript.ld}`
- `examples/none_evm/ble_hid_kbd_mice`（Keil）、`ble_AT/gcc/Makefile`、`peripheral_demo/usb_hid/gcc/Makefile`

新 SDK GCC 示例的编译集合（`usb_hid/gcc/Makefile` + `ble_AT/gcc/Makefile`）：
`driver_pmu.c driver_gpio.c driver_uart_ex.c driver_flash.c usb_core.c usb_dev.c usb_hid.c
co_log.c boot_vectors.c`，链接 `-T ldscript.ld -T syscall_gcc.txt libfr800x_stack.a`。
→ 说明新库不再内置 flash/uart/usb 驱动实现，必须随工程编译。

`syscall_gcc.txt` 仍为 `__initial_sp = 0x1100c000`；新 `ldscript.ld` 写 `LENGTH = 0xD000`
会越过栈顶，因此**保留本工程 `ld/app.ld` 的 `RAM LENGTH = 0xB000`**。

### 2.2 引脚复用（FR201x-U 参考手册 附录 I，pp.249-252）

| 功能号 | 用途 |
|---|---|
| 0x04 | UART0_Rx / UART0_Tx |
| 0x05 | USB_DP / USB_DM（PA0/PA1/PA4/PA5/PB0/PB1/PB4/PB5） |
| 0x0B | UART2_Rx / UART2_Tx（PA0/PA1/PA4/PA5、PB0/PB1/PB4/PB5、PD0/PD1/PD4/PD5） |

→ PA5 作 UART2_TXD 用功能号 **0x0B**；基版 `driver/include/driver_uart_ex.h` 只定义
`Uart0/Uart1`，`Uart2` 仅在 `driver_enhance` 中，故在 `bridge_fr2012a_sdk.c` 内补一行
`#define Uart2 ((struct_UART_t *)UART2_BASE)`（`UART2_BASE/UART2_IRQn` 已在 `plf.h`）。

### 2.3 新 SDK 的破坏性差异

1. USB：`usb_device_init()` 只做时钟/PHY/中断配置，**不配置数据端点 FIFO**
   （FIFO 配置在 `usb_host_init()` 里），因此 `usb_hid_init()` 仍需自行配置 EP1..EP3；
   `usb_dev.c` 不再定义 `f_LedStatusChange/LedStatus/f_Suspend`，改为
   `USB_Suspend_Handler/USB_Resume_Handler/Endpoint_0_ClassRequest_Handler/
   Endpoint_0_DataOut_Handler` 回调。
2. `boot_vectors.c` 新增 `uart2/adc/lvd/cali/wdt` 向量与 `SVC_Handler`。
3. `hid_service.c`（新）≈ 现有 port 版（相似度 0.97），差异仅为桥接钩子与空白；
   port 版继续保留，按新 SDK 内容重新对齐并保留 `bridge_fr_hid_out_report()` 弱钩子。
4. `driver_pmu.c` 需要 `flash_capacity`；新 `driver_flash.c` 参与编译，需核对其是否已定义，
   避免与 `bridge_vendor_compat.c` 重复定义。

## 3. 目标引脚（权威：`board/keyboard/bridge_board.h#L7-11`）

### 键盘模组
| 信号 | 引脚 | 复用/方向 |
|---|---|---|
| SK32 TX → 模组 | PA0 | UART0_RXD，func 0x04，上拉 |
| 模组 → SK32 RX | PA1 | UART0_TXD，func 0x04 |
| WAKE | PB2 | GPIO 输出，高有效，初值低 |
| nRESET | PB3 | GPIO 输入上拉，低有效 |
| 调试日志 TX | PA5 | UART2_TXD，func 0x0B，仅 TX |

> PA0/PA1 同时是 ROM 串口烧录口（FREQCHIP_Download TX→PA0、RX→PA1），烧录时需与 SK32 隔离。

### 接收器（不变）
USB DP=PA4 / DM=PA5（func 0x05）、LED=PB1、PAIR=PB0、调试 TX=PA1（UART0，func 0x04）。

## 4. 实施方案

### 4.1 SDK 目录
- 新增 `wireless/vendor/FREQCHIP/FR8000U-SDK-v2.2/`，从新 SDK 复制：
  - `components/`（**排除** `components/modules/gui/`，LVGL 约 194MB，键盘不用）
  - `examples/none_evm/`（排除 `*.dis *.map *.hex *.bin *.txt *.uvgui* Objects/`）
  - `tools/`
  - 不复制 `docs/*.pdf`（原始资料已在 `FR2012资料`）
- 删除 `wireless/vendor/FREQCHIP/Dongle-TGK621-V1.0.0/` 与 `Dongle-TGK621-V1.0.0.zip`。
- 旧 `FR201x-U资料打包`/`SK32...` 目录与本任务无关，不动。

### 4.2 `port/fr2012a/port.mk`
- `FR_ROOT ?= $(ROOT)/../vendor/FREQCHIP/FR8000U-SDK-v2.2`
- `CHIP_SDK_SRC` 改为：`driver_pmu.c driver_gpio.c driver_uart_ex.c driver_flash.c
  co_log.c boot_vectors.c`
- dongle：`$(FR_COMP)/driver/usb_core.c $(FR_COMP)/driver/usb_dev.c
  $(ROOT)/port/fr2012a/usb/usb_hid_user.c`
  （删除 `port/fr2012a/usb/usb_core.c`、`usb_dev.c`）
- keyboard：`$(ROOT)/port/fr2012a/hid/hid_service.c`
- `CHIP_SDK_INCS` 保持新 SDK 目录；增加 `$(FR_COMP)/ble/profiles/ble_hid`
- `CHIP_CDEFS`/`CHIP_LDFLAGS`/`ld/app.ld` 不变。

### 4.3 `bridge_fr2012a_sdk.{c,h}` —— UART 重写
- 数据面从 UART1 改为 **UART0**：
  - `sdk_uart1_mux/init/write_byte/tx_empty/irq_enable` → `sdk_data_uart_*`
  - mux：RX/TX func 0x04，RX 上拉
  - 时钟：`__SYSTEM_UART0_CLK_ENABLE()` + `__SYSTEM_UART0_CLK_SELECT_96M()`，
    `system_regs->rst.uart0_rst = 0`
  - ISR：`uart0_isr`（`UART0_IRQn` 9），逻辑同原 `uart1_isr`
- 调试口改为按板选择：
  - 新增 `sdk_dbg_uart_init(baud)` / `sdk_dbg_uart_write(d,l)`，根据板级 `BRIDGE_DBG_UART`
    （2=键盘 PA5 func 0x0B；0=接收器 PA1 func 0x04）初始化 UART2/UART0
  - 删除 `sdk_uart0_init/sdk_uart0_write` 的对外语义（改为内部实现）
- 补 `#define Uart2 ((struct_UART_t *)UART2_BASE)`（`#ifndef` 保护）
- GPIO 接口不变（WAKE/NRESET 由板级宏决定）
- `sdk_sys_init()` 的 `__SYSTEM_UART1_CLK_ENABLE()` 删除/替换为 UART0（调试口按需开启）

### 4.4 板级头
- `board/keyboard/bridge_board.h`：宏改为
  `RX=A/0`、`TX=A/1`、`WAKE=B/2`、`NRESET=B/3`、`DBG_TX=A/5`、`BRIDGE_DBG_UART=2`；
  注释保留“PA0/PA1 为 ROM 烧录口”的警示。
- `board/dongle/bridge_board.h`：补 `BRIDGE_DBG_UART=0`，调试引脚保持 A/1；
  UART 数据宏保留占位（注释标明接收器无 SK32 串口）。

### 4.5 端口层
- `bridge_port_fr2012a.c`：`port_uart_*` 改调 `sdk_data_uart_*`；
  `port_log_write()` 改调 `sdk_dbg_uart_init/write`；日志文本 UART1→UART0。
- `bridge_usb_fr2012a.c`：
  - 删除 `extern f_LedStatusChange/LedStatus/f_Suspend`；
  - LED：由 `usb_hid_user.c` 在 HID `SET_REPORT`（RID 8，1 字节）时写入
    `usb_hid_led_state()`/`usb_hid_take_led_change()`（port 内新增的小接口），
    poll 中取变化值后 `bridge_fr_ble_send_led()`；
  - 挂起/唤醒：注册 `USB_Suspend_Handler`/`USB_Resume_Handler`，维护
    `s_usb_suspended`，poll 中变化时 `bridge_fr_ble_send_host_power()`；
  - `usb_device_init()` 后不再重复配置 EP FIFO（交给新 SDK）。
- `usb/usb_hid_user.c`：适配新 SDK
  - 保留自定义复合描述符（键盘 15B NKRO RID8 / 消费者 RID2 / 系统 RID1 / OTA RID3-4）；
  - `usb_hid_init()` 去掉与新 SDK `usb_device_init()` 重复的 FIFO 配置；
  - 增加 `Endpoint_0_ClassRequest_Handler`：`HID_SET_REPORT` 记录 RID/长度，
    `Endpoint_0_DataOut_Handler` 读 EP0 数据 → 更新 LED 状态；
  - 删除对旧 `usb_dev.c` 内部变量的依赖。
- `hid/hid_service.c`：以新 SDK `ble/profiles/ble_hid/hid_service.c` 为准重新覆盖 port 版，
  末尾保留 `bridge_fr_hid_out_report()` 弱钩子（供键盘 LED 上报）。
- `bridge_vendor_compat.c`：若新 `driver_flash.c` 已定义 `flash_capacity`，删除本文件里的定义；
  `master_write_data/show_reg` 空实现保留（先编译验证是否需要）。

### 4.6 配置
- `app_config.h`：
  - 时钟保持 96MHz（`SYSTEM_CLOCK_SEL 5`）；
  - 厂商 LOG 改为 `LOG_OUTPUT_UART1`（PA2/PA3），避免任何库路径调用 `log_init()` 时占用
    UART0(PA0/PA1) 与 UART2(PA5)；本工程日志不走厂商 LOG；
  - Flash/协议栈参数保持。

### 4.7 文档同步
- `docs/pinmap_fr2012a.md`：更新为 PA0/PA1=UART0 数据、PA5=UART2 日志、PB2/PB3。
- `.trae/skills/fr2012a-dev/SKILL.md` 与 `references/{sdk-api,build-flash,project-port}.md`：
  改 SDK 路径/版本、构建源集合、UART/引脚描述、烧录口说明。
- `.trae/skills/fr2012a-dev/scripts/fr2012a.ps1`：如引用了旧 SDK 路径则同步。

## 5. 验证

工作目录 `wireless/bridge_fw`，工具链：
`C:\ChibiStudio\tools\GNU Tools ARM Embedded\11.3 2022.08\bin` + `D:\QMK_MSYS\usr\bin`。

1. `make keyboard CHIP=fr2012a` 通过（无 error），检查 `.text/.data/.bss` 在
   256KB Flash / 0xB000 RAM 内。
2. `make dongle CHIP=fr2012a` 通过，同样检查容量。
3. 符号自检：
   - `arm-none-eabi-nm` 确认 `uart0_isr` 为强符号、`user_main` 存在；
   - 确认无 `Uart1` 残留引用、无旧 `f_LedStatusChange` 未定义。
4. `make -C test_host`（或现有 host 测试入口）保持通过。
5. 硬件（用户执行）：FREQCHIP_Download 经 PA0/PA1 烧录；键盘模组 UART0 与 SK32 互通，
   PA5 输出日志；WAKE(PB2)/nRESET(PB3) 电平符合预期。

## 6. 风险与回退

- UART2 未在基版头文件暴露：用 `UART2_BASE` 直连，功能号 0x0B 已由参考手册确认；
  若实测无输出，回退方案是把日志改到 UART1(PA2/PA3)（不影响数据面）。
- PA0/PA1 复用为烧录口：量产烧录需断开局端串口，文档中标注。
- 新 USB 栈行为变化：若 `usb_hid_user.c` 与新栈枚举不兼容，保留旧 `usb_core/usb_dev`
  作为临时回退（单独提交，不混入本次迁移）。
