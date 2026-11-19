/* 模式状态机（规格第 5 章）
 *
 * 优先级：VBUS 插入 > SET_MODE > 上次记忆模式
 * 切换必须走 6 步事务，且任何切换前先 HID_RELEASE_ALL（防卡键）。
 */
#ifndef BRIDGE_MODE_H
#define BRIDGE_MODE_H

#include "bridge_config.h"
#include "bridge_types.h"

typedef enum {
    MODE_USB       = 0,   /* 有线透传，射频关 */
    MODE_BLE       = 1,   /* BLE 已连接 / 待连 */
    MODE_BLE_PAIR  = 2,   /* BLE 配对窗口 */
    MODE_2G4       = 3,   /* 2.4G 已连 Dongle */
    MODE_2G4_PAIR  = 4,   /* 2.4G 配对窗口 */
    MODE_OTA       = 5,
    MODE_SLEEP     = 6
} bridge_mode_t;

/* 射频事件（由 port 层回调） */
#define BRIDGE_RADIO_EVT_CONNECTED     0u
#define BRIDGE_RADIO_EVT_DISCONNECTED  1u
#define BRIDGE_RADIO_EVT_PAIRED        2u
#define BRIDGE_RADIO_EVT_PAIR_FAILED   3u

void          bridge_mode_init(void);
void          bridge_mode_poll(void);

int           bridge_mode_request(uint8_t mode, bool persist);
bridge_mode_t bridge_mode_get(void);
uint8_t       bridge_mode_radio_id(void);     /* BRIDGE_RADIO_* */
uint8_t       bridge_mode_wireless_state(void); /* BRIDGE_WL_* 综合状态（免组合判断） */
bool          bridge_mode_is_connected(void);
bool          bridge_mode_is_wireless(void);

/* VBUS（由 SK32 经 UART 同步，或板级直采） */
void          bridge_mode_set_vbus(bool present);
bool          bridge_mode_vbus_present(void);

/* 板级上电缺省模式：Dongle=2G4（无 BLE），键盘=USB（射频关闭，等 SK32 命令）。
 * 也用于睡眠唤醒 / VBUS 拔出后的回退目标。 */
uint8_t       bridge_mode_default(void);

/* 记忆模式 → 可恢复模式映射：配对窗口（*_PAIR）是过渡态，绝不能作为唤醒 /
 * VBUS 拔出后的恢复目标（恢复到 *_PAIR 只重开广播、不走 connect_profile，
 * 表现为"深睡唤醒后连不上，需手动切模式再切回"）。其余值原样返回。 */
uint8_t       bridge_mode_restore_target(uint8_t remembered);

/* 射频回调 */
void          bridge_mode_on_radio_event(uint8_t evt);

/* 活动标记：用于空闲休眠判定 */
void          bridge_mode_note_activity(void);
uint32_t      bridge_mode_idle_ms(void);

/* 主机电源状态（PC 运行 / 挂起）：
 *   来源为 USB 挂起/恢复边沿（Dongle 侧经 2.4G 下发键盘）或 BLE HID
 *   Control Point 的 Suspend/Exit Suspend。主机挂起本身即视为「PC 对模组的
 *   休眠控制」——bridge_mode_sleep_permitted() 在该状态下返回许可，模组可浅睡
 *   以保持链路并可被 PC 唤醒。状态变化会上报 EVT_HOST_POWER。 */
void          bridge_mode_set_host_awake(bool awake);
bool          bridge_mode_host_awake(void);

/* 睡眠许可（配合 SET_CONFIG key 0x06 的睡眠策略）：
 *   策略 AUTO   → 恒许可（模块自行判闲休眠）；
 *   策略 MANUAL → 仅 CMD_SLEEP 指令置位后许可，WAKE 指令清除。
 * port 层据此决定是否进行浅睡/深睡，core 自动深睡亦以此/策略为准。 */
bool          bridge_mode_sleep_permitted(void);
void          bridge_mode_sleep_permit(bool en);

/* 浅睡（IDLE，保持链路）许可谓词：在 bridge_mode_sleep_permitted() 基础上叠加
 * 「AUTO 策略下距最近一次串口接收活动达 cfg->sleep_idle_s 秒」的时间门限
 * （idle_s=0 表示禁用自动浅睡）。port 层浅睡门控据此收口。 */
bool          bridge_mode_shallow_permitted(void);

/* 配对窗口持续时间（秒），0 = 不自动关闭。由 PAIR_START 的可选持续时间参数设置；
 * core bridge_mode_poll() 据此判定窗口超时并回落。不落盘（掉电回编译期默认）。 */
void          bridge_mode_set_pair_timeout(uint16_t s);

/* 模式名（日志用） */
const char   *bridge_mode_name(bridge_mode_t mode);

#endif /* BRIDGE_MODE_H */
