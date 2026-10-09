/* 厂商例程回调 → 本工程入口的翻译层（纯翻译，不依赖 WCH SDK 头）
 *
 * 为什么单独成文件：
 *   这些适配器只把「厂商回调语义」翻译成 bridge_ch58x_radio_evt() /
 *   bridge_ch58x_radio_hid_in()，不需要任何 SDK 头，因此可以从
 *   bridge_ch58x_sdk.c（全工程唯一 #include SDK 头的文件）中分离出来，
 *   纳入主机端验证（见 test_host/port_test.c）；
 *   否则这段翻译逻辑永远无法在 PC 上运行验证。
 *
 * 留在 bridge_ch58x_sdk.c 的两个回调（sdk_ble_on_connect/disconnect）
 * 需要维护 SDK 侧连接句柄状态（s_ble_conn，供 HidDev_Report 使用），
 * 属于 SDK 边界职责，故不在此文件。
 *
 * 调用方（厂商例程文件，需按清单 2.2 接入）：
 *   sdk_ble_on_led_report()  ← hidkbdservice.c 的 Output Report(WriteAttrCB)
 *   sdk_ble_on_raw_report()  ← bridge_ble_hidsvc.c 的 VIA vendor Output Report(RID 5)
 *   sdk_ble_on_host_power()  ← bridge_ble_hidsvc.c 的 HID Dev evtCB（HID Control Point）
 *   sdk_rf24_on_recv()       ← 厂商 2.4G 用例收包回调（**必须在任务上下文调用**）
 *   sdk_rf24_on_link()       ← 厂商 2.4G 用例链路状态回调
 *   sdk_usb_on_led_report()  ← Dongle 侧 USB HID Output Report(LED) 回调
 *   sdk_usb_on_host_power()  ← Dongle 侧 USB 挂起/恢复边沿（sdk_usb_poll 内）
 */
#include "bridge_ch58x.h"
#include "bridge_ch58x_sdk.h"
#include "bridge_types.h"
#include "bridge_mode.h"
#include "bridge_rf24pair.h"

void sdk_ble_on_led_report(uint8_t led_state)
{
    /* 主机 Output Report（LED：Caps/Num/Scroll）→ 回传 SK32 */
    bridge_ch58x_radio_hid_in(&led_state, 1u);
}

void sdk_ble_on_raw_report(const uint8_t *d, uint8_t len)
{
    /* 主机经 BLE HID vendor 报表（RID 5，QMK Raw HID）下发的 VIA 报文
     * → 走 BLE 隧道上报 SK32，由 raw_hid_receive() 处理。
     * 与手机 APP 经 GATT 写 RX 的路径收敛到同一 SK32 入口。 */
    bridge_ch58x_radio_tunnel_in(BRIDGE_TUNNEL_BLE, d, (uint8_t)len);
}

void sdk_rf24_on_recv(const uint8_t *d, uint8_t len)
{
    /* 厂商回调若位于 RF 中断上下文，须先置标志再投递到任务上下文。
     * 2.4G 数据面在载荷前置 1 字节类型（见 BRIDGE_RADIO_PKT_*）：
     *   0x01 HID 报文 / 0x02 透传数据 / 0x03 心跳；无类型前缀时按 HID 兼容处理。 */
    if (d == 0 || len == 0u) {
        return;
    }
    if (d[0] == BRIDGE_RADIO_PKT_HEARTBEAT) {
        /* 保活帧：链路活性已在 rf24 层（收包即刷新 s_hb_miss）记录，
         * 此处只做"不派发"，避免空载荷被误当 HID 上报。 */
        return;
    }
    if (d[0] == BRIDGE_RADIO_PKT_PAIR && len > 1u) {
        /* 配对帧：[sub][kbd_id 4][rand 4][...]（link_id 已由 rf24 层校验/剥除） */
        uint8_t resp[16];
        uint8_t rlen = 0u;
        if (bridge_rf24pair_on_frame(&d[1], (uint8_t)(len - 1u), resp, &rlen) > 0) {
            if (rlen > 0u) {
                (void)sdk_rf24_send_pair(resp, rlen);   /* Dongle 应答键盘 */
            }
            bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_PAIRED);
        }
    } else if (d[0] == BRIDGE_RADIO_PKT_TUNNEL && len > 1u) {
        bridge_ch58x_radio_tunnel_in(BRIDGE_TUNNEL_USB, &d[1], (uint8_t)(len - 1u));
    } else if (d[0] == BRIDGE_RADIO_PKT_CFG && len > 1u) {
        /* 配置中继（键盘 → Dongle）：[CMD][payload...] */
        bridge_ch58x_radio_cfg_in(&d[1], (uint8_t)(len - 1u));
    } else if (d[0] == BRIDGE_RADIO_PKT_HOST && len > 1u) {
        /* 主机电源状态（Dongle → 键盘）：[state]，取 BRIDGE_HOST_* */
        bridge_ch58x_radio_host_in(d[1]);
    } else if (d[0] == BRIDGE_RADIO_PKT_HID) {
        if (len > 1u) {
            bridge_ch58x_radio_hid_in(&d[1], (uint8_t)(len - 1u));
        }
    } else {
        bridge_ch58x_radio_hid_in(d, len);
    }
}

void sdk_rf24_on_link(bool connected)
{
    bridge_ch58x_radio_evt(connected ? BRIDGE_RADIO_EVT_CONNECTED
                                     : BRIDGE_RADIO_EVT_DISCONNECTED);
}

void sdk_usb_on_led_report(uint8_t led_state)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：主机下发的 LED 状态 → 经 2.4G 回传键盘 */
    (void)sdk_rf24_send(&led_state, 1u);
#else
    /* 键盘侧主机链路是 SK32 的 USB，与本回调无关 */
    (void)led_state;
#endif
}

void sdk_ble_on_host_power(bool suspended)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 BLE HID 角色，不应到达 */
    (void)suspended;
#else
    /* 键盘侧：PC 经 BLE HID Control Point(Suspend 0x00 / Exit Suspend 0x01)
     * 控制模组休眠/唤醒 → 更新主机电源状态（挂起时许可浅睡，保持链路） */
    bridge_mode_set_host_awake(!suspended);
#endif
}

void sdk_usb_on_host_power(bool suspended)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：USB 挂起/恢复 → 经 2.4G 下发键盘（键盘据此控休眠并由 SK32 感知） */
    (void)sdk_rf24_send_host(suspended ? BRIDGE_HOST_SUSPEND : BRIDGE_HOST_AWAKE);
#else
    /* 键盘侧主机链路是 SK32 的 USB，与本回调无关 */
    (void)suspended;
#endif
}
