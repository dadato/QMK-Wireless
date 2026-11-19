# Tasks —— FR2012A 迁移到 SDK v2.2

对应规格：[spec.md](spec.md)

## T1 替换厂商 SDK
- [x] 新建 `wireless/vendor/FREQCHIP/FR8000U-SDK-v2.2/`
- [x] 复制 `components/`（排除 `modules/gui/`）
- [x] 复制 `examples/none_evm/`（排除 `*.dis *.map *.hex *.bin *.txt *.uvgui* Objects/`）
- [x] 复制 `tools/`
- [x] 删除 `Dongle-TGK621-V1.0.0/` 与 `Dongle-TGK621-V1.0.0.zip`
- 验证：新目录存在 `components/ble/library/libfr800x_stack.a`、`components/toolchain/gcc/ldscript.ld`

## T2 更新 `port/fr2012a/port.mk`
- [x] `FR_ROOT` 指向 `FR8000U-SDK-v2.2`
- [x] 源集合改为 `driver_pmu/driver_gpio/driver_uart_ex/driver_flash/co_log/boot_vectors`
- [x] dongle 改用 SDK `usb_core.c`/`usb_dev.c` + port `usb_hid_user.c`
- [x] 删除 `port/fr2012a/usb/usb_core.c`、`usb_dev.c`
- [x] 增加 `ble/profiles/ble_hid` 头路径
- 验证：`make keyboard CHIP=fr2012a` 能进入编译阶段

## T3 重写 SDK 原语（`bridge_fr2012a_sdk.c/.h`）
- [x] 数据面 UART1 → UART0，改名 `sdk_data_uart_*`，ISR `uart0_isr`
- [x] 时钟/复位/mux（PA0/PA1 func 0x04、RX 上拉）
- [x] 新增 `sdk_dbg_uart_init/write`：键盘 UART2 PA5 func 0x0B；接收器 UART0 PA1 func 0x04
- [x] `#define Uart2 ((struct_UART_t *)UART2_BASE)`
- [x] 清理 UART1 残留
- 验证：无 `Uart1`/`sdk_uart1_*` 引用

## T4 板级头引脚
- [x] keyboard：RX=A/0、TX=A/1、WAKE=B/2、NRESET=B/3、DBG=A/5、`BRIDGE_DBG_UART=2`
- [x] dongle：`BRIDGE_DBG_UART=0`，其余保持
- 验证：`port/fr2012a/bridge_port_fr2012a.c` 引用一致

## T5 端口层接线
- [x] `bridge_port_fr2012a.c`：`port_uart_*` → `sdk_data_uart_*`；`port_log_write` → `sdk_dbg_uart_*`
- [x] `bridge_usb_fr2012a.c`：去掉旧 LED/Suspend 全局，改用新回调 + 新增 LED 状态接口
- [x] `usb/usb_hid_user.c`：适配新 SDK（保留 FIFO 配置）；新增 HID SET_REPORT/EP0 DataOut 处理
- [x] `hid/hid_service.c`：对齐新 SDK 版本并保留 `bridge_fr_hid_out_report()`
- [x] `bridge_vendor_compat.c`：按新 `driver_flash.c` 调整 `flash_capacity`

## T6 配置
- [x] `app_config.h`：厂商 LOG 改 UART1(PA2/PA3)，其余保持
- [x] 核对 `ld/app.ld` 仍为 FLASH 256K / RAM 0xB000

## T7 构建与修复
- [x] `make keyboard CHIP=fr2012a` 通过
- [x] `make dongle CHIP=fr2012a` 通过
- [x] 检查 size 与关键符号（`user_main`、`uart0_isr`、无未定义）
- [x] host 测试保持通过

## T8 文档/技能同步
- [x] `docs/pinmap_fr2012a.md`
- [x] `.trae/skills/fr2012a-dev/SKILL.md` 及 references
- [x] `scripts/fr2012a.ps1` 中 SDK 路径（无需改，仅含烧录口说明）

## 完成标准
两条构建零 error、容量达标；引脚宏与实现一致；技能文档不再出现“UART1 数据面 / DBG=PA1 / 旧 SDK 路径”。
