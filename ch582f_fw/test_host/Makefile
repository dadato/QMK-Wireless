# 主机端功能验证（PC 上真编译真运行）
#
#   make -C wireless/ch582f_fw/test_host test        # core + port 两套用例
#   make -C wireless/ch582f_fw/test_host test-core   # 只跑 core/（port_host.c 顶替 port 层）
#   make -C wireless/ch582f_fw/test_host test-port   # 只跑 port/ch58x（sdk_stub.c 顶替 SDK）
#
# 角色切换：make DONGLE=1 test      （以 Dongle 角色编译；默认 keyboard）
#
# 分工（两层互补，缺一层就测不到对应的缺陷类别）：
#   test-core：port_host.c 顶替**整个** port 层 → 验证 core/（协议/状态机/D14 时序）
#   test-port：sdk_stub.c 顶替 WCH SDK → 验证 port/ch58x/（FIFO 背压、NV 读-改-写、
#              ADC 分压、GPIO 模式与极性、射频参数翻译、厂商回调翻译、休眠判定）
#              必须排除 port/ch58x/bridge_ch58x_sdk.c（全工程唯一 #include SDK 头的文件）
#              且不得链接 port_host.c（与 bridge_port.h 符号冲突）

CC      ?= gcc
DONGLE  ?= 0

ROOT    := ..
CORE    := $(ROOT)/core
PORT    := $(ROOT)/port/ch58x

INC     := -I$(ROOT)/test_host/board -I$(CORE) -I$(ROOT)/test_host -I$(PORT)
CFLAGS  := -std=c99 -Wall -Wextra -Wno-unused-parameter -g -O1 $(INC)
LDFLAGS :=

ifeq ($(DONGLE),1)
CFLAGS  += -DBRIDGE_TEST_AS_DONGLE=1
SUFFIX  := dongle
else
SUFFIX  := keyboard
endif

# 目标文件按角色分目录：切换 DONGLE 不会误用另一角色编译出的 .o
BUILD   := build/$(SUFFIX)

# 必须排除 port/ch58x 下所有 #include WCH SDK 头的边界文件：
#   bridge_ch58x_sdk.c（SDK 原语）、bridge_rf24_ch58x.c（2.4G 射频接入）、
#   bridge_usb_ch58x.c（Dongle USB 复合设备）、
#   bridge_ble_hidsvc.c（HOGP HID 服务，替代 SDK 的 hidkbdservice.c）、
#   bridge_batt_svc.c（BLE 电池服务，替代 SDK 的 battservice.c），
# 它们用 sdk_stub.c 顶替，否则 PC 编译会找不到 CONFIG.h/HAL.h。
# 且不得链接 port_host.c（与 bridge_port.h 符号冲突）
SDK_BOUNDARY := $(PORT)/bridge_ch58x_sdk.c $(PORT)/bridge_rf24_ch58x.c \
                $(PORT)/bridge_usb_ch58x.c $(PORT)/bridge_ble_hidsvc.c \
                $(PORT)/bridge_batt_svc.c

SRCS_CORE := $(wildcard $(CORE)/*.c) port_host.c main_test.c
SRCS_PORT := $(wildcard $(CORE)/*.c) \
             $(filter-out $(SDK_BOUNDARY),$(wildcard $(PORT)/*.c)) \
             sdk_stub.c port_test.c

OBJS_CORE := $(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SRCS_CORE)))))
OBJS_PORT := $(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SRCS_PORT)))))

BIN_CORE := $(BUILD)/bridge_test_$(SUFFIX)
BIN_PORT := $(BUILD)/bridge_port_test_$(SUFFIX)

VPATH := $(CORE) $(PORT)

.PHONY: test test-core test-port clean

test: test-core test-port

test-core: $(BIN_CORE)
	./$(BIN_CORE)

test-port: $(BIN_PORT)
	./$(BIN_PORT)

$(BIN_CORE): $(OBJS_CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_PORT): $(OBJS_PORT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# 产物全部落在 build/（已被仓库根 .gitignore 的 build/ 规则忽略），
# 避免 .exe/.o 被误提交
clean:
	rm -rf build
