#include "bridge_app.h"
#include "bridge_proto.h"
#include "bridge_uart.h"
#include "bridge_hid.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"
#include "bridge_power.h"
#include "bridge_log.h"
#include "bridge_port.h"
#include "bridge_rf24pair.h"
#include <string.h>

#define BRIDGE_MOD_APP "app"

#if !BRIDGE_BOARD_IS_DONGLE
static uint32_t s_last_ping_ms;
static uint32_t s_last_link_ms;
#endif

static bool name_differs(const char *cur, uint8_t len, const char *def)
{
    return (len != (uint8_t)strlen(def)) || (memcmp(cur, def, len) != 0);
}

/* 上电把持久化的名称 / USB 标识下发给端口层。
 *  - 键盘侧：BLE 广播名本地生效；USB 名 / VID-PID 返回 UNSUPPORTED（忽略）
 *  - Dongle ：USB 产品名 + VID/PID 本地生效；BLE 名返回 UNSUPPORTED（忽略）
 * 改 USB 标识需重新枚举，故仅在**与编译期默认值不同**时才下发，
 * 避免默认配置下每次上电都多一次 USB 重枚举。 */
static void app_apply_ident(void)
{
    const bridge_cfg_t *c = bridge_cfg_get();

    if (name_differs(bridge_cfg_ble_name(), bridge_cfg_ble_name_len(),
                     BRIDGE_BLE_NAME_DEFAULT)) {
        (void)port_set_name(BRIDGE_NAME_TARGET_BLE,
                            (const uint8_t *)bridge_cfg_ble_name(),
                            bridge_cfg_ble_name_len());
    }
    if (name_differs(bridge_cfg_usb_name(), bridge_cfg_usb_name_len(),
                     BRIDGE_USB_NAME_DEFAULT)) {
        (void)port_set_name(BRIDGE_NAME_TARGET_USB,
                            (const uint8_t *)bridge_cfg_usb_name(),
                            bridge_cfg_usb_name_len());
    }
    if ((c->usb_vid != (uint16_t)BRIDGE_USB_VID) ||
        (c->usb_pid != (uint16_t)BRIDGE_USB_PID)) {
        (void)port_set_usb_ids(c->usb_vid, c->usb_pid);
    }
}

/* 上电顺序：日志 → GPIO → 配置 → 串口 → HID → 电量 → 模式 → 协议
 *  - GPIO 必须先于 mode_init（未用引脚先归位，避免切射频期间悬空）
 *  - 串口必须先于 mode_init，因为切模式会立即发 EVT_STATE */
void bridge_app_init(void)
{
    bridge_log_init(BRIDGE_LOG_BAUD);
    port_gpio_init();

    (void)bridge_cfg_init();
    bridge_log_set_level(bridge_cfg_get()->log_level);
    bridge_rf24pair_init();     /* 2.4G 绑定记录掉电装载（mode_init 要用） */
    app_apply_ident();

    bridge_uart_init(BRIDGE_UART_BAUD);
    bridge_hid_init();
    bridge_power_init();
    bridge_proto_init();
    bridge_mode_init();

#if !BRIDGE_BOARD_IS_DONGLE
    s_last_ping_ms = port_millis();
    s_last_link_ms = port_millis();
#endif

    LOG_I(BRIDGE_MOD_APP, "bridge fw %s ready as %s (baud=%lu, gap=%ums)",
          BRIDGE_FW_VERSION, BRIDGE_BOARD_NAME,
          (unsigned long)BRIDGE_UART_BAUD, BRIDGE_FRAME_MIN_GAP_MS);
}

#if !BRIDGE_BOARD_IS_DONGLE
static void app_on_link_lost(void)
{
    LOG_E(BRIDGE_MOD_APP, "uart link lost -> release keys & sleep");
    bridge_hid_release_all();
    /* 断链安全休眠属"自动休眠"：仅在 AUTO 策略下执行。
     * MANUAL（指令控制休眠）下保持清醒，由主控经 CMD_SLEEP 决定何时进睡。 */
    if (bridge_cfg_get()->sleep_policy == BRIDGE_SLEEP_POLICY_AUTO) {
        (void)bridge_mode_request((uint8_t)MODE_SLEEP, false);
    }
}
#endif

void bridge_app_poll(void)
{
    bridge_frame_t f;
    bool woke = false;

    while (bridge_uart_get_frame(&f)) {
        bridge_proto_dispatch(&f);
        /* 令桥进入睡眠的 CMD_SLEEP 本身不算"唤醒"：否则同一条命令在
         * dispatch 里进睡、紧接着又被下面的"休眠中被唤醒"逻辑立刻拉起，
         * CMD_SLEEP(IDLE/DEEP) 将永远无法生效。 */
        bool sleep_cmd = (f.cmd == CMD_SLEEP) &&
                         ((f.len < 1u) || (f.payload[0] != BRIDGE_SLEEP_WAKE));
        if (!sleep_cmd) {
            woke = true;
        }
    }

    /* 休眠中被 SK32 唤醒 */
    if (woke && bridge_mode_get() == MODE_SLEEP) {
        bridge_mode_t m = (bridge_mode_t)bridge_mode_restore_target(
            bridge_cfg_get()->last_mode);
        if (m == MODE_SLEEP || m == MODE_OTA) {
            m = (bridge_mode_t)bridge_mode_default();
        }
        LOG_I(BRIDGE_MOD_APP, "wake from sleep -> %s", bridge_mode_name(m));
        (void)bridge_mode_request((uint8_t)m, false);
    }
    if (woke) {
        bridge_mode_note_activity();
    }

    bridge_uart_poll();

#if !BRIDGE_BOARD_IS_DONGLE
    uint32_t now = port_millis();
    /* 心跳：1s 一次（规格 6.4）。Dongle 无 SK32 上游，跳过。 */
    if ((uint32_t)(now - s_last_ping_ms) >= BRIDGE_PING_PERIOD_MS) {
        s_last_ping_ms = now;
        bridge_uart_ping();
    }

    /* 断链检测：与心跳同周期。Dongle 无上游，跳过（否则 3s 无 PING 即误判断链入睡）。 */
    if ((uint32_t)(now - s_last_link_ms) >= BRIDGE_PING_PERIOD_MS) {
        s_last_link_ms = now;
        if (!bridge_uart_link_check()) {
            app_on_link_lost();
        }
    }
#endif

    bridge_proto_poll();
    bridge_mode_poll();
}

void bridge_app_on_uart_rx(void)
{
    bridge_mode_note_activity();
}
