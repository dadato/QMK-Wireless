# ===========================================================================
# 无线桥固件 —— 目标侧构建（GNU make）
#
# 分层解耦（换无线芯片 = 只换 port/<chip>/，app/ core/ hal/ web/ 不动）：
#
#   hal/           芯片无关硬件抽象接口（bridge_port.h）
#   core/          芯片无关业务核心（协议/会话/HID/OTA/配置/电源/模式）
#   port/<chip>/   芯片适配层（bridge_port.h 实现 + bridge_chip.h 入口 + port.mk）
#   app/           角色应用层（模组 keyboard / 接收器 dongle 各自的板级头 + 链接脚本）
#   boot/<chip>/   常驻 BOOT 镜像（OTA 搬运 / 收包恢复）
#   web/           通用型 Web（测试 debug_web / 升级 ota_web，不区分芯片）
#
# 用法（模组 / 接收器两个目标，同一份源码）：
#
#   make keyboard            # 模组（键盘侧无线模组；debug：日志开）
#   make dongle              # 接收器（USB Dongle；debug：日志开）
#   make both                # 两个目标都构建
#   make keyboard LOG=0      # release（日志代码整体移除，量产出货）
#   make keyboard CHIP=ch58x # 选择芯片适配层（默认 ch58x）
#   make images              # BOOT + APP 合并整机 hex（工厂只烧一条）
#   make clean
#
# 芯片相关参数（SDK 路径 / 源文件集合 / BOOT 镜像源）全部在 port/<chip>/port.mk，
# 本文件只保留与芯片无关的规则；换芯片时新增 port/<newchip>/ 即可。
#
# MounRiver Studio：File → Import → C/C++ → Existing Code as Makefile Project，
# 选本目录，工具链选 RISC-V，两个 Build Configuration 分别填目标名
# keyboard / dongle（见 .cproject）。
#
# 设计依据：wireless/ch582m_bridge_impl_checklist.md §1/§2/§9/§11
# ===========================================================================

# 注意：GNU make 会保留赋值语句中 `#` 之前的所有空白，故本文件一律把注释
# 放在独立行，不写在赋值行尾（否则 TARGET/OUT/BIN 会带上尾随空格，
# 导致规则目标被拆成两个词）。
TARGET  ?= keyboard
CHIP    ?= ch58x
LOG     ?= 1
# 低功耗开关：1 = 量产默认（HAL_SLEEP=TRUE，空闲进浅睡、MODE_SLEEP 进深睡）；
# 0 = 台架/调试（不进睡，SDI 调试口随时可连）。桥固件浅睡时
# CH58X_LowPower→LowPower_Sleep 会停 CPU 时钟，WCH-Link 无法 examine，
# 表现为"跑起来的固件连不上调试口"→易误判为砖→反复救援。调试/烧录期用 SLEEP=0。
SLEEP   ?= 1

ROOT    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
space   := $(empty) $(empty)

rwildcard = $(foreach d,$(wildcard $1/*),$(call rwildcard,$d,$2) $(filter $2,$d))

# --------------------------- 芯片适配层 -------------------------------------
# 每颗芯片一个目录 port/<chip>/，自带 port.mk（SDK 路径 / 源文件 / BOOT 镜像源）。
# 移植到新芯片：新增 port/<newchip>/{bridge_chip.h, bridge_port.h 实现…, port.mk}。
CHIP_MK := $(ROOT)/port/$(CHIP)/port.mk
ifeq ($(wildcard $(CHIP_MK)),)
$(error 未找到芯片适配层 '$(CHIP)'：$(CHIP_MK) 不存在)
endif
include $(CHIP_MK)

# 交叉工具链前缀：MounRiver Studio 新版为 riscv-wch-elf- ；旧版/开源版为
# riscv-none-elf- / riscv-none-embed-
CROSS     ?= riscv-wch-elf-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

# --------------------------- 目标合法性 -------------------------------------
ifeq ($(filter $(TARGET),keyboard dongle),)
$(error TARGET 必须是 keyboard（模组）或 dongle（接收器），当前为 '$(TARGET)')
endif

# --------------------------- 源文件 -----------------------------------------
# 本工程自有源码（模组/接收器共用同一份；板级差异由 -Iapp/$(TARGET) 决定）
OUR_SRC := $(wildcard $(ROOT)/core/*.c) \
           $(wildcard $(ROOT)/port/$(CHIP)/*.c) \
           $(ROOT)/app/main.c

ALL_SRC := $(OUR_SRC) $(CHIP_SDK_SRC) $(RF24_SRC)
ALL_ASM := $(CHIP_SDK_ASM)

# 链接脚本：模组 / 接收器均走同一套「常驻 BOOT」OTA 分区（App=APP_A @0x2000, 216K）。
# 布局与升级流程见 port/ch58x/bridge_ota_layout.h。
LDSCRIPT := $(ROOT)/app/$(TARGET)/Link_App216K.ld

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

INC     := -I$(ROOT)/core -I$(ROOT)/hal -I$(ROOT)/port/$(CHIP) -I$(ROOT)/app/$(TARGET) \
           $(addprefix -I,$(CHIP_SDK_INCS)) $(addprefix -I,$(RF24_INCS))

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

## 模组 / 接收器：递归 make 以便按 TARGET 重新求值路径与输出目录
keyboard dongle:
	@$(MAKE) --no-print-directory __build TARGET=$@ CHIP=$(CHIP) LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)

both:
	@$(MAKE) --no-print-directory __build TARGET=keyboard CHIP=$(CHIP) LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)
	@$(MAKE) --no-print-directory __build TARGET=dongle CHIP=$(CHIP) LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)

__build: $(ELF)
	@$(SIZE) $(ELF)
	@echo ""
	@echo "  target : $(TARGET)   chip=$(CHIP)   log=$(LOG)"
	@echo "  elf    : $(ELF)"
	@echo "  hex/bin: $(OUT)/$(BIN).hex  $(OUT)/$(BIN).bin"
	@echo ""

$(ELF): $(OBJS) $(LDSCRIPT) $(CHIP_SDK_LIBS) | $(OUT)
	$(CC) $(ARCH) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections \
	      -Wl,-Map=$(MAP) $(SYS_LIBS) \
	      -o $@ $(OBJS) $(CHIP_SDK_LIBS)
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
#   boot_keyboard → 0x00000000（8K，查标志搬运 Stage→APP_A；模组 UART OTA）
#   boot_dongle   → 0x00000000（8K，自带 USB HID 收包 + 搬运；接收器 OTA）
# 工厂只需烧一条合并 hex（BOOT + APP_A）：
#   build/boot/bridge_keyboard_full.hex   （模组整机）
#   build/boot/bridge_dongle_full.hex     （接收器整机）
# 用法：make images      （产出 BOOT bin/hex + APP + 合并 hex，可直接 WCHISPStudio 烧录）
# BOOT 源文件/链接脚本/启动文件由 port/<chip>/port.mk 提供（CHIP_BOOT_*）。
# ===========================================================================
BOOT_DIR  := $(ROOT)/build/boot
BOOT_ELF_KBD := $(BOOT_DIR)/bridge_boot_keyboard.elf
BOOT_ELF_DGL := $(BOOT_DIR)/bridge_boot_dongle.elf
# 接收器 BOOT 用自身板级头（去掉模组的 -Iapp/keyboard，改 -Iapp/dongle）：
# 与 App 构建保持同一板级语义（VID/PID 等取自 app/dongle/bridge_board.h）。
BOOT_DGL_CFLAGS := $(filter-out -I$(ROOT)/app/keyboard,$(CFLAGS)) $(CHIP_BOOT_DGL_INC)

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
	@$(MAKE) --no-print-directory both CHIP=$(CHIP) LOG=$(LOG) SLEEP=$(SLEEP) \
	        SDK_ROOT=$(SDK_ROOT) SDK_DIR=$(SDK_DIR) RF24_DIR=$(RF24_DIR) \
	        LIB_FLASH_BASE_ADDRESSS=$(LIB_FLASH_BASE_ADDRESSS)
	# 合并整机 hex：BOOT 先用 0xFF 补齐到 0x2000 再拼 APP，使 0x0..APP_end 成为
	# **单一连续段**。否则两段不连续（0x0..~0xE00 与 0x2000+）会被 WCH OpenOCD 的
	# program/flash write_image 拒绝（"Flash write discontinued ... next section at"），
	# 单条 program 中途失败曾把接收器写成半砖。补齐后工厂/OpenOCD 均可一条烧完。
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
	@echo "      模组  : $(BOOT_DIR)/bridge_keyboard_full.hex"
	@echo "      接收器: $(BOOT_DIR)/bridge_dongle_full.hex"
	@echo ""

$(BOOT_ELF_KBD): $(CHIP_BOOT_KBD_SRC) $(CHIP_BOOT_ASM) $(CHIP_BOOT_LD) | $(BOOT_DIR)
	$(CC) $(CFLAGS) -T$(CHIP_BOOT_LD) -nostartfiles -Wl,--gc-sections \
	      $(SYS_LIBS) -o $@ $(CHIP_BOOT_KBD_SRC) $(CHIP_BOOT_ASM) \
	      $(CHIP_BOOT_LIB)
	$(OBJCOPY) -O binary $@ $(BOOT_DIR)/bridge_boot_keyboard.bin
	$(OBJCOPY) -O ihex   $@ $(BOOT_DIR)/bridge_boot_keyboard.hex

$(BOOT_ELF_DGL): $(CHIP_BOOT_DGL_SRC) $(CHIP_BOOT_ASM) $(CHIP_BOOT_LD) | $(BOOT_DIR)
	$(CC) $(BOOT_DGL_CFLAGS) -T$(CHIP_BOOT_LD) -nostartfiles -Wl,--gc-sections \
	      $(SYS_LIBS) -o $@ $(CHIP_BOOT_DGL_SRC) $(CHIP_BOOT_ASM) \
	      $(CHIP_BOOT_LIB)
	$(OBJCOPY) -O binary $@ $(BOOT_DIR)/bridge_boot_dongle.bin
	$(OBJCOPY) -O ihex   $@ $(BOOT_DIR)/bridge_boot_dongle.hex

$(BOOT_DIR):
	mkdir -p $(BOOT_DIR)

clean:
	rm -rf $(ROOT)/build

help:
	@echo "make keyboard|dongle|both [CHIP=ch58x] [LOG=0|1] [SDK_ROOT=<openwch/ch583 仓库根>] [SDK_DIR=<例程目录>] [RF24_DIR=...] [LIB_FLASH_BASE_ADDRESSS=...]"
	@echo "make images        # BOOT@0x0(模组/接收器) + APP_A@0x2000 + 合并整机 hex（工厂只烧一条）"
