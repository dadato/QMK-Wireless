/* CH58x 射频适配层：实现 core/bridge_port.h 的 port_radio_* 与射频事件入口
 *
 * 本文件**不含任何 SDK 头文件**，只通过 bridge_ch58x_sdk.h 访问射频，
 * 因此可在 PC 上用 test_host/sdk_stub.c 顶替后编译运行（见 test_host/port_test.c）。
 *
 * 射频策略（D11）：2.4G 一律转调厂商用例，本层只做
 *   （1）HID 报文进出，（2）厂商回调 → EVT_* 翻译；不改射频时序与跳频算法。
 */
#include <string.h>

#include "bridge_port.h"
#include "bridge_ch58x.h"
#include "bridge_ch58x_sdk.h"
#include "bridge_ble_hidsvc.h"  /* BRIDGE_HID_RID_* */
#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_mode.h"
#include "bridge_proto.h"
#include "bridge_cfg.h"
#include "bridge_log.h"

#define BRIDGE_MOD_RADIO "radio"

/* 射频类型共 3 个取值（NONE/BLE/2G4），用长度 3 的数组按枚举下标索引 */
#define RADIO_IDX_MAX 3u

static bool    s_up[RADIO_IDX_MAX];        /* 已按该模式初始化 */
static bool    s_connected[RADIO_IDX_MAX]; /* 厂商/协议栈回调带来的链路状态 */
static uint8_t s_active_radio;             /* 最近成功初始化的射频 */

/* SRRC 定频测试时，1 字节 LED 报文的长度约定（主机 Output Report 最小长度） */
#define BRIDGE_RADIO_LED_LEN 1u

static bool radio_idx_ok(uint8_t mode)
{
    return (mode > BRIDGE_RADIO_NONE) && (mode < RADIO_IDX_MAX);
}

/* ---------------- 初始化 / 反初始化 ---------------- */

int port_radio_init(uint8_t mode)
{
    if (!radio_idx_ok(mode)) {
        return BRIDGE_ERR_PARAM;
    }

    if (s_up[mode]) {
        return BRIDGE_OK;       /* 幂等：core 的模式事务可能重复调用 */
    }

    if (mode == BRIDGE_RADIO_BLE) {
        const bridge_cfg_t *cfg = bridge_cfg_get();

        /* BLE 协议栈本身在 main 中已初始化（sdk_ble_stack_init 幂等），
         * 这里只按配置应用「广播名」与「连接间隔」，并进入可连接状态。 */
        if (sdk_ble_stack_init() != BRIDGE_OK) {
            LOG_E(BRIDGE_MOD_RADIO, "ble stack init failed");
            return BRIDGE_ERR_IO;
        }
        (void)sdk_ble_set_adv_name_with_mac(cfg->adv_name_with_mac != 0u);
        if (cfg->conn_interval != 0u) {
            (void)sdk_ble_set_conn_interval(cfg->conn_interval);
        }
    } else {
        if (sdk_rf24_init() != BRIDGE_OK) {
            LOG_E(BRIDGE_MOD_RADIO, "rf24 vendor init failed");
            return BRIDGE_ERR_IO;
        }
        /* 与 BLE 分支一致：按 cfg 下压 2.4G 上报周期（默认 1ms） */
        if (bridge_cfg_get()->rf24_report_ms != 0u) {
            (void)sdk_rf24_set_report_period_ms(bridge_cfg_get()->rf24_report_ms);
        }
    }

    s_up[mode]          = true;
    s_connected[mode]   = false;
    s_active_radio      = mode;

    LOG_I(BRIDGE_MOD_RADIO, "radio %u up", mode);
    return BRIDGE_OK;
}

int port_radio_deinit(uint8_t mode)
{
    if (!radio_idx_ok(mode)) {
        return BRIDGE_ERR_PARAM;
    }
    if (!s_up[mode]) {
        return BRIDGE_OK;
    }

    if (mode == BRIDGE_RADIO_BLE) {
        (void)sdk_ble_deinit();
    } else {
        (void)sdk_rf24_deinit();
    }

    s_up[mode]        = false;
    s_connected[mode] = false;
    LOG_I(BRIDGE_MOD_RADIO, "radio %u down", mode);
    return BRIDGE_OK;
}

/* ---------------- 数据面 ---------------- */

/* BLE 侧按报文长度选 HOGP Report ID。
 * core 的 HID 转发路径（bridge_hid.c）只把载荷与长度交给 port 层，类型信息
 * 已在此前用长度严格校验过（k_report_len[]），故此处长度即可唯一确定类型：
 *   8B 键盘 / 2B 多媒体 / 1B 系统控制；4B 鼠标与 16B NKRO 在 BLE 侧无对应报表
 *   （NKRO 已在 core 层按 D3 降级为 8B 键盘报文）。 */
static uint8_t ble_rid_for_len(uint8_t len)
{
    switch (len) {
        case BRIDGE_REPORT_KEYBOARD_LEN:
            return BRIDGE_HID_RID_KEYBOARD;
        case BRIDGE_REPORT_CONSUMER_LEN:
            return BRIDGE_HID_RID_CONSUMER;
        case BRIDGE_REPORT_SYSTEM_LEN:
            return BRIDGE_HID_RID_SYSTEM;
        default:
            return 0u;
    }
}

int port_radio_send(uint8_t mode, const uint8_t *payload, uint8_t len)
{
    if (!radio_idx_ok(mode) || payload == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (!s_up[mode] || !port_radio_is_connected(mode)) {
        return BRIDGE_ERR_STATE;
    }

    if (mode == BRIDGE_RADIO_BLE) {
        uint8_t rid = ble_rid_for_len(len);

        if (rid == 0u) {
            return BRIDGE_ERR_UNSUPPORTED;
        }
        return sdk_ble_hid_send(rid, payload, len);
    }
    return sdk_rf24_send(payload, len);
}

bool port_radio_is_connected(uint8_t mode)
{
    if (!radio_idx_ok(mode) || !s_up[mode]) {
        return false;
    }
    if (mode == BRIDGE_RADIO_BLE) {
        /* 以协议栈为准，回调只做事件通知 */
        return sdk_ble_is_connected();
    }
    return sdk_rf24_is_connected();
}

uint8_t port_radio_protocol_ver(uint8_t mode)
{
    if (mode == BRIDGE_RADIO_BLE) {
        return sdk_ble_protocol_ver();
    }
    if (mode == BRIDGE_RADIO_2G4) {
        return sdk_rf24_protocol_ver();
    }
    return 0u;
}

/* ---------------- 配对 / 绑定管理 ---------------- */

int port_radio_pair_start(uint8_t mode)
{
    if (mode == BRIDGE_RADIO_BLE) {
        if (!s_up[BRIDGE_RADIO_BLE]) {
            return BRIDGE_ERR_STATE;
        }
        return sdk_ble_pair_start((uint16_t)BRIDGE_PAIR_TIMEOUT_S);
    }
    if (mode == BRIDGE_RADIO_2G4) {
        if (!s_up[BRIDGE_RADIO_2G4]) {
            return BRIDGE_ERR_STATE;
        }
        /* 厂商用例负责配对流程，本层只触发（D11） */
        return sdk_rf24_pair_start();
    }
    return BRIDGE_ERR_PARAM;
}

int port_radio_pair_stop(uint8_t mode)
{
    if (mode == BRIDGE_RADIO_BLE) {
        if (!s_up[BRIDGE_RADIO_BLE]) {
            return BRIDGE_ERR_STATE;
        }
        return sdk_ble_pair_stop();
    }
    if (mode == BRIDGE_RADIO_2G4) {
        if (!s_up[BRIDGE_RADIO_2G4]) {
            return BRIDGE_ERR_STATE;
        }
        return sdk_rf24_pair_stop();
    }
    return BRIDGE_ERR_PARAM;
}

int port_radio_connect_profile(uint8_t mode, uint8_t profile)
{
    if (mode != BRIDGE_RADIO_BLE) {
        return BRIDGE_ERR_UNSUPPORTED;      /* 2.4G 无多主机 profile 概念 */
    }
    if (profile < 1u || profile > BRIDGE_MAX_BLE_PROFILE) {
        return BRIDGE_ERR_PARAM;
    }
    if (!s_up[BRIDGE_RADIO_BLE]) {
        return BRIDGE_ERR_STATE;
    }
    return sdk_ble_connect_profile(profile);
}

int port_radio_unpair(uint8_t mode, uint8_t profile)
{
    if (mode != BRIDGE_RADIO_BLE) {
        return sdk_rf24_unpair();
    }
    return sdk_ble_unpair(profile);         /* 0xFF = 全部 */
}

int port_radio_set_report_period_ms(uint8_t mode, uint16_t ms)
{
    if (ms == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (mode == BRIDGE_RADIO_2G4) {
        /* 目标 1ms（D5）；厂商用例能力受限时以其实际能力为准 */
        return sdk_rf24_set_report_period_ms(ms);
    }
    if (mode == BRIDGE_RADIO_BLE) {
        /* BLE 只有连接间隔（单位 1.25ms），毫秒值需换算 */
        uint32_t units = ((uint32_t)ms * 4u) / 5u;
        if (units == 0u || units > 0xFFu) {
            return BRIDGE_ERR_PARAM;
        }
        return sdk_ble_set_conn_interval((uint8_t)units);
    }
    return BRIDGE_ERR_PARAM;
}

int port_radio_test_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
    /* SRRC 定频（D9）：仅 BLE 侧可实现，2.4G 侧返回 UNSUPPORTED。
     *   mode == BRIDGE_RADIO_NONE → 停止定频并把射频归还给当前在用射频
     *   mode == BRIDGE_RADIO_BLE  → 在 channel 上开启定频发射
     *
     * 关键：定频由 BLE 链路层承载（LL_SingleChannel），而 BLE LL 与 2.4G RF_PHY
     * **共享同一片射频硬件**。LL_TestEnd 只结束 LL 测试，不会重建 RF_PHY 角色，
     * 因此停止定频后必须重建当前在用的射频；否则 2.4G 接收通路永久失效
     * （真机表现：测试模式退出后 WL 一直停在 2G4_DISCONN，rx_any 冻结，
     *  只能靠 MCU 复位恢复）。 */
    if (mode == BRIDGE_RADIO_NONE) {
        if (sdk_ble_test_mode(0u, 0u, 0u) != BRIDGE_OK) {
            return BRIDGE_ERR_IO;
        }
        if (radio_idx_ok(s_active_radio) && s_up[s_active_radio]) {
            uint8_t r = s_active_radio;

            (void)port_radio_deinit(r);
            if (port_radio_init(r) != BRIDGE_OK) {
                LOG_E(BRIDGE_MOD_RADIO, "radio %u restore after test failed", r);
                return BRIDGE_ERR_IO;
            }
            LOG_I(BRIDGE_MOD_RADIO, "radio %u restored after SRRC test", r);
        }
        return BRIDGE_OK;
    }

    if (mode != BRIDGE_RADIO_BLE) {
        return BRIDGE_ERR_UNSUPPORTED;
    }
    return sdk_ble_test_mode(1u, channel, power);
}

int port_radio_diag(uint8_t *out, uint8_t *out_len)
{
    return sdk_rf24_diag(out, out_len);
}

/* ---------------- 中断/回调 → core 事件翻译 ---------------- */

void bridge_ch58x_radio_evt(uint8_t evt)
{
    uint8_t idx = s_active_radio;

    if (evt == BRIDGE_RADIO_EVT_CONNECTED || evt == BRIDGE_RADIO_EVT_PAIRED) {
        if (radio_idx_ok(idx)) {
            s_connected[idx] = true;
        }
    } else if (evt == BRIDGE_RADIO_EVT_DISCONNECTED || evt == BRIDGE_RADIO_EVT_PAIR_FAILED) {
        if (radio_idx_ok(idx)) {
            s_connected[idx] = false;
        }
    } else {
        return;
    }

    /* core 的模式状态机负责防卡键（断链先 release_all）与 EVT_STATE 上报 */
    bridge_mode_on_radio_event(evt);
}

void bridge_ch58x_radio_hid_in(const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：键盘经 2.4G 上报的 HID 报文 → USB HID 转发主机 */
    if (sdk_usb_hid_send(payload, len) != BRIDGE_OK) {
        LOG_W(BRIDGE_MOD_RADIO, "usb hid send failed len=%u", len);
    }
#else
    /* 键盘侧：射频收到的只有主机下发的 Output Report（LED 状态，1 字节） */
    if (len == BRIDGE_RADIO_LED_LEN) {
        bridge_evt_leds(payload[0]);    /* 回传 SK32，由其驱动指示灯 */
        port_on_leds(payload[0]);
    }
#endif
}

void bridge_ch58x_radio_host_in(uint8_t state)
{
    if (state != BRIDGE_HOST_AWAKE && state != BRIDGE_HOST_SUSPEND) {
        return;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 侧主机电源状态由本地上报，键盘不会下发；忽略 */
    (void)state;
#else
    /* 键盘侧：Dongle 转发的主机挂起/恢复 → 更新主机电源状态并上报 SK32 */
    bridge_mode_set_host_awake(state == BRIDGE_HOST_AWAKE);
#endif
}

/* ---------------- 透传通道（借鉴市售模组 0x88/0x89「透传数据」） ---------------- */

int port_tunnel_send(uint8_t kind, const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 侧出口只有 USB（Raw HID → 上位机），无 BLE/2.4G 之分 */
    (void)kind;
    return sdk_usb_tunnel_send(payload, len);
#else
    if (kind == BRIDGE_TUNNEL_BLE) {
        /* VIA over BLE：出站必须走 HOGP vendor 报表（RID 5，32B Input），
         * 主机侧 usevia.app 经 WebHID 才能收到 raw HID 回包。
         * 手机 APP 的 GATT 透传 notify 出站通道不再使用（其 RX 写通道
         * 0xFFF2 仍保留，用于把主机报文上报 SK32）。 */
        return sdk_ble_hid_send(BRIDGE_HID_RID_RAW, payload, len);
    }
    return sdk_rf24_send_tunnel(payload, len);          /* 经 2.4G 送 Dongle */
#endif
}

void bridge_ch58x_radio_tunnel_in(uint8_t kind, const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：键盘/上位机方向的透传 → USB Raw HID 送主机 */
    (void)kind;
    (void)sdk_usb_tunnel_send(payload, len);
#else
    /* 键盘侧：射频收到的透传 → 上报 SK32 */
    bridge_evt_tunnel(kind, payload, len);
#endif
}

/* ---------------- 名称 / USB 标识（CMD_SET_NAME / CMD_SET_USB_ID） ---------------- */

bool port_is_dongle(void)
{
    return BRIDGE_BOARD_IS_DONGLE ? true : false;
}

int port_set_name(uint8_t target, const uint8_t *name, uint8_t len)
{
    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }

#if BRIDGE_BOARD_IS_DONGLE
    if (target == BRIDGE_NAME_TARGET_USB) {
        return sdk_usb_set_product_name(name, len);     /* 重建字符串描述符并重枚举 */
    }
    return BRIDGE_ERR_UNSUPPORTED;      /* Dongle 无 BLE 广播 */
#else
    if (target == BRIDGE_NAME_TARGET_BLE) {
        return sdk_ble_set_adv_name(name, len);         /* 更新 Scan Response 名 */
    }
    return BRIDGE_ERR_UNSUPPORTED;      /* 键盘侧 USB 名由 Dongle 负责（core 会中继） */
#endif
}

int port_set_usb_ids(uint16_t vid, uint16_t pid)
{
#if BRIDGE_BOARD_IS_DONGLE
    return sdk_usb_set_ids(vid, pid);
#else
    /* 键盘侧无 USB，但 BLE HOGP 的 VID/PID 取自 DIS 的 PnP ID 特征值：
     * 一并写入，使主机/VIA 识别到与键盘定义一致的标识。
     * 仍返回 UNSUPPORTED，让 core 把该命令中继给 Dongle 改写其 USB 描述符。 */
    (void)sdk_ble_set_ids(vid, pid);
    return BRIDGE_ERR_UNSUPPORTED;
#endif
}

int port_radio_cfg_send(const uint8_t *frame, uint8_t len)
{
    if (frame == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
#if BRIDGE_BOARD_IS_DONGLE
    (void)frame;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;      /* Dongle 是配置的接收端，无下游 */
#else
    return sdk_rf24_send_cfg(frame, len);   /* [CMD][payload...] → 2.4G 到 Dongle */
#endif
}

/* Dongle：收到键盘中继的配置帧 → 本地生效（不回调 SK32） */
void bridge_ch58x_radio_cfg_in(const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len < 1u) {
        return;
    }
    (void)bridge_proto_apply_remote(payload[0], &payload[1], (uint8_t)(len - 1u));
}
