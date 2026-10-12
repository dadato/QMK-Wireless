# ===========================================================================
# CH582F 透传桥固件 —— 目标侧构建（GNU make）
#
# 用法（键盘侧 / Dongle 侧两个目标，同一份源码）：
#
#   make keyboard            # 键盘侧（debug：日志开）
#   make dongle              # Dongle 侧（debug：日志开）
#   make both                # 两个目标都构建
#   make keyboard LOG=0      # 键盘侧 release（日志代码整体移除，量产出货）
#   make clean
#
# SDK 默认取本工程内已固定的 git submodule（openwch/ch583 仓库根）：
#   sdk/ch583 @ bd508ad7ceed48377619837051412a651952857f
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
# MounRiver Studio：File → Import → C/C++ → Existing Code as Makefile Project，
# 选本目录，工具链选 RISC-V，两个 Build Configuration 分别填目标名
# keyboard / dongle（见 .cproject）。
#
# 设计依据：wireless/ch582m_bridge_impl_checklist.md §1/§2/§9/§11
#   - D12 常驻 BOOT + Stage 暂存：LD 用 board/{keyboard,dongle}/Link_App216K.ld，
#     应用链接于 0x2000、216KB；BOOT 8K@0x0 + APP_A 216K + Stage 216K（见 bridge_ota_layout.h）
#   - 官方例程的 main（hidkbd_main.c）由 board/main.c 取代（见 §8 初始化顺序）
#   - 官方例程中需改动的仅 hidkbdservice.c / hidkbd.c 的回调转发，
#     转发目标是本工程的 sdk_ble_on_* （见 bridge_ch58x_sdk.h 末尾）
# ===========================================================================

# 注意：GNU make 会保留赋值语句中 `#` 之前的所有空白，故本文件一律把注释
# 放在独立行，不写在赋值行尾（否则 TARGET/OUT/BIN 会带上尾随空格，
# 导致规则目标被拆成两个词）。
TARGET  ?= keyboard
LOG     ?= 1
# 低功耗开关：1 = 量产默认（HAL_SLEEP=TRUE，空闲进浅睡、MODE_SLEEP 进深睡）；
# 0 = 台架/调试（不进睡，SDI 调试口随时可连）。桥固件浅睡时
# CH58X_LowPower→LowPower_Sleep 会停 CPU 时钟，WCH-Link 无法 examine，
# 表现为"跑起来的固件连不上调试口"→易误判为砖→反复救援。调试/烧录期用 SLEEP=0。
SLEEP   ?= 1

ROOT    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
space   := $(empty) $(empty)

# --------------------------- 外部依赖（按需覆盖） ---------------------------
# WCH CH583 EVT 仓库根（openwch/ch583）；默认用本工程内的 submodule
SDK_ROOT  ?= $(ROOT)/sdk/ch583

# 由仓库根推导的共享目录与例程目录（见文件头「目录布局」）
SDK_EXAM   := $(SDK_ROOT)/EVT/EXAM
SDK_SHARED := $(SDK_EXAM)/SRC
SDK_BLE    := $(SDK_EXAM)/BLE

# 例程基座（键盘实现 + HID Profile）；换例程时覆盖此变量
SDK_DIR    ?= $(SDK_BLE)/HID_Keyboard

# 可选：厂商 2.4G 用例目录（D11）。SDK 内为 $(SDK_BLE)/RF_PHY（提供 RF_PHY.c）
RF24_DIR  ?=

# 交叉工具链前缀：MounRiver Studio 新版为 riscv-wch-elf- ；旧版/开源版为
# riscv-none-elf- / riscv-none-embed-
CROSS     ?= riscv-wch-elf-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

# 固定库基址：必须与所选 SDK 版本的 LIBCH58xBLE.a 一致，配错会上电即崩（清单 11.2）
LIB_FLASH_BASE_ADDRESSS ?=

# --------------------------- 目标合法性 -------------------------------------
ifeq ($(filter $(TARGET),keyboard dongle),)
$(error TARGET 必须是 keyboard 或 dongle，当前为 '$(TARGET)')
endif

# --------------------------- 源文件 -----------------------------------------
# 本工程自有源码（键盘/Dongle 共用同一份；板级差异由 -Iboard/$(TARGET) 决定）
OUR_SRC := $(wildcard $(ROOT)/core/*.c) \
           $(wildcard $(ROOT)/port/ch58x/*.c) \
           $(ROOT)/board/main.c

rwildcard = $(foreach d,$(wildcard $1/*),$(call rwildcard,$d,$2) $(filter $2,$d))

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

# 官方例程入口必须排除，否则与 board/main.c 的 main() 冲突；
# Profile/hidkbdservice.c 由本工程 port/ch58x/bridge_ble_hidsvc.c 整体替代
# （厂商 Report Map 只有键盘 RID 0，无法扩展 Consumer/System 报表），
# 两者同名符号会重复定义，故必须排除。
# Profile/battservice.c 由本工程 port/ch58x/bridge_batt_svc.c 整体替代
# （厂商实现电量来源与模组真实电量无关且单调下降，主机侧恒为常量），
# 两者同名符号会重复定义，故必须排除。
SDK_SRC := $(filter-out %_main.c %Main.c %hidkbdservice.c %battservice.c,\
                     $(SDK_FILES_EXAM) $(SDK_FILES_SHARED) $(SDK_FILES_HAL))

# 共享启动文件 / 链接脚本 / 预编译库（官方 .cproject 的链接集）
SDK_ASM  := $(SDK_SHARED)/Startup/startup_CH583.S
# 链接脚本：键盘 / Dongle 均走同一套「常驻 BOOT」OTA 分区（App=APP_A @0x2000, 216K），
# 各自板级目录下放置同名脚本（内容仅注释不同）。
# 布局与升级流程见 port/ch58x/bridge_ota_layout.h。
ifeq ($(TARGET),keyboard)
LDSCRIPT := $(ROOT)/board/keyboard/Link_App216K.ld
else
LDSCRIPT := $(ROOT)/board/dongle/Link_App216K.ld
endif
SDK_LIBS := $(SDK_BLE)/LIB/LIBCH58xBLE.a $(SDK_SHARED)/StdPeriphDriver/libISP583.a

SDK_INCS := $(SDK_DIR)/APP/include \
            $(SDK_DIR)/Profile/include \
            $(SDK_SHARED)/StdPeriphDriver/inc \
            $(SDK_BLE)/HAL/include \
            $(SDK_BLE)/LIB \
            $(SDK_SHARED)/RVMSIS

ifneq ($(strip $(RF24_DIR)),)
RF24_SRC  := $(filter-out %_main.c %Main.c,$(call rwildcard,$(RF24_DIR),%.c))
RF24_INCS := $(sort $(dir $(call rwildcard,$(RF24_DIR),%.h)))
endif

ALL_SRC := $(OUR_SRC) $(SDK_SRC) $(RF24_SRC)
ALL_ASM := $(SDK_ASM)

# --------------------------- 编译/链接参数 ----------------------------------
ARCH    := -march=rv32imac -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore
OPT     ?= -Os

# 不定义 CH58xBLE_ROM：那是「烧在 0x10000 的 ROM 库」变体（CH58xBLE_ROM.h）。
# 本工程链接 EVT/EXAM/BLE/LIB/LIBCH58xBLE.a，须让 CONFIG.h 走 CH58xBLE_LIB.h。
# 也不定义 DEBUG：SDK 的 PRINT/printf 会走 UART1，与桥数据面冲突（日志走 UART0，
# 见 core/bridge_log.h）。
CDEFS   :=
# 低功耗：CH58X_LowPower() 仅在 HAL_SLEEP==TRUE 时真正进睡（HAL/SLEEP.c 编译期开关）。
# SDK 的 HAL/include/config.h 用 #ifndef 提供默认值 FALSE，这里用命令行宏覆盖，
# 避免修改 SDK submodule（固定 commit）。SLEEP=0 时不定义 → HAL_SLEEP 默认 FALSE → 不进睡。
ifeq ($(SLEEP),0)
else
CDEFS   += -DHAL_SLEEP=TRUE
endif
# BLE 数据面缓冲长度：SDK HAL/MCU.c 用它填 cfg.BufMaxLen，而 ATT_MTU = BufMaxLen - 4。
# SDK 默认 27 → ATT_MTU=23 → 单次 notify 上限 MTU-3=20B，无法承载 VIA Raw HID 的
# RID0 32B 报表（HidDev_sendNoti→GATT_Notification 因超长失败）。128 → ATT_MTU=124，
# 既满足 RID0 32B，也给 GATT 透传服务留足余量。用命令行宏覆盖，不动 SDK submodule。
CDEFS   += -DBLE_BUFF_MAX_LEN=128
ifeq ($(LOG),0)
CDEFS   += -DBRIDGE_BOARD_LOG_ENABLE=0
endif
ifneq ($(strip $(LIB_FLASH_BASE_ADDRESSS)),)
CDEFS   += -DLIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)
endif
# 排障用：命令行追加宏（如 EXTRA_CDEFS=-DBRIDGE_WDOG_TIMEOUT_MS=0）。
# 仅 __build 直接调用时生效（keyboard/dongle 递归不会透传，属排障口）。
ifneq ($(strip $(EXTRA_CDEFS)),)
CDEFS   += $(EXTRA_CDEFS)
endif

INC     := -I$(ROOT)/core -I$(ROOT)/port/ch58x -I$(ROOT)/board/$(TARGET) \
           $(addprefix -I,$(SDK_INCS)) $(addprefix -I,$(RF24_INCS))

WARN    ?= -Wall -Wno-unused-parameter
CFLAGS  := $(ARCH) $(OPT) $(WARN) -std=gnu99 -fmessage-length=0 -fsigned-char \
           -ffunction-sections -fdata-sections -fno-common -g3 $(CDEFS) $(INC)
ASFLAGS := $(ARCH) -x assembler-with-cpp -g3 $(INC)

# 与官方 .cproject 一致：newlib-nano + nosys（提供 _sbrk / _write 等桩函数）
SYS_LIBS ?= --specs=nano.specs --specs=nosys.specs

# --------------------------- 输出 -------------------------------------------
ifeq ($(SLEEP),0)
OUT     := $(ROOT)/build/$(TARGET)-nosleep
else ifeq ($(LOG),0)
OUT     := $(ROOT)/build/$(TARGET)-release
else
OUT     := $(ROOT)/build/$(TARGET)
endif
BIN     := bridge_$(TARGET)
ELF     := $(OUT)/$(BIN).elf
MAP     := $(OUT)/$(BIN).map

sanitize = $(subst :,_,$(subst /,_,$(subst $(space),_,$1)))
obj_of   = $(OUT)/$(call sanitize,$1).o

OBJS := $(foreach s,$(ALL_SRC) $(ALL_ASM),$(call obj_of,$(s)))

# ===========================================================================
# 目标
# ===========================================================================
.PHONY: all keyboard dongle both clean help __build

all: keyboard

## 键盘侧 / Dongle 侧：递归 make 以便按 TARGET 重新求值路径与输出目录
keyboard dongle:
	@$(MAKE) --no-print-directory __build TARGET=$@ LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)

both:
	@$(MAKE) --no-print-directory __build TARGET=keyboard LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)
	@$(MAKE) --no-print-directory __build TARGET=dongle LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)

__build: $(ELF)
	@$(SIZE) $(ELF)
	@echo ""
	@echo "  target : $(TARGET)   log=$(LOG)"
	@echo "  elf    : $(ELF)"
	@echo "  hex/bin: $(OUT)/$(BIN).hex  $(OUT)/$(BIN).bin"
	@echo ""

$(ELF): $(OBJS) $(LDSCRIPT) $(SDK_LIBS) | $(OUT)
	$(CC) $(ARCH) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections \
	      -Wl,-Map=$(MAP) $(SYS_LIBS) \
	      -o $@ $(OBJS) $(SDK_LIBS)
	$(OBJCOPY) -O ihex   $@ $(OUT)/$(BIN).hex
	$(OBJCOPY) -O binary $@ $(OUT)/$(BIN).bin

$(OUT):
	mkdir -p $(OUT)

# 每个源文件一条显式规则；对象名由绝对路径扁平化而来，不会重名
define COMPILE_RULE
$(call obj_of,$(1)): $(1) | $(OUT)
	$(CC) -c $$(CFLAGS) -o $$@ $$<
endef
define ASSEMBLE_RULE
$(call obj_of,$(1)): $(1) | $(OUT)
	$(CC) -c $$(ASFLAGS) -o $$@ $$<
endef

$(foreach s,$(ALL_SRC),$(eval $(call COMPILE_RULE,$(s))))
$(foreach s,$(ALL_ASM),$(eval $(call ASSEMBLE_RULE,$(s))))

# ===========================================================================
# OTA 辅助镜像：「一个常驻 BOOT」结构（见 port/ch58x/bridge_ota_layout.h）
#   boot_keyboard → 0x00000000（8K，查标志搬运 Stage→APP_A；键盘 UART OTA）
#   boot_dongle   → 0x00000000（8K，自带 USB HID 收包 + 搬运；接收器 OTA）
# 工厂只需烧一条合并 hex（BOOT + APP_A）：
#   build/boot/bridge_keyboard_full.hex   （键盘侧整机）
#   build/boot/bridge_dongle_full.hex     （接收器整机）
# 用法：make images      （产出 BOOT bin/hex + APP + 合并 hex，可直接 WCHISPStudio 烧录）
# ===========================================================================
BOOT_DIR  := $(ROOT)/build/boot
BOOT_LD   := $(ROOT)/board/boot/Link_Boot8K.ld
BOOT_ASM  := $(SDK_BLE)/BackupUpgrade_IAP/Startup/startup_CH583.S
BOOT_ELF_KBD := $(BOOT_DIR)/bridge_boot_keyboard.elf
BOOT_ELF_DGL := $(BOOT_DIR)/bridge_boot_dongle.elf
# 键盘 BOOT（搬运 + App 无效时 UART 收包）与 Dongle USB BOOT（收包+搬运）共享
# iap_common.c 原语。键盘 BOOT 额外链 CH58x_uart1.c（UART1 轮询收发，无 ISR）与
# CH58x_gpio.c（PA8/PA9 引脚配置），二者均纯 StdPeriphDriver、无 BLE/TMOS 依赖。
BOOT_KBD_SRC := $(ROOT)/board/iap/iap_main.c \
                $(ROOT)/board/iap/iap_common.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_flash.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_clk.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_sys.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_uart1.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_gpio.c
# Dongle USB BOOT：自带轮询式 USB HID 设备栈（.highcode 内 USB_DevTransProcess），
# 故**不**链接 CH58x_usbdev.c（否则端点缓冲/处理函数重复定义）。
# 额外头：board/dongle(VID/PID/板级)；core/port 已在全局 INC 中。
BOOT_DGL_SRC := $(ROOT)/board/iap/iap_usb_main.c \
                $(ROOT)/board/iap/iap_common.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_flash.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_clk.c \
                $(SDK_SHARED)/StdPeriphDriver/CH58x_sys.c
BOOT_DGL_INC := -I$(ROOT)/board/dongle -I$(ROOT)/core
# App 输出目录后缀（与上方 OUT 规则一致），用于定位 App hex 以合并
ifeq ($(SLEEP),0)
APP_SFX := -nosleep
else ifeq ($(LOG),0)
APP_SFX := -release
else
APP_SFX :=
endif
KEYBOARD_APP_HEX := $(ROOT)/build/keyboard$(APP_SFX)/bridge_keyboard.hex
DONGLE_APP_HEX   := $(ROOT)/build/dongle$(APP_SFX)/bridge_dongle.hex

.PHONY: images
images: $(BOOT_ELF_KBD) $(BOOT_ELF_DGL)
	@echo ""
	@$(MAKE) --no-print-directory both LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)
	# 合并整机 hex：BOOT 先用 0xFF 补齐到 0x2000 再拼 APP，使 0x0..APP_end 成为
	# **单一连续段**。否则两段不连续（0x0..~0xE00 与 0x2000+）会被 WCH OpenOCD 的
	# program/flash write_image 拒绝（"Flash write discontinued ... next section at"），
	# 单条 program 中途失败曾把 Dongle 写成半砖。补齐后工厂/OpenOCD 均可一条烧完。
	@dd if=/dev/zero bs=8192 count=1 2>/dev/null | tr '\000' '\377' > $(BOOT_DIR)/_pad_kbd.bin
	@dd if=$(BOOT_DIR)/bridge_boot_keyboard.bin of=$(BOOT_DIR)/_pad_kbd.bin conv=notrunc 2>/dev/null
	@$(OBJCOPY) -I binary -O ihex $(BOOT_DIR)/_pad_kbd.bin $(BOOT_DIR)/_pad_kbd.hex
	@grep -v ':00000001FF' $(BOOT_DIR)/_pad_kbd.hex > $(BOOT_DIR)/bridge_keyboard_full.hex
	@grep -v ':00000001FF' $(KEYBOARD_APP_HEX) >> $(BOOT_DIR)/bridge_keyboard_full.hex
	@echo ':00000001FF' >> $(BOOT_DIR)/bridge_keyboard_full.hex
	@rm -f $(BOOT_DIR)/_pad_kbd.bin $(BOOT_DIR)/_pad_kbd.hex
	@dd if=/dev/zero bs=8192 count=1 2>/dev/null | tr '\000' '\377' > $(BOOT_DIR)/_pad_dgl.bin
	@dd if=$(BOOT_DIR)/bridge_boot_dongle.bin of=$(BOOT_DIR)/_pad_dgl.bin conv=notrunc 2>/dev/null
	@$(OBJCOPY) -I binary -O ihex $(BOOT_DIR)/_pad_dgl.bin $(BOOT_DIR)/_pad_dgl.hex
	@grep -v ':00000001FF' $(BOOT_DIR)/_pad_dgl.hex > $(BOOT_DIR)/bridge_dongle_full.hex
	@grep -v ':00000001FF' $(DONGLE_APP_HEX) >> $(BOOT_DIR)/bridge_dongle_full.hex
	@echo ':00000001FF' >> $(BOOT_DIR)/bridge_dongle_full.hex
	@rm -f $(BOOT_DIR)/_pad_dgl.bin $(BOOT_DIR)/_pad_dgl.hex
	@echo ""
	@echo "  OTA images (常驻 BOOT @0x0 + APP_A @0x2000):"
	@echo "    boot_kbd : $(BOOT_DIR)/bridge_boot_keyboard.bin → 0x00000000"
	@echo "    boot_dgl : $(BOOT_DIR)/bridge_boot_dongle.bin   → 0x00000000"
	@echo "    app_kbd  : $(KEYBOARD_APP_HEX)  → 0x00002000（自带地址）"
	@echo "    app_dgl  : $(DONGLE_APP_HEX)    → 0x00002000（自带地址）"
	@echo "    工厂整机烧录（推荐，一条 hex）："
	@echo "      键盘  : $(BOOT_DIR)/bridge_keyboard_full.hex"
	@echo "      接收器: $(BOOT_DIR)/bridge_dongle_full.hex"
	@echo ""

$(BOOT_ELF_KBD): $(BOOT_KBD_SRC) $(BOOT_ASM) $(BOOT_LD) | $(BOOT_DIR)
	$(CC) $(CFLAGS) -T$(BOOT_LD) -nostartfiles -Wl,--gc-sections \
	      $(SYS_LIBS) -o $@ $(BOOT_KBD_SRC) $(BOOT_ASM) \
	      $(SDK_SHARED)/StdPeriphDriver/libISP583.a
	$(OBJCOPY) -O binary $@ $(BOOT_DIR)/bridge_boot_keyboard.bin
	$(OBJCOPY) -O ihex   $@ $(BOOT_DIR)/bridge_boot_keyboard.hex

$(BOOT_ELF_DGL): $(BOOT_DGL_SRC) $(BOOT_ASM) $(BOOT_LD) | $(BOOT_DIR)
	$(CC) $(CFLAGS) $(BOOT_DGL_INC) -T$(BOOT_LD) -nostartfiles -Wl,--gc-sections \
	      $(SYS_LIBS) -o $@ $(BOOT_DGL_SRC) $(BOOT_ASM) \
	      $(SDK_SHARED)/StdPeriphDriver/libISP583.a
	$(OBJCOPY) -O binary $@ $(BOOT_DIR)/bridge_boot_dongle.bin
	$(OBJCOPY) -O ihex   $@ $(BOOT_DIR)/bridge_boot_dongle.hex

$(BOOT_DIR):
	mkdir -p $(BOOT_DIR)

clean:
	rm -rf $(ROOT)/build

help:
	@echo "make keyboard|dongle|both [LOG=0|1] [SDK_ROOT=<openwch/ch583 仓库根>] [SDK_DIR=<例程目录>] [RF24_DIR=...] [LIB_FLASH_BASE_ADDRESSS=...]"
	@echo "make images        # BOOT@0x0(键盘/接收器) + APP_A@0x2000 + 合并整机 hex（工厂只烧一条）"
