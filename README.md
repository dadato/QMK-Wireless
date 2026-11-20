# QMK-Wireless

面向 QMK 键盘的**无线桥（Wireless Bridge）固件**：以 CH582F / FR2012A 作为无线协处理器，通过 UART 接收主控（如 SK32F077）的 HID 报文，经 **BLE / 2.4G** 转发给主机；配套 USB 接收器（Dongle）、主控侧客户端库与浏览器调试/OTA 工具。

## 功能

- 键盘侧模组与 USB Dongle 共用同一套 `core/` 协议栈
- BLE / 2.4G 建链、配对与多主机切换，HID 键盘、Consumer、NKRO 报文转发
- 连接状态、HID LED、电量事件上报；1000 ms 心跳与断链检测
- 常驻 BOOT + 双区 OTA（Web 升级页）
- 可配置的空闲/深睡低功耗策略
- 芯片无关分层：换无线芯片只需新增 `port/<chip>/`

## 支持芯片

| 芯片 | 内核 | 适配层 | 工具链 |
|---|---|---|---|
| CH582F / CH583 | RISC-V | `bridge_fw/port/ch58x` | `riscv-wch-elf-`（MounRiver / WCH GCC） |
| FR2012A（FR800x） | Cortex-M3 | `bridge_fw/port/fr2012a` | `arm-none-eabi-`（Arm GNU Toolchain） |

两个适配层均支持键盘侧模组与 USB Dongle 两种角色；无线通路目前 CH58x 提供 BLE + 2.4G，FR2012A 提供 BLE。

## 目录结构

```text
bridge_fw/
  hal/            芯片无关硬件抽象接口（bridge_port.h）
  core/           协议 / 会话 / HID / OTA / 配置 / 电源 / 模式
  port/<chip>/    芯片适配层（SDK 路径、源文件集合、BOOT 镜像）
  app/            keyboard（模组）与 dongle（接收器）板级定义与链接脚本
  boot/<chip>/    常驻 BOOT（OTA 搬运 / 收包恢复）
  docs/           功能规格、协议、引脚与时序文档
  tools/          sk32sim 协议模拟器、bench、日志
  test_host/      主机侧单元测试桩
  web/debug       Web 调试控制台
  web/ota         Web OTA 升级页
bridge_fw_client/ 主控侧（SK32 / QMK）客户端库：帧编解码、会话、键码映射
reference/        三模键盘方案参考
vendor/           厂商 SDK 与参考物料（默认不入库）
```

## 快速开始

依赖：GNU make、Python 3。CH58x 需 WCH / MounRiver RISC-V 工具链；FR2012A 需 Arm GNU Toolchain。

```bash
git submodule update --init          # 拉取 openwch/ch583 SDK

cd bridge_fw
make keyboard                        # 键盘侧模组（默认 CHIP=ch58x，日志开启）
make dongle                          # USB 接收器
make both                            # 两个目标
make keyboard LOG=0 SLEEP=1          # 量产：移除日志代码、允许休眠
make images                          # BOOT + APP 合并为整机 hex
make keyboard CHIP=fr2012a           # 切换到 FR2012A 适配层
```

常用参数：`TARGET=keyboard|dongle`、`CHIP=ch58x|fr2012a`、`LOG=0|1`、`SLEEP=0|1`（`SLEEP=0` 用于台架调试，避免休眠停 CPU 时钟后调试口失联）。

产物位置：

- 单镜像：`build/{keyboard,dongle}/bridge_*.hex`
- `LOG=0`：`build/{keyboard,dongle}-release/`
- `SLEEP=0`：`build/{keyboard,dongle}-nosleep/`
- 整机合并：`build/boot/bridge_keyboard_full.hex`、`build/boot/bridge_dongle_full.hex`

FR2012A 构建前需将厂商 SDK 放到 `vendor/FREQCHIP/FR8000U-SDK-v2.2`（该目录默认被忽略）。

## 文档

- [协议详解](bridge_fw/docs/bridge_protocol.md)
- [功能规格](bridge_fw/docs/bridge_fw_spec.md)
- [实现清单](bridge_fw/docs/bridge_impl_checklist.md)
- [CH58x 引脚定义](bridge_fw/docs/pinmap_ch58x.md)
- [FR2012A 引脚定义](bridge_fw/docs/pinmap_fr2012a.md)

UART 帧格式：`AA 55 | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16`，CRC-16/CCITT-FALSE，38400 8N1。

## 相关

- 键盘固件：[dadato/qmk_firmware](https://github.com/dadato/qmk_firmware)（`feat/sk32-wireless`）
- SK32 ChibiOS HAL：[ChibiOS-Contrib PR #452](https://github.com/ChibiOS/ChibiOS-Contrib/pull/452)（已合并）

## 说明

个人项目，按现状提供。仓库内的厂商 SDK 与参考物料遵循其原始许可，且默认不纳入版本库；`bridge_fw/port/ch58x/sdk/ch583` 为 `openwch/ch583` 子模块。
