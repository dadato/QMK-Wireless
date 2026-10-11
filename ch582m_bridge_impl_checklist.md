# CH582F 透传桥固件 —— 协议实现清单（v0.4）

> 配套文档：[ch582m_bridge_fw_spec.md](./ch582m_bridge_fw_spec.md) · [ch582f_pinmap.md](./ch582f_pinmap.md)
> 本文档回答三个问题：**命令表 → 函数签名 → 状态机落到哪个文件**。
> 引脚与封装决策以 [ch582f_pinmap.md](./ch582f_pinmap.md) 第 0 章 D1~D15 为准（CH582F / QFN28）。
>
> **代码落地位置**：`wireless/ch582f_fw/`（键盘侧 `keyboard/` + Dongle 侧 `dongle/`，共享 `core/` 与 `port/ch58x/`）。
>
> **API 名称免责说明**：本文引用的 SDK 函数名以 CH583 EVT 官方例程（`openwch/ch583`）为基准。
> 不同 SDK 版本存在差异，落地时必须对照 `LIB/CH58xBLE_LIB.h` 与所选例程逐一核实。

---

## 1. 工程基线

从官方例程 **`EXAM/BLE/Peripheral/HID_Keyboard`** 起改（它已包含 HID over GATT 服务、报文发送、LED Output Report 解析）。

保留不动的部分：

- `LIB/LIBCH58xBLE.a` —— BLE 协议栈（预编译，不修改）
- `LIB/CH58xBLE_LIB.h` —— 协议栈 API 头
- `RVMSIS/` + `Startup/startup_CH583.S` —— 内核与启动
- `Peripheral/src/CH58x_uart*.c` / `CH58x_gpio.c` / `CH58x_adc.c` / `CH58x_flash.c` / `CH58x_pwr.c` —— 外设驱动

---

## 2. 文件清单

### 2.1 新增文件（桥接逻辑）

| 文件 | 职责 | 依赖 |
|---|---|---|
| `APP/bridge_uart.c` / `.h` | UART 帧收发、环形缓冲、CRC、心跳 | SDK UART 驱动 |
| `APP/bridge_proto.c` / `.h` | 命令表、分发、应答/错误/事件发送 | `bridge_uart` |
| `APP/bridge_mode.c` / `.h` | 模式状态机、优先级仲裁、记忆持久化 | `bridge_ble` / `bridge_rf24` / `bridge_cfg` |
| `APP/bridge_hid.c` / `.h` | HID 报文组装、NKRO→6KRO 降级、全键释放 | `bridge_ble` / `bridge_rf24` |
| `APP/bridge_ble.c` / `.h` | BLE 封装：初始化、多主机、配对、连接回调 | Profil/ SDK BLE |
| `APP/bridge_rf24.c` / `.h` | 2.4G 封装：**对厂商 2.4G 用例的薄封装**（init / send / 回调转事件），不自研协议（D11） | 厂商 2.4G 用例 |
| `APP/bridge_power.c` / `.h` | 休眠策略、唤醒源、ADC 电量、充电检测 | SDK PWR/ADC |
| `APP/bridge_gpio.c` / `.h` | `MODE_SEL` / `WAKE` / `LINK` 控制 | SDK GPIO |
| `APP/bridge_cfg.c` / `.h` | 配置与配对元数据的 EEPROM 持久化（双备份+CRC） | SDK EEPROM |
| `APP/bridge_log.c` / `.h` | **printf 日志（D13）**：编译期/运行期开关、分级、UART0 输出 | SDK UART0 |
| `APP/bridge_test.c` / `.h` | **SRRC 认证**：读芯片唯一 MAC、定频连续发射测试模式 | SDK BLE / 2.4G |

### 2.2 修改文件（官方例程）

| 文件 | 改动 |
|---|---|
| `Main.c` | 按第 8 章顺序初始化新模块；注册 `Bridge_ProcessEvent` |
| `APP/.../hidkbdservice.c`（或 `Profile/hidkbdservice.c`） | HID Output Report（LED）回调转发到 `bridge_hid_on_led_output()` |
| `APP/.../hidkbd.c` | 保留 HID 设备注册；报文发送改由 `bridge_hid.c` 统一调用 |
| `APP/include/bridge_cfg.h` | **新增**：桥接的编译期配置（UART 实例、波特率、引脚、超时、日志开关） |
| `EXAM/.../2.4G`（厂商用例原有文件） | **直接并入工程**，只做最小改动：暴露「初始化 / 发送 HID 报文 / 链路状态回调」三个入口供 `bridge_rf24.c` 调用（D11），**不改射频时序与跳频算法** |

### 2.3 分层关系

```
        Main.c  ──►  Bridge_ProcessEvent()   （TMOS 任务入口）
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
  bridge_uart       bridge_mode       bridge_power
        │                 │                 │
        ▼                 │                 │
  bridge_proto            │                 │
        │                 │                 │
        ├────────► bridge_hid ◄─────────────┤
        │                 │                 │
        │        ┌────────┴────────┐        │
        │        ▼                 ▼        │
        │   bridge_ble       bridge_rf24    │
        │        │                 │        │
        └────────┴─────────────────┴────────┘
                          │
                    bridge_gpio / bridge_cfg
```

**规则**：`bridge_proto` 只做参数校验与分发，**不直接碰射频 API**；射频动作一律经 `bridge_hid` / `bridge_mode`。

---

## 3. 命令表 → 处理函数

### 3.1 上行命令（SK32 → 桥）落在 `bridge_proto.c`

分发方式（as-built）：`bridge_proto_dispatch()` 用**单层 `switch (frame->cmd)`** 直分，无 `s_cmd_table[]` 表；各处理函数为 `static void handle_xxx(const bridge_frame_t *f)`，成功/失败在函数内经 `bridge_proto_ack()` / `reply()` / `error()` 表达（HID 路径 `return bridge_hid_send(...)`）；未命中 `default` 回 `ERR(PARAM)` 并返回 `BRIDGE_ERR_UNSUPPORTED`。

| CMD | 名称 | 处理函数签名 | 需应答 |
|---|---|---|---|
| `0x01` | `GET_VERSION` | `static void handle_get_version(const bridge_frame_t *f);` | 是 |
| `0x02` | `GET_STATUS` | `static void handle_get_status(const bridge_frame_t *f);` | 是 |
| `0x03` | `SET_MODE` | `static void handle_set_mode(const bridge_frame_t *f);` | 是 |
| `0x04` | `SET_PROFILE` | `static void handle_set_profile(const bridge_frame_t *f);` | 是 |
| `0x05` | `PAIR_START` | `static void handle_pair_start(const bridge_frame_t *f);` | 是 |
| `0x06` | `UNPAIR` | `static void handle_unpair(const bridge_frame_t *f);` | 是 |
| `0x07` | `RESET` | 内联处理（应答后 `port_reset()`） | 是（应答后再复位） |
| `0x08` | `ENTER_BOOT` | 内联处理（应答并 flush 后 `port_enter_bootloader()`） | 是 |
| `0x09` | `SET_CONFIG` | `static void handle_set_config(const bridge_frame_t *f);` | 是 |
| `0x0A` | `GET_MAC` | `static void handle_get_mac(const bridge_frame_t *f);` | 是 |
| `0x0B` | `SET_TEST_MODE` | `static void handle_test_mode(const bridge_frame_t *f);` | 是 |
| `0x0C` | `GET_MODE` | `static void handle_get_mode(const bridge_frame_t *f);` | 是 |
| `0x0D` | `SET_HEARTBEAT` | `static void handle_set_heartbeat(const bridge_frame_t *f);` | 是 |
| `0x0E` | `GET_WIRELESS_STATE` | `static void handle_get_wireless_state(const bridge_frame_t *f);` | 是 |
| `0x10` | `HID_KEYBOARD` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x11` | `HID_NKRO` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x12` | `HID_CONSUMER` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x13` | `HID_MOUSE` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x14` | `HID_SYSTEM` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x15` | `HID_RELEASE_ALL` | `bridge_hid_send()`（无独立 handle） | **否** |
| `0x18` | `TUNNEL_USB` | `static void handle_tunnel(const bridge_frame_t *f);` | **否** |
| `0x19` | `TUNNEL_BLE` | `static void handle_tunnel(const bridge_frame_t *f);`（与 USB 同函数，按 cmd 分派 kind） | **否** |
| `0x1A` | `NV_WRITE` | `static void handle_nv_write(const bridge_frame_t *f);` | 是 |
| `0x1B` | `NV_READ` | `static void handle_nv_read(const bridge_frame_t *f);` | 是 |
| `0x1C` | `SET_BATTERY` | `static void handle_set_battery(const bridge_frame_t *f);` | 是 |
| `0x1D` | `SLEEP` | `static void handle_sleep(const bridge_frame_t *f);` | 是 |
| `0x1E` | `GET_ADC` | `static void handle_get_adc(const bridge_frame_t *f);` | 是 |
| `0x1F` | `SET_NAME` | `static void handle_set_name(const bridge_frame_t *f);` | 是 |
| `0x20` | `SET_USB_ID` | `static void handle_set_usb_id(const bridge_frame_t *f);` | 是 |
| `0x21` | `FW_BEGIN` | `static void handle_fw_begin(const bridge_frame_t *f);` | 是 |
| `0x22` | `FW_DATA` | `static void handle_fw_data(const bridge_frame_t *f);` | 是 |
| `0x23` | `FW_END` | `static void handle_fw_end(const bridge_frame_t *f);` | 是（应答并 flush 后进 IAP） |
| `0x24` | `FW_ABORT` | `static void handle_fw_abort(const bridge_frame_t *f);` | 是 |
| `0x7F` | `PING` | 内联处理（心跳开时回 `bridge_evt_pong()`） | 是（回 `PONG`） |

> 统一返回约定：处理函数为 `void`，成功/失败由函数内应答表达；`bridge_proto_dispatch()` 对未支持命令返回 `BRIDGE_ERR_UNSUPPORTED`。
> 命令码枚举权威定义见 `core/bridge_types.h`；完整 payload / 应答格式见 [debug_web/SPEC.md](./ch582f_fw/board/debug_web/SPEC.md) §4.6 / §4.7。`0x0F`、`0x16`、`0x17` 保留未用。

### 3.2 下行事件（桥 → SK32）构造函数在 `bridge_proto.c`

| CMD | 名称 | 构造函数签名 |
|---|---|---|
| `0x80` | `EVT_STATE` | `void bridge_evt_state(void);` |
| `0x81` | `EVT_LEDS` | `void bridge_evt_leds(uint8_t led_state);` |
| `0x82` | `EVT_BATTERY` | `void bridge_evt_battery(uint16_t mv, uint8_t percent);` |
| `0x83` | `EVT_PAIR_RESULT` | `void bridge_evt_pair_result(uint8_t result, uint8_t profile);` |
| `0x84` | `EVT_ERROR` | `void bridge_evt_error(uint8_t code);` |
| `0x85` | `EVT_TUNNEL_USB` | `void bridge_evt_tunnel(uint8_t kind, const uint8_t *payload, uint8_t len);`（`kind = BRIDGE_TUNNEL_USB`） |
| `0x86` | `EVT_TUNNEL_BLE` | `void bridge_evt_tunnel(uint8_t kind, const uint8_t *payload, uint8_t len);`（`kind = BRIDGE_TUNNEL_BLE`，与 USB 同一构造函数） |
| `0x87` | `EVT_HOST_POWER` | `void bridge_evt_host_power(uint8_t state);`（`state` 取 `BRIDGE_HOST_*`） |
| `0x7F` | `PONG` | `void bridge_evt_pong(uint8_t seq);` |

---

## 4. 各模块函数签名

### 4.1 `bridge_uart.h`

```c
/* 初始化桥接串口（默认 921600, 8N1, 无流控；D14） */
void     bridge_uart_init(uint32_t baudrate);

/* 发送一帧；返回 0 成功，-1 忙（忙时按 BRIDGE_TX_LATEST_ONLY 只保留最新 HID 帧） */
int      bridge_uart_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                          const uint8_t *payload, uint8_t len);

/* 时序约束（D14）：
 *  - 距上一帧不足 BRIDGE_FRAME_MIN_GAP_MS 时返回 -2（调用方需排队重试）
 *  - HID 报文去重：与上一帧内容完全一致时返回 -3（不发送） */
int      bridge_uart_send_hid(uint8_t cmd, const uint8_t *payload, uint8_t len);
bool     bridge_uart_tx_ready(void);     /* 空闲且已满足最小帧间隔 */
uint32_t bridge_uart_last_tx_ms(void);

/* 中断服务中调用：把收到的字节推入环形缓冲 */
void     bridge_uart_isr_feed(uint8_t byte);

/* 任务中调用：尝试从环形缓冲取出一个完整帧 */
bool     bridge_uart_get_frame(bridge_frame_t *out);

/* 心跳：发送 PING */
void     bridge_uart_ping(void);

/* 断链检测：3 次未收到 PING 触发回调 */
bool     bridge_uart_is_link_alive(void);
uint32_t bridge_uart_rx_gap_us(void);

/* CRC-16/CCITT-FALSE */
uint16_t bridge_crc16_ccitt(const uint8_t *data, uint16_t len);
```

### 4.2 `bridge_proto.h`

```c
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t flags;
    uint8_t len;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];   /* BRIDGE_MAX_PAYLOAD = 64 */
} bridge_frame_t;

/* TMOS 任务入口（在 Main.c 中注册） */
uint16_t Bridge_ProcessEvent(uint8_t task_id, uint16_t events);

/* 分发单帧，内部查 s_cmd_table */
int      bridge_proto_dispatch(const bridge_frame_t *frame);

/* 应答/错误/事件发送 */
void     bridge_proto_ack(uint8_t seq, uint8_t cmd);
void     bridge_proto_error(uint8_t seq, uint8_t code);
void     bridge_proto_send_event(uint8_t cmd, const uint8_t *payload, uint8_t len);
```

### 4.3 `bridge_mode.h`

```c
typedef enum {
    BRIDGE_MODE_USB = 0,
    BRIDGE_MODE_BLE,
    BRIDGE_MODE_BLE_PAIR,
    BRIDGE_MODE_2G4,
    BRIDGE_MODE_2G4_PAIR,
    BRIDGE_MODE_OTA,
    BRIDGE_MODE_TEST,        /* SRRC 认证定频测试（见 4.10 / 9 章） */
    BRIDGE_MODE_SLEEP,
} bridge_mode_t;

void          bridge_mode_init(void);                     /* 读 MODE_SEL + 记忆 */
int           bridge_mode_set(bridge_mode_t m, bool persist);
bridge_mode_t bridge_mode_get(void);
void          bridge_mode_on_vbus(bool present);          /* 最高优先级 */
void          bridge_mode_tick(void);                     /* 空闲判定 → SLEEP */
void          bridge_mode_on_uart_lost(void);             /* 失联处理 */
```

### 4.4 `bridge_hid.h`

```c
void bridge_hid_send_keyboard(const uint8_t *report8);
void bridge_hid_send_nkro(const uint8_t *report16);   /* 内部按模式降级 */
void bridge_hid_send_consumer(const uint8_t *report2);
void bridge_hid_send_mouse(const uint8_t *report4);
void bridge_hid_send_system(uint8_t report1);         /* 1 字节 */
void bridge_hid_release_all(void);                    /* 全 0 释放，切模式/复位前必调 */
void bridge_hid_on_led_output(uint8_t led_state);     /* 主机 Output Report → EVT_LEDS */
bool bridge_hid_nkro_enabled(void);
```

### 4.5 `bridge_ble.h`

```c
int      bridge_ble_init(void);
int      bridge_ble_connect_profile(uint8_t profile);      /* 1~4 */
int      bridge_ble_start_pairing(uint16_t timeout_s);
int      bridge_ble_unpair(uint8_t profile);               /* 0xFF = 全部 */
bool     bridge_ble_is_connected(void);
uint8_t  bridge_ble_get_profile(void);

/* 由 SDK 回调触发 */
void     bridge_ble_on_connected(uint16_t connHandle);
void     bridge_ble_on_disconnected(uint16_t connHandle, uint8_t reason);
void     bridge_ble_on_led_report(uint8_t led_state);
```

### 4.6 `bridge_rf24.h`（厂商用例薄封装，D11）

```c
/* 全部实现均转调厂商 2.4G 用例，不自研射频私有协议 */
int      bridge_rf24_init(void);          /* → 厂商 RF 初始化 */
int      bridge_rf24_pair_start(void);    /* → 厂商配对入口 */
bool     bridge_rf24_is_connected(void);  /* ← 厂商链路状态 */
uint8_t  bridge_rf24_get_protocol_ver(void); /* ← 厂商协议版本 */
void     bridge_rf24_on_link_lost(void);  /* ← 厂商断链回调转事件 */
int      bridge_rf24_send(const uint8_t *payload, uint8_t len); /* → 厂商发送接口，HID 报文直传 */
void     bridge_rf24_set_report_period_ms(uint16_t ms);         /* 目标 1ms；受厂商用例能力限制 */
void     bridge_rf24_deinit(void);
```

> 落地要点：厂商用例的**收发主体、配对、跳频、重传全部保留原样**；本模块只负责（1）把 `bridge_hid` 的报文送进去，（2）把厂商回调翻译成 `EVT_*`。
> 上报周期若厂商用例不开放 1ms 配置，则按厂商能力取值并更新规格 8.2。

### 4.7 `bridge_power.h`

```c
void     bridge_power_init(void);
void     bridge_power_enter_sleep(void);
void     bridge_power_on_wakeup(uint32_t wake_src);
uint16_t bridge_power_get_mv(void);
uint8_t  bridge_power_get_percent(void);
bool     bridge_power_is_charging(void);
void     bridge_power_tick(void);          /* 10s 采样 + 低电告警 */
```

### 4.8 `bridge_gpio.h`

```c
void          bridge_gpio_init(void);
bridge_mode_t bridge_gpio_read_mode_sel(void);
void          bridge_gpio_set_wake(bool active);
void          bridge_gpio_set_link(bool connected);
```

### 4.9 `bridge_cfg.h` / `bridge_cfg.c`

```c
int  bridge_cfg_load(void);
int  bridge_cfg_save(void);
/* 记忆模式 */
int  bridge_cfg_set_memory_mode(uint8_t mode);
int  bridge_cfg_get_memory_mode(uint8_t *mode);
/* 绑定信息整包（由 BLE 栈侧提供 blob，双备份 + CRC 存 EEPROM） */
int  bridge_cfg_store_bond(const void *blob, uint16_t len);
int  bridge_cfg_load_bond(void *blob, uint16_t max_len);
int  bridge_cfg_erase_bond(uint8_t profile);
```

### 4.10 `bridge_test.h`（SRRC 认证专用）

```c
/* 读芯片唯一 MAC，6 字节；用于生成广播名 SK32-3M-XXXX 与配对标识 */
int  bridge_test_get_mac(uint8_t mac[6]);

/* 广播名生成：写入 buf（至少 16 字节），返回实际长度 */
uint8_t bridge_test_build_adv_name(uint8_t *buf, uint8_t mac[6]);

/* 定频测试模式：
 *   mode    = 0 退出测试模式（恢复正常运行）
 *           = 1 连续发射（未调制定频载波 / 定频数据包）
 *   channel = 0~39（BLE 信道）；2.4G 模式按私有协议信道号
 *   power   = 功率档索引（查表映射到 SDK 功率设置，禁止超出申报值）
 * 进入测试模式前必须先 bridge_hid_release_all()，并关闭正常射频 */
int  bridge_test_set_mode(uint8_t mode, uint8_t channel, uint8_t power);

/* 是否处于认证测试模式（测试模式下禁止进入 SLEEP） */
bool bridge_test_is_active(void);
```

### 4.11 `bridge_log.h`（printf 日志开关，D13）

```c
/* 等级（与 BRIDGE_LOG_LEVEL 同值域） */
#define BRIDGE_LOG_OFF     0
#define BRIDGE_LOG_ERROR   1
#define BRIDGE_LOG_WARN    2
#define BRIDGE_LOG_INFO    3
#define BRIDGE_LOG_DEBUG   4

void   bridge_log_init(uint32_t baudrate);       /* 初始化 UART0 日志通道（PB7/PB4） */
void   bridge_log_set_level(uint8_t level);      /* 运行期改等级（立即生效） */
uint8_t bridge_log_get_level(void);

/* 输出；编译期 BRIDGE_LOG_ENABLE=0 时整个宏被移除 */
void   bridge_log_printf(uint8_t level, const char *module, const char *fmt, ...);
```

宏封装（供业务代码使用）：

```c
#if BRIDGE_LOG_ENABLE
  #define LOG_E(mod, fmt, ...)  bridge_log_printf(BRIDGE_LOG_ERROR, mod, fmt, ##__VA_ARGS__)
  #define LOG_W(mod, fmt, ...)  bridge_log_printf(BRIDGE_LOG_WARN,  mod, fmt, ##__VA_ARGS__)
  #define LOG_I(mod, fmt, ...)  bridge_log_printf(BRIDGE_LOG_INFO,  mod, fmt, ##__VA_ARGS__)
  #define LOG_D(mod, fmt, ...)  bridge_log_printf(BRIDGE_LOG_DEBUG, mod, fmt, ##__VA_ARGS__)
#else
  #define LOG_E(mod, fmt, ...)  do {} while (0)
  #define LOG_W(mod, fmt, ...)  do {} while (0)
  #define LOG_I(mod, fmt, ...)  do {} while (0)
  #define LOG_D(mod, fmt, ...)  do {} while (0)
#endif
```

**禁止事项（务必写进代码评审清单）：**

1. `bridge_hid.c` 的 0x10~0x15 转发路径内**不得**出现任何 `LOG_*`
2. 任何 ISR 内不得调用 `bridge_log_printf()`（非阻塞也不建议，改为置标志）
3. **不得**把日志改到 UART1 —— 那是桥↔SK32 的二进制数据面

`bridge_log_init()` 在 `BRIDGE_LOG_ENABLE=0` 时应为空实现，且编译后不产生代码。
运行期 `SET_CONFIG(LOG_LEVEL)` 由 `cmd_set_config()` 转调 `bridge_log_set_level()`；`BRIDGE_LOG_ENABLE=0` 时返回「不支持」错误码。


---

## 5. TMOS 任务与事件

### 5.1 事件位定义（`bridge_proto.h`）

```c
/* 注意：SYS_EVENT_MSG (0x8000) 为系统保留，不可占用 */
#define START_DEVICE_EVT     (1 << 0)
#define UART_RX_EVT          (1 << 1)
#define MODE_SWITCH_EVT      (1 << 2)
#define SLEEP_CHECK_EVT      (1 << 3)
#define BATT_SAMPLE_EVT      (1 << 4)
#define LINK_CHECK_EVT       (1 << 5)
#define PING_TX_EVT          (1 << 6)
#define HID_RELEASE_EVT      (1 << 7)
```

### 5.2 周期任务表

| 事件 | 周期 | 落点函数 |
|---|---|---|
| `PING_TX_EVT` | 1 s | `bridge_uart_ping()` |
| `LINK_CHECK_EVT` | 1 s | `bridge_uart_is_link_alive()` → 失败走 `bridge_mode_on_uart_lost()` |
| `BATT_SAMPLE_EVT` | 10 s（睡眠时 60 s） | `bridge_power_tick()` |
| `SLEEP_CHECK_EVT` | 5 s | `bridge_mode_tick()` |

### 5.3 `Bridge_ProcessEvent` 骨架

```c
uint16_t Bridge_ProcessEvent(uint8_t task_id, uint16_t events)
{
    if (events & SYS_EVENT_MSG) {                  /* 系统消息：BLE 事件 */
        uint8_t *msg = tmos_msg_receive(task_id);
        if (msg) {
            bridge_ble_on_sys_msg(msg);            /* 内部转 bridge_ble_on_* */
            tmos_msg_deallocate(msg);
        }
        return (events ^ SYS_EVENT_MSG);
    }

    if (events & UART_RX_EVT) {
        bridge_frame_t f;
        while (bridge_uart_get_frame(&f)) {
            bridge_proto_dispatch(&f);
        }
        return (events ^ UART_RX_EVT);
    }

    if (events & MODE_SWITCH_EVT)  { bridge_mode_on_switch_req(); return (events ^ MODE_SWITCH_EVT); }
    if (events & SLEEP_CHECK_EVT)  { bridge_mode_tick();          return (events ^ SLEEP_CHECK_EVT); }
    if (events & BATT_SAMPLE_EVT)  { bridge_power_tick();         return (events ^ BATT_SAMPLE_EVT); }
    if (events & LINK_CHECK_EVT)   { bridge_check_uart_link();    return (events ^ LINK_CHECK_EVT); }
    if (events & PING_TX_EVT)      { bridge_uart_ping();          return (events ^ PING_TX_EVT); }
    if (events & HID_RELEASE_EVT)  { bridge_hid_release_all();    return (events ^ HID_RELEASE_EVT); }

    return 0;
}
```

---

## 6. 状态机落点

### 6.1 迁移表 → 函数

| 当前状态 | 事件 | 目标状态 | 实现函数 |
|---|---|---|---|
| 任意 | 上电/复位 | 按 `MODE_SEL` | `bridge_mode_init()` |
| 任意 | VBUS 插入 | `MODE_USB` | `bridge_mode_on_vbus(true)` |
| `MODE_USB` | VBUS 移除 | 记忆模式 | `bridge_mode_on_vbus(false)` |
| `MODE_BLE` | `SET_MODE(2G4)` | `MODE_2G4` | `bridge_mode_set()` → `bridge_ble_deinit()` + `bridge_rf24_init()` |
| `MODE_BLE` | `PAIR_START` | `MODE_BLE_PAIR` | `cmd_pair_start()` → `bridge_ble_start_pairing()` |
| `MODE_BLE_PAIR` | 配对成功 | `MODE_BLE` | `bridge_ble_on_connected()` |
| `MODE_2G4` | 链路丢失 > 3s | `MODE_2G4_PAIR` | `bridge_rf24_on_link_lost()` |
| `MODE_BLE_PAIR` / `MODE_2G4_PAIR` | 配对超时 > `BRIDGE_PAIR_TIMEOUT_S`（60s） | `MODE_BLE` / `MODE_2G4` | `bridge_mode_poll()` → `port_radio_pair_stop()` + `bridge_evt_pair_result(1, profile)` |
| 任意 | 空闲 > `T_idle` | `MODE_SLEEP` | `bridge_mode_tick()` → `bridge_power_enter_sleep()` |
| `MODE_SLEEP` | 唤醒源 | 原模式 | `bridge_power_on_wakeup()` |
| 任意 | `SET_TEST_MODE(1)` | `MODE_TEST` | `cmd_set_test_mode()` → `bridge_hid_release_all()` + 关正常射频 + `bridge_test_set_mode()` |
| `MODE_TEST` | `SET_TEST_MODE(0)` | 按 `MODE_SEL` | `cmd_set_test_mode()` → 退出定频，重新初始化射频 |

### 6.2 切换事务的强制顺序

`bridge_mode_set()` **必须**按此顺序，否则会卡键或丢键：

```
1. bridge_proto_send_event(EVT_STATE, ...)   // 先告知 SK32
2. bridge_hid_release_all()                  // 释放所有键（可延时 20ms 保证发出）
3. 关闭当前射频（bridge_ble_deinit / bridge_rf24_deinit）
4. 初始化目标射频
5. bridge_gpio_set_wake(true)                // 通知 SK32 切换完成
6. 若 persist: bridge_cfg_set_memory_mode()
```

### 6.3 优先级仲裁

```c
/* bridge_mode_set() 入口处的守卫 */
if (bridge_vbus_present && m != BRIDGE_MODE_USB && m != BRIDGE_MODE_OTA)
    return -ERR_VBUS_PRIORITY;      /* VBUS 在场时拒绝一切无线模式 */
```

---

## 7. 中断与缓冲

| 中断 | 优先级 | 落点 | 动作 |
|---|---|---|---|
| UART RX | 最高 | `UARTx_IRQHandler` | `bridge_uart_isr_feed(byte)` → `tmos_set_event(task, UART_RX_EVT)` |
| `MODE_SEL` GPIO | 次高 | `GPIOx_IRQHandler` | `tmos_set_event(task, MODE_SWITCH_EVT)` |
| ADC | 低 | `ADC_IRQHandler` | 置标志，任务中取值 |
| BLE（BB/LLE） | 由栈管理 | SDK 内部 | 经 `SYS_EVENT_MSG` 转 `bridge_ble_on_*` |
| WDOG | — | 硬件复位 | — |

**环形缓冲规格**：容量 ≥ 4× 最大帧（约 300 B），单生产者（ISR）单消费者（任务），用读写指针 + 计数器，不加锁。

**缓冲设计要求（D14）：**

| 缓冲 | 容量 | 说明 |
|---|---|---|
| RX 环形缓冲 | 256~300 B | @921600 单帧占线 ≤0.26ms，可吸收数毫秒的连续输入，足够覆盖一次任务调度抖动 |
| TX 待发槽 | **1 帧**（16~24 B，非队列） | 背压保护：忙时新 HID 帧**覆盖**旧值，只发最新状态；控制命令单独占 1 个待发槽排队 |
| RX 帧内超时 | 5 ms（不变） | @921600 单字节 10.9µs，5ms 内可容纳约 460 字节，足以正确切分半帧 |

---

## 8. 初始化顺序（`Main.c`）

```c
int main(void)
{
    SetSysClock(&HSECFG);          /* 1. 时钟 */
    bridge_log_init(BRIDGE_LOG_BAUD); /* 1b. 日志（D13；ENABLE=0 时为空实现） */
    LOG_I("main", "boot, ver=%s", BRIDGE_FW_VERSION);

    bridge_gpio_init();            /* 2. 控制引脚（先读 MODE_SEL） */
    bridge_uart_init(2000000);     /* 3. 桥接串口 */
    bridge_cfg_load();             /* 4. 读配置与绑定元数据（含日志等级） */
    bridge_mode_init();            /* 5. 依 MODE_SEL + 记忆定初始模式 */

    CH58x_BLEInit();               /* 6. BLE 协议栈初始化 */
    HAL_Init();
    GAPRole_PeripheralInit();
    GAPBondMgr_Init();
    GATTServApp_Init();
    HidDev_Init();                 /* HID 设备注册 */
    bridge_ble_init();             /* 7. 桥对 BLE 的封装 */

    if (bridge_mode_get() == BRIDGE_MODE_2G4 ||
        bridge_mode_get() == BRIDGE_MODE_2G4_PAIR) {
        bridge_rf24_init();        /* 8. 按需启动 2.4G（转调厂商用例，D11） */
    }
    bridge_power_init();           /* 9. 电源/电量 */

    tmos_start_task(bridgeTaskId, START_DEVICE_EVT, 1600);  /* 10. 启动周期任务 */
    tmos_start_reload_task(bridgeTaskId, PING_TX_EVT,     1600);
    tmos_start_reload_task(bridgeTaskId, LINK_CHECK_EVT,  1600);
    tmos_start_reload_task(bridgeTaskId, BATT_SAMPLE_EVT, 16000);
    tmos_start_reload_task(bridgeTaskId, SLEEP_CHECK_EVT,  8000);

    while (1) {
        bridge_uart_poll();        /* 消费 RX 事件 */
        bridge_proto_poll();       /* 处理待发送事件队列 */
        if (bridge_can_sleep()) {
            bridge_power_enter_sleep();   /* 内部调用 CH58x_LowPower() */
        }
    }
}
```

---

## 9. 编译配置（`bridge_cfg.h`）

> 引脚与封装依据：[ch582f_pinmap.md](./ch582f_pinmap.md)（CH582F / QFN28_4x4，20 GPIO）。

```c
/* ---------- 版本（GET_VERSION 返回，日志启动行打印） ---------- */
#define BRIDGE_FW_VERSION         "0.1.0"

/* ---------- 目标封装 ---------- */
#define BRIDGE_CHIP_QFN28         1     /* CH582F；0 = CH582M QFN48 备选 */

/* ---------- UART（D2：UART1 = PA8/PA9；D14：921600） ---------- */
#define BRIDGE_UART_INST          UART1   /* PA8=RXD1(pin6) / PA9=TXD1(pin7) */
#define BRIDGE_UART_BAUD          921600
#define BRIDGE_MAX_PAYLOAD        64
/* UART2 不可用：其映射脚 PB22/PB23 中 PB23 为 RST#（见 pinmap 第 2 章） */

/* ---------- 时序约束（D14，见规格 6.3） ---------- */
#define BRIDGE_FRAME_MIN_GAP_MS   5       /* 两帧最小间隔：保守上界（16B@921600 仅 0.17ms） */
#define BRIDGE_TX_LATEST_ONLY     1       /* 1 = TX 忙时只保留最新 HID 帧（背压保护） */
#define BRIDGE_HID_DEDUP          1       /* 1 = 相同 HID 报文抑制（必须开启） */
#define BRIDGE_HID_FRAME_BYTES    16      /* 8B 键盘报文整帧长度，用于带宽估算 */

/* ---------- 引脚 ---------- */
/* 说明：WCH SDK 中 GPIOA/GPIOB 引脚定义同名（GPIO_Pin_4 等），
 *       故宏中必须同时携带端口，勿只写 GPIO_Pin_n。 */
#define BRIDGE_PIN_GPIOA          GPIOA
#define BRIDGE_PIN_GPIOB          GPIOB

#define BRIDGE_PIN_MODE_SEL0      GPIO_Pin_12   /* PB12 (pin 11) 输入 */
#define BRIDGE_PIN_MODE_SEL1      GPIO_Pin_13   /* PB13 (pin 10) 输入 */
#define BRIDGE_PIN_WAKE           GPIO_Pin_5    /* PA5  (pin 24) 输出，高有效 */
#define BRIDGE_PIN_LINK           GPIO_Pin_13   /* PA13 (pin 27) 输出，可选 */
#define BRIDGE_PIN_CHRG           GPIO_Pin_15   /* PA15 (pin 25) 输入 */
#define BRIDGE_PIN_BAT_ADC        GPIO_Pin_14   /* PA14 (pin 26) ADC ch4 (AIN4) */
#define BRIDGE_ADC_CH_BAT         4             /* BAT_ADC 对应 ADC 通道 */

/* 量产 ISP 配置引脚：必须 10k 上拉，上电不得被拉低，否则进 BootLoader */
#define BRIDGE_PIN_ISP_CFG        GPIO_Pin_22   /* PB22 (pin 17) */

/* 调试串口 = printf 日志通道（D13），非必需但建议留焊盘 */
#define BRIDGE_PIN_DBG_TX         GPIO_Pin_7    /* PB7  (pin 14) UART0 TXD */
#define BRIDGE_PIN_DBG_RX         GPIO_Pin_4    /* PB4  (pin 15) UART0 RXD */

/* 未使用 GPIO 统一配置为输入+下拉（省电），见 pinmap 第 6 章 */
#define BRIDGE_UNUSED_GPIO_MODE   GPIO_ModeIN_PD

/* ---------- 超时 ---------- */
#define BRIDGE_PING_PERIOD_MS     1000
#define BRIDGE_LINK_LOST_CNT      3
#define BRIDGE_LINK_TIMEOUT_S     3
#define BRIDGE_IDLE_TO_SLEEP_S    30
#define BRIDGE_PAIR_TIMEOUT_S     60

/* ---------- 特性开关（对应 pinmap 第 0 章 D1~D13） ---------- */
#define BRIDGE_FEATURE_NKRO_BLE   0       /* D3：BLE 模式禁用 NKRO，16B 降级为 6KRO */
#define BRIDGE_RF24_REPORT_HZ     1000    /* D5：2.4G 上报率 1000 Hz（1 ms） */
#define BRIDGE_FEATURE_ADV_NAME   "SK32-3M-"   /* D6：后接 MAC 低 16 位十六进制大写 */
#define BRIDGE_FEATURE_TOUCHKEY   0       /* D10：不使用触摸按键 */
#define BRIDGE_FEATURE_EXT_FLASH  0       /* D8：不使用外挂 Flash */
#define BRIDGE_FEATURE_SRRC_TEST  1       /* D9：提供定频测试模式（0x0B） */
#define BRIDGE_FEATURE_RF24_VENDOR 1      /* D11：2.4G 走厂商用例，不自研协议 */
#define BRIDGE_FEATURE_OTA_SINGLE 1       /* D12：单一应用区 APP_A + Stage 暂存，由常驻 BOOT 搬运（非 A/B 双应用区，可回退） */

/* ---------- printf 日志（D13） ---------- */
#define BRIDGE_LOG_ENABLE         0       /* 0 = 全部日志代码被移除（量产）；1 = 启用 */
#define BRIDGE_LOG_LEVEL          4       /* 0=off 1=ERROR 2=WARN 3=INFO 4=DEBUG（仅 ENABLE=1 有效） */
#define BRIDGE_LOG_UART_INST      UART0   /* 必须 UART0（PB4/PB7）；严禁 UART1 */
#define BRIDGE_LOG_BAUD           921600
#define BRIDGE_LOG_BUF_SIZE       256     /* SRAM 占用；ENABLE=0 时不存在 */
```

**两个构建目标**（同一份源码，靠宏区分）：

| 目标 | 关键宏 | 说明 |
|---|---|---|
| `debug` | `BRIDGE_LOG_ENABLE=1`, `BRIDGE_LOG_LEVEL=4` | 开发/认证问题定位用 |
| `release` | `BRIDGE_LOG_ENABLE=0`, `BRIDGE_LOG_LEVEL=0` | 量产出货，日志零开销 |


Dongle 侧（同一份代码，编译开关区分）：

```c
#define BRIDGE_ROLE_DONGLE        1       /* 键盘侧为 0 */
/* Dongle：USB1 走 PB10=UD-(pin13) / PB11=UD+(pin12)；
 *         LED_STAT=PA4(pin23)，PAIR_KEY=PA5(pin24)（见 pinmap 第 4 章）
 *         键盘侧无 USB HID 需求，这些引脚不作 USB 用途 */
#define BRIDGE_PIN_LED_STAT       GPIO_Pin_4    /* PA4 */
#define BRIDGE_PIN_PAIR_KEY       GPIO_Pin_5    /* PA5 */
```

LD 文件约束（与规格 10.4 对应）：

- 必须定义 `LIB_FLASH_BASE_ADDRESSS`，把预编译 BLE 固定库定位到固定地址
- 必须定义 `CH58xBLE_ROM` 宏，表示使用固定库
- **D12 已定为「常驻 BOOT + Stage 暂存」**：不划分 A/B 双应用区；分区为 `BOOT@0x0(8K) + APP_A@0x2000(216K) + Stage@0x38000(216K)`（顶部余 8K），见 `port/ch58x/bridge_ota_layout.h`
- 应用可用 **216KB**（非旧稿「约 ≤300KB」）；Stage 为暂存区、不占应用可见空间
- **可回退**：APP_A 在搬运完成前始终未动；先写 `IAP` 再搬运，断电则下次上电自动重试；取消/失败/空闲超时（60s）写 `A` 回原 App。ROM ISP（`ENTER_BOOTLOADER`/PB22）仅作为应用+BOOT 均损坏时的最终兜底
- 投板前实测 `debug` 与 `release` 两个构建的占用（≤ 216KB），`release` 需保留 **≥15%** 余量

---

## 10. 实施里程碑

| 里程碑 | 内容 | 验证方式 |
|---|---|---|
| M0 | **`bridge_log` + 双构建目标**（debug/release） | 串口看到分级日志；`release` 构建日志零输出、无额外占用 |
| M1 | `bridge_uart` + `bridge_proto` 骨架 | 串口助手回环，命令/应答/CRC 正确 |
| M2 | `bridge_hid` + `bridge_ble` 单主机透传 | 打字母，PC 正确输入 |
| M3 | `bridge_mode` 状态机 + 多主机切换 | 4 主机切换无卡键 |
| M4 | `bridge_rf24`（厂商用例并入）+ Dongle 配对 | 2.4G 端到端延迟往返 ≤10ms / 上行 ≤4ms（D14）；确认厂商用例可配 1ms 射频上报 |
| M5 | `bridge_power` 休眠 + 电量 | 待机电流、电量上报正确 |
| M6 | **常驻 BOOT OTA（Stage 暂存 + 搬运）+ 异常处理 + 看门狗** | 升级成功进新应用；故意断电后上电自动重试搬运；取消/超时回退原 App；ROM ISP 兜底可恢复 |
| M7 | 与 SK32 联调 | 通过规格第 14 章全部 KPI；**专项：连续快速打字 10 秒无滞后/无丢键（D14 带宽验证）** |
| M8 | **SRRC 认证准备**：`GET_MAC` + 广播名生成 + `SET_TEST_MODE` 定频发射 | 频谱仪确认频点/功率/带宽符合申报值；确认厂商 2.4G 用例的定频能力 |

---

## 11. 落地风险

1. **多主机绑定能力**：SDK 的 `GAPBondMgr` 是否原生支持 4 组绑定需实机确认；若只支持单绑定，需自行扩展 EEPROM 存多个配对 blob（`bridge_cfg_store_bond` 已为此预留）。
2. **固定库地址**：`LIB_FLASH_BASE_ADDRESSS` 必须与所选 SDK 版本一致，配错会导致上电即崩。
3. **下载配置引脚**：已定 **PB22（pin 17，10k 上拉）**，见 pinmap 第 3.1/6 章；上电被拉低会误进 BootLoader，PCB 上必须保证默认高。
4. **NKRO 降级**：BLE 侧 16 字节报文兼容性差，`bridge_hid_send_nkro()` 在 BLE 模式下必须降级并保证不丢键。
5. **UART 中断优先级**：必须高于射频相关处理，否则高频上报时会积压导致延迟抖动。
6. **切换事务顺序**：第 6.2 节的 6 步不可调换，尤其 `HID_RELEASE_ALL` 必须在关射频之前。
7. **SRRC 定频模式的实现依赖**：BLE 侧可用标准 HCI 测试接口实现定频；**2.4G 走厂商用例（D11），其定频能力取决于厂商是否暴露接口**，需先向 WCH FAE 确认；若 2.4G 侧无法定频，认证须以厂商提供的产测固件配合完成。
8. **QFN28 引脚余量**：实测已用 12 个 GPIO，仅余 3~5 个备用；若后续需加 EEPROM、传感器或指示灯，须先复核 pinmap 第 3.4 节。
9. **OTA 可回退但仍需保证恢复通道（D12）**：本方案 APP_A 在搬运完成前不动、断电自动重试、取消/超时可回退，但**应用+常驻 BOOT 均损坏时**只能靠 ROM ISP 恢复。因此必须保证 ISP 通道在主循环任何状态下都可进入（含射频异常、看门狗刚复位后）。
10. **厂商 2.4G 用例的耦合度（D11）**：并入厂商用例后，其全局变量/中断向量可能与桥接模块冲突（如抢占 UART0、定时器、时钟配置）。并入时须逐项核对资源占用，且**上报周期 1ms 的达成以厂商能力为准**，不可为此改动厂商射频时序。
11. **日志误占桥串口（D13）**：printf 若被错误重定向到 UART1 会直接破坏数据面且表现为「随机丢键/乱码」，代码评审须把「日志只用 UART0」列为硬性检查项。
12. **串口带宽（D14）**：波特率已统一提升至 **921600**（单帧占线 ≤0.26ms、可用带宽 92160 B/s），带宽不再是瓶颈。但 SK32 侧仍需做「事件驱动 + 去重 + 5ms 帧间隔」，否则快速连打仍可能积压造成输入滞后。**M7 联调仍包含「连续快速打字 10 秒无滞后」的专项测试**。
13. **RX 帧内超时与波特率的匹配（D14）**：5ms 半帧超时需保证任务调度不会被射频/Flash 操作阻塞超过 5ms（@921600 下单帧占线仅 ≤0.26ms，阻塞超过 5ms 会误判半帧丢弃）。射频发送与 Flash 操作期间需屏蔽半帧超时判定或延长至 10ms。

---

## 12. 落地实现（as-built）与主机端验证

> 本节记录**代码实际落地形态**与第 2/5/8 章设计稿的差异，以及可复现的验证方式与结果。
> 凡与本节冲突处，以本节（即 `wireless/ch582f_fw/` 的实际代码）为准。

### 12.1 实际文件布局（对第 2.1 / 2.2 章的修订）

| 实际文件 | 对应设计稿 | 说明 |
|---|---|---|
| `core/bridge_{app,uart,proto,mode,hid,power,cfg,test,log,ringbuf,crc16}.c` | `APP/bridge_*.c` | 改名并新增 `bridge_app.*`（主循环编排）、`bridge_ringbuf.*`、`bridge_crc16.*`；**`bridge_gpio.*` 未单列**，GPIO 并入 `bridge_port.h` 的 `port_gpio_*` |
| `port/ch58x/bridge_port_ch58x.c` | `bridge_ble.c` / `bridge_rf24.c` / `bridge_gpio.c` 的 SDK 侧 | 实现 `core/bridge_port.h`：串口数据面、时间、NV、ADC、GPIO、系统控制、主循环 |
| `port/ch58x/bridge_radio_ch58x.c` | `bridge_ble.c` + `bridge_rf24.c` | 射频统一入口 `port_radio_*`，BLE 与 2.4G 由 `mode` 参数区分 |
| `port/ch58x/bridge_ch58x_callbacks.c` | `bridge_ble.c` / `bridge_rf24.c` 的回调 | **新增拆分**：`sdk_ble_on_led_report` / `sdk_rf24_on_recv` / `sdk_rf24_on_link` / `sdk_usb_on_led_report` 四个回调**不含 SDK 头**，抽出以便纳入主机验证 |
| `port/ch58x/bridge_ch58x_sdk.{c,h}` | — | **新增**：BLE/系统原语边界（三个 SDK 头边界文件之一），`#include` WCH SDK 头 |
| `port/ch58x/bridge_rf24_ch58x.c` | `bridge_rf24.c` 的 SDK 侧 | **新增**：2.4G 射频接入，按厂商 `EVT/EXAM/BLE/RF_PHY` 例程实现 `sdk_rf24_*`（`RF_RoleInit`/`RF_Config`/`RF_Tx`/`RF_Rx`/`RF_Shut` + TMOS 任务上下文投递 + 1s 心跳/连续 3 次无收断链） |
| `port/ch58x/bridge_usb_ch58x.c` | — | **新增**：Dongle USB 复合设备（3 接口：键盘 / 辅助键 / VIA Raw HID），按厂商 `EVT/EXAM/USB/Device/CompoundDev` + `BLE_USB` 例程实现 `sdk_usb_*` |
| `board/main.c` + `board/{keyboard,dongle}/bridge_board.h` | `Main.c` + `APP/include/bridge_cfg.h` | 两目标共用一份 main；板级差异全部由 `-Iboard/$(TARGET)` 决定 |

> 含 WCH SDK 头的**边界文件共 3 个**：`bridge_ch58x_sdk.c` / `bridge_rf24_ch58x.c` /
> `bridge_usb_ch58x.c`；三者均**排除**在 `test_host` 构建外（`test_host/Makefile` 的
> `SDK_BOUNDARY`），由 `sdk_stub.c` 顶替。

**第 2.3 章分层关系**：`bridge_proto` → `bridge_hid` → `port_radio_*` 的规则不变，
但**改为「core 定义接口 / port 实现」的单向依赖**（`bridge_port.h` 为唯一边界），
不再有 core 直接调用射频 API 的路径。

### 12.2 与第 5 / 8 章的差异：不使用 TMOS 任务驱动桥逻辑

设计稿以 `Bridge_ProcessEvent`（TMOS 任务）驱动桥逻辑。落地改为**主循环直调**：

- `board/main.c`：`sdk_sys_init()` → `bridge_ch58x_init()` → `for(;;) bridge_ch58x_loop()`
- `bridge_ch58x_loop()` = `sdk_tmos_process()`（只服务 BLE 协议栈）→ `port_uart_tx_poll()` →
  `pair_key_poll()`（仅 Dongle）→ `bridge_app_poll()`
- 理由：即便 921600 下单帧占线仅 ≤0.26ms，再叠加 TMOS 调度延迟仍不利于按键低延迟（D14）。
  因此第 5 章的事件位定义、`Bridge_ProcessEvent` 骨架**在本工程中未采用**，
  心跳/链路检测/模式轮询改由 `bridge_app_poll()` 内的定时判定完成。

### 12.3 主机端三层验证（可复现）

| 层 | 替身 | 覆盖内容 | 产物 |
|---|---|---|---|
| core | `test_host/port_host.c` 顶替**整个 port 层** | 协议帧机、D14 带宽预算、HID 去重/NKRO 降级、模式切换 6 步事务、日志开关 | `build/<role>/bridge_test_<role>` |
| port | `test_host/sdk_stub.c` 顶替 **WCH SDK** | 串口 FIFO 背压与 ISR 读空、NV 256B 读-改-写与邻槽保护、ADC 分压、GPIO 模式/极性、MAC/复位/进 ISP、射频参数与事件翻译、厂商回调翻译、休眠判定、配对键去抖、`port_radio_pair_stop` 参数/状态校验与幂等 | `build/<role>/bridge_port_test_<role>` |
| SDK | 无（三个边界文件 `bridge_ch58x_sdk.c` / `bridge_rf24_ch58x.c` / `bridge_usb_ch58x.c` 均无法主机编译） | 靠文件内 `TODO(核实)` 清单 + 真机比对，见 12.5 | — |

```bash
make -C wireless/ch582f_fw/test_host test          # keyboard 角色
make -C wireless/ch582f_fw/test_host DONGLE=1 test # dongle 角色
```

`test_host/board/bridge_board.h` 按角色**精确镜像** `board/{keyboard,dongle}/bridge_board.h`
的被引用宏（取值完全一致），故不可为「让测试通过」而放宽该文件——否则主机测试
会覆盖固件上不存在的分支（第 5 条保真约定）。

### 12.4 验证结果（最近一次全量）

工具链：MSYS2 `gcc 14.2.0` + `make 4.4.1`（`D:\QMK_MSYS`）。

| 角色 | core | port | 编译告警 |
|---|---|---|---|
| keyboard | 385 passed / 0 failed | 332 passed / 0 failed | 0 |
| dongle | 363 passed / 0 failed | 265 passed / 0 failed | 0 |

目标侧双构建（工具链 `riscv-none-embed-`，`make -C wireless/ch582f_fw both`）：

| 目标 | text | data | bss |
|---|---|---|---|
| keyboard | 167868 | 1484 | 10684 |
| dongle | 170292 | 1480 | 11292 |

验证过程中定位并修复的**真实缺陷**（均会阻断目标侧构建或功能）：

1. `port/ch58x/bridge_port_ch58x.c` 引用了**不存在**的 `bridge_uart_tx_busy()`
   （全工程无定义）→ 目标侧链接失败；改为 `port_uart_tx_busy()`。
2. `port/ch58x/bridge_port_ch58x.c` 调用 `bridge_app_init()` / `bridge_app_poll()`
   **未包含 `bridge_app.h`** → C99 隐式声明报错；补 include。
3. `port/ch58x/bridge_ch58x_callbacks.c` 使用 `BRIDGE_RADIO_EVT_*` **未包含 `bridge_mode.h`**；补 include。
4. `port/ch58x/bridge_ch58x_sdk.c` 同样使用 `BRIDGE_RADIO_EVT_*` **未包含 `bridge_mode.h`**
   → 该文件**不参与主机编译**，此缺陷只会在目标侧构建时暴露；补 include。
5. `port/ch58x/bridge_port_ch58x.c` 的 `port_enter_bootloader()` 早期只调 `sdk_reset()`，
   使 SDK 侧专用的 `sdk_enter_bootloader()` 成为死代码，恢复通道实际缺失 → 一度改为直调它。
   **当前 as-built（键盘 UART OTA 实测通过后）按角色分流**：键盘侧写标志 `UART_IAP` + 软复位，
   进**本工程的常驻 BOOT**（`iap_uart_ota_run`，非 ROM ISP）；Dongle 侧 `PB22` 拉低 +
   `sdk_enter_bootloader()`（进 ROM ISP，保留为应用+BOOT 均损坏时的最终兜底）。

### 12.5 尚未验证部分（需真机）

三个边界文件（`bridge_ch58x_sdk.c` / `bridge_rf24_ch58x.c` / `bridge_usb_ch58x.c`）
均无法在 PC 上编译（依赖 WCH SDK 头与预编译库），其中所有 `TODO(核实)` / `TODO(厂商用例)`
必须在并入所选 CH583 EVT 版本时逐项比对：

- 头文件名、寄存器名、外设函数名（含 `HidDev_Report` 所属的 HID 服务头）
- `sdk_ble_mac()` 的 API 名（`GAP_GetDevAddress` / `BLE_GetMACAddr`）
- `LIB_FLASH_BASE_ADDRESSS` 必须与所选 `LIBCH58xBLE.a` 一致（配错会上电即崩）
- 2.4G `sdk_rf24_*` **已落地**（`bridge_rf24_ch58x.c`）：按厂商 `RF_PHY` 例程
  `RF_Config` + `RF_Tx`/`RF_Rx`/`RF_Shut`，TMOS 任务上下文投递，1s 心跳 + 连续 3 次无收断链；
  保持 `LLE_MODE_AUTO`。**WCH 硬件跳频未启用**（`BRIDGE_FEATURE_RF24_HOP=0`，见 §12.6）：
  厂商 `RF_FrequencyHoppingTx/Rx` 是 TX 全通道遍历 / RX 单频点监听的非对称骨架、API 不带应用载荷，
  无法承载本桥双向数据面，故保持**固定信道**（`RF24_CHANNEL`）；该宏仅作将来接入自定义跳频的
  预留开关。`core` 无需改动
- **2.4G 配对 / 绑定已落地**（见 §12.6「2.4G 配对 / 绑定」）：数据面帧升级为
  `[type][link_id LE(2)][seq][payload]`（帧级序号去重，结论3），未绑定设备数据帧互不可达
  （防同环境多套串扰），绑定信息（密钥 + 对端 ID + 随机数）掉电保存（NV 槽 `BRIDGE_NV_ID_RF24_BOND`），
  并据其派生 access address（结论4）
- **Dongle 角色修正**（core 板级差异）：Dongle 无 SK32 上游 → 不做 UART 心跳/断链判定、
  禁止空闲入睡；上电默认模式 2G4（旧固件记忆为 BLE 也强制回 2G4）
- Dongle `sdk_usb_*` **已落地**（`bridge_usb_ch58x.c`）：按厂商 `CompoundDev` + `BLE_USB`
  例程实现 USB 复合设备（IF0 键盘 EP1 / IF1 辅助键 EP2 / IF2 VIA Raw HID EP3），
  **尚未经真机枚举验证**（VID/PID 须与目标键盘 `via.json` 一致，见 spec §8.4）
- **已落地（原"桩"项已补齐）**：
  - BLE GATT 透传服务（`sdk_ble_tunnel_send`，`bridge_ch58x_sdk.c`）：自定义服务 0xFFF0
    （TX 0xFFF1 notify / RX 0xFFF2 write + CCCD），手写 `gattAttribute_t[]`，
    参考 `EVT/EXAM/BLE/BLE_USB/Profile/ble_usb_service.c`
  - SRRC 定频（`sdk_ble_test_mode`）：`LL_SetTxPowerLevel` + `LL_SingleChannel` + `LL_TestEnd`，
    参考 `EVT/EXAM/BLE/Direct_Test_Mode/APP/RF_dtm.c`
  - 广播名带 MAC（`sdk_ble_set_adv_name_with_mac`）：基准名 + `GGS_SetParameter`
  - **运行时改标识**：`CMD_SET_NAME` 0x1F（target=BLE 键盘侧本地生效 / target=USB Dongle
    侧本地生效，另一侧经 2.4G 中继）、`CMD_SET_USB_ID` 0x20（写设备描述符 VID/PID 后重枚举）；
    键盘侧接收后中继给 Dongle，Dongle 侧按同类型前缀本地生效
  - **上电应用**：`bridge_app_init()` 读回持久化的名称 / VID-PID 下发端口层
    （`app_apply_ident()`，仅在与编译期默认值不同时才下发，避免默认配置下多余 USB 重枚举）
  - **外部电量**：`CMD_SET_BATTERY` 0x1C / `SET_CONFIG` key 0x05 → `BRIDGE_BATT_SRC_EXTERNAL`，
    由 SK32 上传（`bridge_power.c` 已按该源选择）
- 目标侧构建：本仓库已内置 SDK submodule（`sdk/ch583` @ bd508ad7），
  工具链需 `riscv-none-embed-`（本机 MounRiver Studio2）或 `riscv-wch-elf-`；
  M0~M8 的实机验证项（第 10 章）仍全部待执行。

### 12.6 设备标识 / 外部电量 / 跳频 决策（as-built）

- **设备标识（名称 / VID-PID）**
  - `CMD_SET_NAME` 0x1F：payload `[target][name...]`，`target` 0=BLE、1=USB，
    `name` ≤ `BRIDGE_NAME_MAX`(21)。键盘侧 BLE 名本地生效；USB 名在键盘侧返回
    `BRIDGE_ERR_UNSUPPORTED` → `relay_cfg()` 经 2.4G 中继给 Dongle。Dongle 侧反之。
  - `CMD_SET_USB_ID` 0x20：payload `[vid LE(2)][pid LE(2)]`，写设备描述符
    `s_dev_desc[8..11]` 后断开 D+ 上拉重枚举（`usb_reenumerate()`，约 200ms）。
  - 持久化：名称存 NV 槽 `BRIDGE_NV_ID_NAMES`（blob，`[0]=ble_len [1]=usb_len`），
    VID/PID 随 `bridge_cfg_t` 存 `BRIDGE_NV_ID_CFG`。
  - 上电应用：`bridge_app_init()` → `app_apply_ident()`，仅在持久值与编译期默认值
    （`BRIDGE_BLE_NAME_DEFAULT` / `BRIDGE_USB_NAME_DEFAULT` / `BRIDGE_USB_VID` /
    `BRIDGE_USB_PID`）不同时才下发，避免默认配置下每次上电多一次 USB 重枚举。
- **外部电量**：`CMD_SET_BATTERY` 0x1C（cycle 内下发电量百分比）或
  `SET_CONFIG` key 0x05 置 `BRIDGE_BATT_SRC_EXTERNAL`(2)；`bridge_power.c` 在
  `AUTO` 下"外部有效则用外部"，在 `EXTERNAL` 下仅用 SK32 上传值，本地 ADC 不参与。
- **2.4G 跳频：未启用**（`BRIDGE_FEATURE_RF24_HOP=0`，`RF24_HOP_PERIOD=0`，仅作将来接入自定义跳频的
  预留开关）。厂商 `RF_PHY_Hop` 例程中 `RF_FrequencyHoppingTx(resendCount)` /
  `RF_FrequencyHoppingRx(timeoutMS)` 均**不带应用载荷**，是 TX 端每周期全通道遍历、RX 端每周期
  单频点监听的**非对称单向**骨架；WCH 官方（论坛 thread-132278）明确「一跳多的绑定/跳频/透传需
  自己写算法」。故本层保持**固定信道**（`RF24_CHANNEL`）+ `LLE_MODE_AUTO` 自动 ACK/重传，
  以重传 + 帧级序号去重 + 绑定派生 AA 实现抗干扰，`core` 不改。
  - **更正**：早期文档记为「跳频已启用（=1）、HopPeriod=16」，与代码不符——`bridge_rf24_ch58x.c`
    实际 `cfg.HopPeriod=RF24_HOP_PERIOD=0`、`cfg.HopIndex=0`，运行时 diag `hop=0`，该结论作废。
  - **LSE**：外部 32.768kHz 晶振（`CLK_OSC32K=0`，D15）作 RTC / TMOS 时基使用；同步跳频若将来
    实现，可再以其为同步基准（当前无此用途）。
  - **PHY 速率 A/B（结论5，已实测定论）**：现采用 `LLE_MODE_PHY_2M`（`bridge_rf24_ch58x.c`，**双端同码**）。
    早期曾按参照工程切 2M 得到丢包 **1.67%**（`tx_done_fail≈53k`，首次 TX 大量失败、全靠重传兜底）、
    回退 1M 后 0.85%，故结论为「2M 负优化、保留 1M」——**该结论作废**：彼时尚未落地帧级序号去重 +
    抢收自愈修复，重传导致的重复/丢帧未被正确统计。修复后复测 2M 往返丢包 **0.01%**（优于 1M 的 0.02%）、
    往返 8.6ms / 上行 3.6ms，2M 空口时间减半正式生效。
- **2.4G 配对 / 绑定**（`core/bridge_rf24pair.{c,h}` + `bridge_rf24_ch58x.c` 帧层）：
  - 帧格式：数据面 `[type][link_id LE(2)][seq][payload]`（线上帧头 4B）；`link_id` = `CRC16(绑定密钥)`，
    配对帧 `link_id=0xFFFF`。收包侧在 RF 层校验：未绑定 / 密钥不符的帧丢弃且**不刷新心跳**
    （防陌生射频撑活链路）。
  - **帧级序号去重（结论3）**：发送方每入队一帧 `seq++`（重传不改 seq），收侧记录上一交付
    `seq`，同序号重复帧直接丢弃。用于消除「ACK 丢失 → 发送方重传 → 收侧重复交付」的重复上报；
    与 `link_id` 组成「序号 + 密钥」双重校验（对照参照工程 3modekbd 的 `num + magicword`）。
  - **绑定派生 access address（结论4）**：两端用绑定记录 `key/peer_id/rand` 经
    `bridge_rf24pair_access_addr()` 派生唯一的 32bit AA（两端逐字节相同 → 派生值必然一致）；
    `rf24_arm_rx()` 每次接收前按绑定状态在「默认 `0x71764129` ↔ 派生 AA」间运行时切换
    （`RF_Shut`+`RF_Config`）。未绑定用默认 AA（配对帧须互通），绑定后各套设备 AA 不同，
    进一步降低同频互扰（对照参照工程用配对 MAC 作 accessAddress）。
  - 配对流程（键盘发起 / Dongle 应答）：键盘 `CMD_PAIR_START`（或 Dongle 配对键）→
    进入 `MODE_2G4_PAIR` → 键盘每 200ms 发 `PAIR_REQ [kbd_id 4][rand 4]`；Dongle 收到后
    派生密钥 `key[i]=kbd_id^rand^dgl_id`（MAC 末 4B）→ 落盘 → 回 `PAIR_RESP`；
    键盘校验 `rand` 匹配后落盘。双方绑定完成即建链（`PAIRED` 事件 → `MODE_2G4`）。
  - 掉电记忆：绑定记录（magic+版本+CRC，18B）存 NV 槽 `BRIDGE_NV_ID_RF24_BOND`；
    `bridge_app_init()` 上电装载，重启后凭密钥直接互通。`CMD_UNPAIR`(2G4) → `forget()`。
  - 配对窗口超时（core 通用，BLE / 2.4G 同一路径）：`bridge_mode.c` 记录窗口起始时刻
    （`s_pair_started_ms`，进入 `MODE_BLE_PAIR` / `MODE_2G4_PAIR` 及重复配对分支时打点）；
    `bridge_mode_poll()` 检测超过 `BRIDGE_PAIR_TIMEOUT_S`（60s）未配对成功即停窗口并回落
    对应非配对无线模式——`port_radio_pair_stop(r)`（`bridge_radio_ch58x.c`）按射频分派：
    - 2.4G → `sdk_rf24_pair_stop()`：`tmos_stop_task` + `tmos_clear_event(s_task, RF24_EVT_PAIR)`，
      停掉每 200ms 的 `PAIR_REQ` 周期重发；
    - BLE → `sdk_ble_pair_stop()`：先 `bridge_ble_hidsvc_set_adv_intent(false)` 撤广播意图，
      再置 `GAPROLE_ADVERT_ENABLED=false` 关广播。
    同时复用 `EVT_PAIR_RESULT(1, profile)` 上报一次瞬时"配对失败/超时"，并 `note_activity()`
    重置空闲计时，避免刚回落后立即入睡。
  - 竞态修复：`sdk_rf24_deinit()` 原未停 `RF24_EVT_PAIR` 自续期任务，deinit→init 会残留旧任务
    导致重复发 `PAIR_REQ`；现已补 `tmos_stop_task` + `tmos_clear_event`。
  - 密钥既用于链路区分（非安全用途），也**派生 access address（结论4）**；CRC/信道仍固定。
    未绑定时共用默认 AA，同环境多套在此阶段仅靠协议层隔离；绑定后 AA 随绑定信息派生而互异。
- **Dongle 角色修正**（core 板级差异，`bridge_app.c` / `bridge_mode.c`）：
  - 无 SK32 上游 → 跳过 UART 心跳 / 断链判定（否则 3s 无 PING 误判断链入睡）；
    禁止空闲休眠。
  - 上电默认 `MODE_2G4`（`bridge_mode_default()`），旧固件记忆为 BLE 也强制回 2G4；
    首次上电 / NV 失效写默认值同样按板级角色。



