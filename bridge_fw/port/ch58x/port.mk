# ===========================================================================
# CH58x（WCH CH582M / CH583）芯片适配层构建描述
#
# 由顶层 Makefile 在定义 ROOT / space / rwildcard 之后 include。
# 本文件是**移植到新芯片时需要新增的唯一构建文件**：只需在本目录（port/<chip>/）
# 提供与 hal/bridge_port.h、port/ch58x/bridge_chip.h 对应的实现，并在此声明
# 该芯片的 SDK 路径、源文件集合与常驻 BOOT 镜像源。
#
# SDK 默认取本芯片目录内已固定的 git submodule（openwch/ch583 仓库根）：
#   port/ch58x/sdk/ch583 @ bd508ad7ceed48377619837051412a651952857f
# 换 SDK 版本只需覆盖仓库根，其余路径自动推导：
#
#   make keyboard SDK_ROOT=/d/wch/ch583
#   make keyboard RF24_DIR=/d/wch/ch583/EVT/EXAM/BLE/RF_PHY   # 并入厂商 2.4G 用例（D11）
#   make keyboard LIB_FLASH_BASE_ADDRESSS=0x00010000          # 与所选 SDK 版本一致（清单 11.2）
#
# 目录布局（openwch/ch583 是「共享目录 + 例程目录」，非每例程自包含）：
#   EVT/EXAM/SRC                 共享：StdPeriphDriver/ RVMSIS/ Ld/Link.ld Startup/
#   EVT/EXAM/BLE/LIB             共享：BLE 协议栈预编译库 LIBCH58xBLE.a + 头
#   EVT/EXAM/BLE/HAL             共享：CH58X_BLEInit / CH58X_LowPower / RTC
#   EVT/EXAM/BLE/HID_Keyboard    例程基座：APP/hidkbd.c + Profile/hiddev|hidkbdservice|...
#
# 设计依据：wireless/bridge_fw/docs/bridge_impl_checklist.md §1/§2/§9/§11
#   - D12 常驻 BOOT + Stage 暂存：App 链接于 0x2000、216KB（app/<target>/Link_App216K.ld）；
#     BOOT 8K@0x0 + APP_A 216K + Stage 216K（见 port/ch58x/bridge_ota_layout.h）
# ===========================================================================

# --------------------------- 外部依赖（按需覆盖） ---------------------------
SDK_ROOT  ?= $(ROOT)/port/ch58x/sdk/ch583

# 由仓库根推导的共享目录与例程目录（见文件头「目录布局」）
SDK_EXAM   := $(SDK_ROOT)/EVT/EXAM
SDK_SHARED := $(SDK_EXAM)/SRC
SDK_BLE    := $(SDK_EXAM)/BLE

# 例程基座（键盘实现 + HID Profile）；换例程时覆盖此变量
SDK_DIR    ?= $(SDK_BLE)/HID_Keyboard

# 可选：厂商 2.4G 用例目录（D11）。SDK 内为 $(SDK_BLE)/RF_PHY（提供 RF_PHY.c）
RF24_DIR  ?=

# 固定库基址：必须与所选 SDK 版本的 LIBCH58xBLE.a 一致，配错会上电即崩（清单 11.2）
LIB_FLASH_BASE_ADDRESSS ?=

# --------------------------- 厂商源码集合 -----------------------------------
# 例程基座：APP/（hidkbd.c）+ Profile/（hiddev.c / hidkbdservice.c / battservice.c ...）
SDK_FILES_EXAM   := $(call rwildcard,$(SDK_DIR),%.c)
# 共享外设驱动 + RISC-V 内核支持（PFIC_* 等）
SDK_FILES_SHARED := $(wildcard $(SDK_SHARED)/StdPeriphDriver/*.c) \
                    $(SDK_SHARED)/RVMSIS/core_riscv.c
# 共享 BLE HAL（KEY.c / LED.c 是官方按键/灯例程，本工程不用）
# SLEEP.c 被**排除**：它无条件定义 CH58X_LowPower / HAL_SleepInit，而 CH58X_BLEInit()
# 会把 CH58X_LowPower 注册成协议栈的 pfnIdleCB，协议栈空闲时会自行进睡、绕过本工程
# 的睡眠策略判定（详见 port/ch58x/bridge_ch58x_sdk.c「低功耗」段）。本工程自带带
# 授权门的同名实现，故必须排除 SDK 版本以免同名符号重复定义。
SDK_FILES_HAL    := $(SDK_BLE)/HAL/MCU.c $(SDK_BLE)/HAL/RTC.c

# 官方例程入口必须排除，否则与 app/main.c 的 main() 冲突；
# Profile/hidkbdservice.c 由本工程 port/ch58x/bridge_ble_hidsvc.c 整体替代
# （厂商 Report Map 只有键盘 RID 0，无法扩展 Consumer/System 报表），
# 两者同名符号会重复定义，故必须排除。
# Profile/battservice.c 由本工程 port/ch58x/bridge_batt_svc.c 整体替代
# （厂商实现电量来源与模组真实电量无关且单调下降，主机侧恒为常量），
# 两者同名符号会重复定义，故必须排除。
CHIP_SDK_SRC := $(filter-out %_main.c %Main.c %hidkbdservice.c %battservice.c,\
                         $(SDK_FILES_EXAM) $(SDK_FILES_SHARED) $(SDK_FILES_HAL))

# 共享启动文件 / 预编译库（官方 .cproject 的链接集）
CHIP_SDK_ASM  := $(SDK_SHARED)/Startup/startup_CH583.S
CHIP_SDK_LIBS := $(SDK_BLE)/LIB/LIBCH58xBLE.a \
                 $(SDK_SHARED)/StdPeriphDriver/libISP583.a

CHIP_SDK_INCS := $(SDK_DIR)/APP/include \
                 $(SDK_DIR)/Profile/include \
                 $(SDK_SHARED)/StdPeriphDriver/inc \
                 $(SDK_BLE)/HAL/include \
                 $(SDK_BLE)/LIB \
                 $(SDK_SHARED)/RVMSIS

ifneq ($(strip $(RF24_DIR)),)
RF24_SRC  := $(filter-out %_main.c %Main.c,$(call rwildcard,$(RF24_DIR),%.c))
RF24_INCS := $(sort $(dir $(call rwildcard,$(RF24_DIR),%.h)))
endif

# --------------------------- 常驻 BOOT 镜像源 -------------------------------
# 「一个常驻 BOOT」结构（见 port/ch58x/bridge_ota_layout.h）：
#   模组 BOOT   → 0x00000000（8K，查标志搬运 Stage→APP_A；模组 UART OTA 收包）
#   接收器 BOOT → 0x00000000（8K，自带 USB HID 收包 + 搬运）
# 模组 BOOT（搬运 + App 无效时 UART 收包）与接收器 USB BOOT（收包+搬运）共享
# iap_common.c 原语。模组 BOOT 额外链 CH58x_uart1.c（UART1 轮询收发，无 ISR）与
# CH58x_gpio.c（PA8/PA9 引脚配置），二者均纯 StdPeriphDriver、无 BLE/TMOS 依赖。
CHIP_BOOT_LD  := $(ROOT)/boot/ch58x/Link_Boot8K.ld
CHIP_BOOT_ASM := $(SDK_BLE)/BackupUpgrade_IAP/Startup/startup_CH583.S
CHIP_BOOT_LIB := $(SDK_SHARED)/StdPeriphDriver/libISP583.a

CHIP_BOOT_KBD_SRC := $(ROOT)/boot/ch58x/iap_main.c \
                     $(ROOT)/boot/ch58x/iap_common.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_flash.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_clk.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_sys.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_uart1.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_gpio.c

# 接收器 USB BOOT：自带轮询式 USB HID 设备栈（原生 .highcode 的 USB_DevTransProcess），
# 故**不**链接 CH58x_usbdev.c（否则端点缓冲/处理函数重复定义）。
CHIP_BOOT_DGL_SRC := $(ROOT)/boot/ch58x/iap_usb_main.c \
                     $(ROOT)/boot/ch58x/iap_common.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_flash.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_clk.c \
                     $(SDK_SHARED)/StdPeriphDriver/CH58x_sys.c

# BOOT 额外包含路径：core/hal/本芯片适配层/BOOT 目录；接收器再加自身板级目录
CHIP_BOOT_INC     := -I$(ROOT)/core -I$(ROOT)/hal -I$(ROOT)/port/ch58x -I$(ROOT)/boot/ch58x
CHIP_BOOT_DGL_INC := $(CHIP_BOOT_INC) -I$(ROOT)/app/dongle

# --------------------------- 芯片相关编译宏 ---------------------------------
CHIP_CDEFS :=
# 低功耗：CH58X_LowPower() 仅在 HAL_SLEEP==TRUE 时真正进睡（HAL/SLEEP.c 编译期开关）。
# SDK 的 HAL/include/config.h 用 #ifndef 提供默认值 FALSE，这里用命令行宏覆盖，
# 避免修改 SDK submodule（固定 commit）。SLEEP=0 时不定义 → 默认 FALSE → 不进睡。
ifeq ($(SLEEP),0)
else
CHIP_CDEFS += -DHAL_SLEEP=TRUE
endif
# BLE 数据面缓冲长度：SDK HAL/MCU.c 用它填 cfg.BufMaxLen，而 ATT_MTU = BufMaxLen - 4。
# 默认 27 → ATT_MTU=23 → 单次 notify 上限 20B，无法承载 VIA Raw HID 的 RID0 32B 报表。
# 128 → ATT_MTU=124，既满足 RID0 32B，也给 GATT 透传服务留足余量。
CHIP_CDEFS += -DBLE_BUFF_MAX_LEN=128
