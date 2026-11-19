# ===========================================================================
# FR2012A（FreqChip FR800x，Cortex-M3）芯片适配层构建描述
#
# 由顶层 Makefile 在定义 ROOT / space / rwildcard 之后 include。
# 与其他芯片一样，本文件是移植时需要新增的唯一构建文件。
#
# 工具链：arm-none-eabi-（Arm GNU Toolchain，本机默认装在
#   C:\ChibiStudio\tools\GNU Tools ARM Embedded\11.3 2022.08\bin）
#   命令行覆盖示例：make keyboard CHIP=fr2012a CROSS=/d/gcc-arm/bin/arm-none-eabi-
#
# 厂商 SDK 目录（可覆盖）：wireless/vendor/FREQCHIP/FR8000U-SDK-v2.2
#   components/driver            外设驱动 + USB 栈（driver_pmu/gpio/uart_ex/flash、
#                                usb_core/usb_dev/usb_hid），随工程编译
#   components/ble/library       libfr800x_stack.a + syscall_gcc.txt（符号地址表）
#   components/ble/profiles      厂商 GATT 服务（hid_service）
#   components/toolchain/gcc     启动向量 boot_vectors.c
#   components/modules/...       co_log 与 OS/平台头文件
FR_ROOT ?= $(ROOT)/../vendor/FREQCHIP/FR8000U-SDK-v2.2
FR_COMP := $(FR_ROOT)/components

# --------------------------- 工具链 / 编译参数 ------------------------------
CROSS     := arm-none-eabi-
ARCH      := -mcpu=cortex-m3 -mthumb
SYS_LIBS  := --specs=nano.specs --specs=nosys.specs

# 闭源库要求链接其符号地址表（第二个 -T）
CHIP_LDFLAGS := -T$(FR_COMP)/ble/library/syscall_gcc.txt

# 应用占用 0x10000000 起的 256KB（见 port/fr2012a/ld/app.ld）；
# NV / MAC / BLE 绑定区在其后 0x7A000 / 0x7B000 / 0x7D000~0x7F000。
CHIP_CDEFS   := -DFLASH_CAPACITY=0x00080000

# 角色入口：厂商 ROM 的 main() 调用 user_main()，故不用 app/main.c
CHIP_APP_MAIN := $(ROOT)/port/fr2012a/bridge_fr2012a_boot.c

# 板级头：本芯片把 keyboard/dongle 的 bridge_board.h 放在 port/fr2012a/board/ 下，
# 必须排在 -Iapp/$(TARGET) 之前，避免误用 CH58x 的同名板级头。
CHIP_INCS := -I$(ROOT)/port/fr2012a/board/$(TARGET)

# 输出目录后缀：与 ch58x 的 build/<target>/ 分开，避免不同架构目标文件混用
CHIP_OUT_SUFFIX := -fr2012a

# --------------------------- 链接脚本 ---------------------------------------
LDSCRIPT := $(ROOT)/port/fr2012a/ld/app.ld

# --------------------------- 厂商源码集合 -----------------------------------
# 与 SDK v2.2 的 GCC 示例一致：外设驱动（含 Flash/UART）与 USB 栈都随工程编译，
# 协议栈实现才在 libfr800x_stack.a 中。
CHIP_SDK_SRC := $(FR_COMP)/driver/driver_pmu.c \
                $(FR_COMP)/driver/driver_gpio.c \
                $(FR_COMP)/driver/driver_uart_ex.c \
                $(FR_COMP)/driver/driver_flash.c \
                $(FR_COMP)/modules/common/src/co_log.c \
                $(FR_COMP)/toolchain/gcc/boot_vectors.c

# 角色相关
ifeq ($(TARGET),dongle)
# 接收器：SDK 的 USB 设备栈 + 本工程的复合 HID 描述符实现
CHIP_SDK_SRC += $(FR_COMP)/driver/usb_core.c \
                $(FR_COMP)/driver/usb_dev.c \
                $(ROOT)/port/fr2012a/usb/usb_hid_user.c
else
# 模组：HOGP HID 服务（键盘 / 多媒体 / 鼠标报表）
CHIP_SDK_SRC += $(ROOT)/port/fr2012a/hid/hid_service.c
endif

CHIP_SDK_LIBS := $(FR_COMP)/ble/library/libfr800x_stack.a

CHIP_SDK_INCS := $(FR_COMP)/driver/include \
                 $(FR_COMP)/driver/drv_common \
                 $(FR_COMP)/modules/platform/include \
                 $(FR_COMP)/modules/platform/include/cmsis \
                 $(FR_COMP)/modules/common/include \
                 $(FR_COMP)/modules/sys/include \
                 $(FR_COMP)/modules/os/include \
                 $(FR_COMP)/ble/include \
                 $(FR_COMP)/ble/include/gap \
                 $(FR_COMP)/ble/include/gatt \
                 $(FR_COMP)/ble/include/mesh \
                 $(FR_COMP)/ble/profiles/ble_hid \
                 $(ROOT)/port/fr2012a/usb \
                 $(ROOT)/port/fr2012a/hid

# v1 无 BOOT 镜像：CHIP_BOOT_* 留空（`make images` 只构建 App）
CHIP_BOOT_LD  :=
CHIP_BOOT_ASM :=
CHIP_BOOT_LIB :=
