/* FR2012A BLE 射频适配层实现
 *
 * 说明（与 CH58x 的差异）：
 *   1. 本芯片只有 BLE 射频；CH58x 的 2.4G 私有用例在这里用 BLE 私有 GATT
 *      链路（0xFFF0/0xFFF1/0xFFF2，见 bridge_gatt_fr2012a.c）替代。
 *   2. 键盘侧 BLE 模式 = HOGP 直连主机（hid_service，与 CH58x 相同）。
 *   3. 接收器为 BLE 主机：扫描带 0xFFF0 服务的广播 → 连接 → 订阅 0xFFF2 通知。
 *
 * 本文件是全工程除 SDK 原语外唯一直接使用 FreqChip BLE API 的地方。
 */
#include <string.h>

#include "gap_api.h"
#include "gatt_api.h"
#include "gatt_sig_uuid.h"
#include "ble_stack.h"
#include "driver_gpio.h"

#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"
#include "bridge_port.h"
#include "bridge_log.h"

#include "bridge_ble_fr2012a.h"
#include "bridge_gatt_fr2012a.h"
#include "bridge_fr2012a.h"
#include "bridge_fr2012a_sdk.h"

#if !BRIDGE_BOARD_IS_DONGLE
#include "hid_service.h"
#endif

#define BRIDGE_MOD_BLE   "ble"
#define FR_CONN_NONE     0xFFu

/* 私有桥下行帧类型（接收器 → 模组） */
#define FR_LINK_DOWN_LED        0x01u
#define FR_LINK_DOWN_HOST_PWR   0x02u

/* ---------------- 2.4G 低延迟链路参数 ----------------
 * 本芯片的厂商包**不含私有 2.4G 物理层**（闭源库只提供 BLE 与 DTM 测试函数），
 * 因此 `BRIDGE_RADIO_2G4` 用一条为键盘调优的 BLE 私有链路承载：
 *   - LE 2M PHY（空中速率翻倍，缩短单包占用）
 *   - 连接间隔 7.5ms（BLE 规范下限 6×1.25ms）+ slave latency 0
 *   - 两个方向都走 Write Without Response / Notification（无 ATT 应答往返）
 *   - MTU 128（ATT 载荷 124B），足够容纳 64B 桥载荷 + 32B Raw HID
 * 若后续拿到厂商 RF_PHY 私有 2.4G 库，只需替换本文件与 bridge_gatt_fr2012a
 * 的承载层，core/ 与 port_radio_* 接口不变。
 */
#define FR_LINK_ITVL_UNITS      6u      /* 7.5ms（BLE 规范下限） */
#define FR_LINK_ITVL_FAST       1u      /* 1.25ms：仅当厂商 double rate 可用 */
#define FR_LINK_ITVL_MAX_UNITS  3200u   /* 4s */
#define FR_LINK_TIMEOUT_UNITS   400u    /* 4s */
#define FR_LINK_ADV_ITVL        0x20u   /* 20ms */
#define FR_LINK_SCAN_ITVL       0x30u   /* 30ms */

/* 实际可用的最小连接间隔：厂商完整 SDK 启用 double rate 后为 1.25ms */
static uint16_t fr_itvl_min(void)
{
    return sdk_rf_fast_response_supported() ? FR_LINK_ITVL_FAST
                                            : FR_LINK_ITVL_UNITS;
}

static uint8_t  s_radio;
static uint8_t  s_conidx = FR_CONN_NONE;
static bool     s_started;

#if BRIDGE_BOARD_IS_DONGLE
static uint32_t s_last_scan_ms;
static uint8_t  s_client_id;
static const uint8_t s_bridge_svc_uuid[2] = {
    LO_UINT16(BRIDGE_GATT_SVC_UUID), HI_UINT16(BRIDGE_GATT_SVC_UUID)
};
static const gatt_uuid_t s_client_att_tb[2] = {
    { UUID_SIZE_2, UUID16_ARR(BRIDGE_GATT_RX_UUID) },
    { UUID_SIZE_2, UUID16_ARR(BRIDGE_GATT_TX_UUID) },
};
#else
static uint8_t  s_adv_name[BRIDGE_NAME_MAX + 1u];
#endif

/* ============================ 公共工具 ============================ */

bool bridge_fr_ble_is_connected(void)
{
    return (s_conidx != FR_CONN_NONE);
}

/* 连上后立即把链路参数压到键盘档：7.5ms 间隔、latency 0、2M PHY。
 * 主/从两侧都主动请求，避免对端默认参数（30~50ms）拖慢按键延迟。 */
static void fr_link_tune(uint8_t conidx)
{
    uint16_t itvl = fr_itvl_min();

    gap_conn_param_update(conidx, itvl, itvl, 0u, FR_LINK_TIMEOUT_UNITS);
    /* SDK v2.2 追加 coded_rate 参数（非 Coded PHY 传 0） */
    gap_conn_phy_update(conidx, GAP_PHY_2MBPS, GAP_PHY_2MBPS, 0u);
}

#if !BRIDGE_BOARD_IS_DONGLE

/* HOGP 报表索引（与 hid_service.c 的 hid_rpt_info[] 对应） */
#define FR_RPT_MOUSE      0u
#define FR_RPT_CONSUMER   1u
#define FR_RPT_KEYBOARD   2u

static void fr_adv_update(void)
{
    uint8_t  adv[31];
    uint8_t  i = 0u;
    const char *name = bridge_cfg_ble_name();
    uint8_t  nlen = bridge_cfg_ble_name_len();

    if (nlen > 20u) {
        nlen = 20u;
    }

    adv[i++] = 2u;                  /* Flags */
    adv[i++] = 0x01;
    adv[i++] = 0x06;                /* LE General Discoverable + BR/EDR Not Supported */

    if (s_radio == BRIDGE_RADIO_2G4) {
        /* 2.4G 私有链路：只广播桥服务，避免手机/PC 把它当 HOGP 键盘抢连 */
        adv[i++] = 3u;
        adv[i++] = GAP_ADVTYPE_16BIT_COMPLETE;
        adv[i++] = LO_UINT16(BRIDGE_GATT_SVC_UUID);
        adv[i++] = HI_UINT16(BRIDGE_GATT_SVC_UUID);
    } else {
        /* BLE 模式：HOGP(0x1812) + 私有桥服务(0xFFF0) */
        adv[i++] = 5u;
        adv[i++] = GAP_ADVTYPE_16BIT_COMPLETE;
        adv[i++] = LO_UINT16(0x1812u);
        adv[i++] = HI_UINT16(0x1812u);
        adv[i++] = LO_UINT16(BRIDGE_GATT_SVC_UUID);
        adv[i++] = HI_UINT16(BRIDGE_GATT_SVC_UUID);
    }

    adv[i++] = (uint8_t)(nlen + 1u);
    adv[i++] = GAP_ADVTYPE_LOCAL_NAME_COMPLETE;
    memcpy(&adv[i], name, nlen);
    i = (uint8_t)(i + nlen);

    gap_set_advertising_data(adv, i);

    memcpy(s_adv_name, name, nlen);
    s_adv_name[nlen] = 0u;
}

static void fr_adv_start(void)
{
    gap_adv_param_t param;

    memset(&param, 0, sizeof(param));
    param.adv_mode       = GAP_ADV_MODE_UNDIRECT;
    param.adv_addr_type  = GAP_ADDR_TYPE_PUBLIC;
    param.phy_mode       = GAP_PHY_1MBPS;
    param.adv_intv_min   = FR_LINK_ADV_ITVL;
    param.adv_intv_max   = FR_LINK_ADV_ITVL;
    param.adv_chnl_map   = GAP_ADV_CHAN_ALL;
    param.adv_filt_policy = GAP_ADV_ALLOW_SCAN_ANY_CON_ANY;
    param.disc_mode      = GAP_ADV_DISC_MODE_GEN_DISC;

    gap_set_advertising_param(&param);
    gap_start_advertising(0);
    LOG_I(BRIDGE_MOD_BLE, "adv start name=%s", s_adv_name);
}

#endif /* !BRIDGE_BOARD_IS_DONGLE */

#if BRIDGE_BOARD_IS_DONGLE

static void fr_scan_start(void)
{
    gap_scan_param_t param;

    memset(&param, 0, sizeof(param));
    param.scan_mode   = GAP_SCAN_MODE_GEN_DISC;
    param.dup_filt_pol = 0;
    param.phy_mode    = GAP_PHY_1MBPS;
    param.scan_intv   = FR_LINK_SCAN_ITVL;
    param.scan_window = FR_LINK_SCAN_ITVL;
    param.duration    = 0;          /* 持续扫描 */

    gap_start_scan(&param);
    s_last_scan_ms = port_millis();
    LOG_I(BRIDGE_MOD_BLE, "scan start");
}

/* 广播里是否带私有桥服务 0xFFF0 */
static bool fr_adv_is_bridge(const gap_evt_adv_report_t *rpt)
{
    uint16_t i = 0u;

    while ((uint16_t)(i + 1u) < rpt->length) {
        uint8_t len = rpt->data[i];

        if (len == 0u) {
            break;
        }
        if ((rpt->data[i + 1u] == GAP_ADVTYPE_16BIT_COMPLETE) ||
            (rpt->data[i + 1u] == GAP_ADVTYPE_16BIT_MORE)) {
            for (uint8_t k = (uint8_t)(i + 2u); (uint16_t)(k + 1u) < (uint16_t)(i + 1u + len); k = (uint8_t)(k + 2u)) {
                if ((rpt->data[k] == LO_UINT16(BRIDGE_GATT_SVC_UUID)) &&
                    (rpt->data[k + 1u] == HI_UINT16(BRIDGE_GATT_SVC_UUID))) {
                    return true;
                }
            }
        }
        i = (uint16_t)(i + 1u + len);
    }
    return false;
}

static int fr_link_write(const uint8_t *data, uint8_t len)
{
    gatt_client_write_t wr;

    if ((s_conidx == FR_CONN_NONE) || data == NULL || len == 0u) {
        return BRIDGE_ERR_STATE;
    }
    wr.conidx    = s_conidx;
    wr.client_id = s_client_id;
    wr.att_idx   = 0u;              /* 0xFFF1（RX，写） */
    wr.p_data    = (uint8_t *)data;
    wr.data_len  = len;
    gatt_client_write_cmd(wr);
    return BRIDGE_OK;
}

static uint16_t fr_central_gatt_msg(gatt_msg_t *msg)
{
    switch (msg->msg_evt) {
    case GATTC_MSG_NTF_REQ:
        bridge_fr2012a_hid_in(msg->param.msg.p_msg_data,
                              (uint8_t)msg->param.msg.msg_len);
        break;

    case GATTC_MSG_CMP_EVT:
        if (msg->param.op.operation == GATT_OP_PEER_SVC_REGISTERED) {
            gatt_client_enable_ntf_t en;

            en.conidx    = msg->conn_idx;
            en.client_id = s_client_id;
            en.att_idx   = 1u;      /* 0xFFF2（TX，通知） */
            gatt_client_enable_ntf(en);
            LOG_I(BRIDGE_MOD_BLE, "bridge svc discovered, ntf enabled");
        }
        break;

    case GATTC_MSG_LINK_LOST:
        bridge_gatt_set_link(msg->conn_idx, false);
        break;

    default:
        break;
    }
    return 0;
}

#endif /* BRIDGE_BOARD_IS_DONGLE */

/* ============================ GAP 事件 ============================ */

static void fr_gap_evt_cb(gap_event_t *evt)
{
    switch (evt->type) {
    case GAP_EVT_ALL_SVC_ADDED:
        LOG_I(BRIDGE_MOD_BLE, "all svc added");
        break;

#if !BRIDGE_BOARD_IS_DONGLE
    case GAP_EVT_SLAVE_CONNECT:
        s_conidx = evt->param.slave_connect.conidx;
        bridge_gatt_set_link(s_conidx, true);
        fr_link_tune(s_conidx);
        LOG_I(BRIDGE_MOD_BLE, "peer connected conidx=%u", (unsigned)s_conidx);
        bridge_fr2012a_set_connected(true);
        bridge_fr2012a_radio_evt(BRIDGE_RADIO_EVT_CONNECTED);
        break;
#else
    case GAP_EVT_ADV_REPORT:
        if (!s_started || (s_conidx != FR_CONN_NONE)) {
            break;
        }
        if (fr_adv_is_bridge(evt->param.adv_rpt)) {
            uint16_t itvl = fr_itvl_min();

            gap_stop_scan();
            gap_start_conn(&evt->param.adv_rpt->src_addr.addr,
                           evt->param.adv_rpt->src_addr.addr_type,
                           itvl, itvl, 0u, FR_LINK_TIMEOUT_UNITS);
            LOG_I(BRIDGE_MOD_BLE, "bridge peer found, connecting");
        }
        break;

    case GAP_EVT_MASTER_CONNECT:
        s_conidx = evt->param.master_connect.conidx;
        bridge_gatt_set_link(s_conidx, true);
        bridge_fr2012a_set_connected(true);
        bridge_fr2012a_radio_evt(BRIDGE_RADIO_EVT_CONNECTED);
        fr_link_tune(s_conidx);
        gatt_mtu_exchange_req(s_conidx);
        gatt_discovery_peer_svc(s_client_id, s_conidx, 2u,
                                (uint8_t *)s_bridge_svc_uuid);
        LOG_I(BRIDGE_MOD_BLE, "connected conidx=%u", (unsigned)s_conidx);
        break;
#endif

    case GAP_EVT_DISCONNECT:
        if (evt->param.disconnect.conidx == s_conidx) {
            s_conidx = FR_CONN_NONE;
            bridge_gatt_set_link(evt->param.disconnect.conidx, false);
            bridge_fr2012a_set_connected(false);
            bridge_fr2012a_radio_evt(BRIDGE_RADIO_EVT_DISCONNECTED);
            LOG_I(BRIDGE_MOD_BLE, "disconnect reason=0x%02x",
                  (unsigned)evt->param.disconnect.reason);
        }
#if BRIDGE_BOARD_IS_DONGLE
        if (s_started) {
            s_last_scan_ms = port_millis() - 1000u;  /* 下一轮 poll 立即重扫 */
        }
#else
        /* 模组侧：断链后立刻恢复可发现/可连接（主机掉线后自动回连） */
        if (s_started) {
            fr_adv_start();
        }
#endif
        break;

    case GAP_EVT_MTU:
        LOG_I(BRIDGE_MOD_BLE, "mtu=%u", (unsigned)evt->param.mtu.value);
        break;

    default:
        break;
    }
}

/* ============================ 对外接口 ============================ */

void bridge_fr_ble_init(void)
{
    gap_security_param_t sec;
    const char *name = bridge_cfg_ble_name();
    uint8_t     nlen = bridge_cfg_ble_name_len();

    gap_set_cb_func(fr_gap_evt_cb);

    memset(&sec, 0, sizeof(sec));
    sec.mitm           = false;
    sec.ble_secure_conn = true;
    sec.io_cap         = GAP_IO_CAP_NO_INPUT_NO_OUTPUT;
    sec.pair_init_mode = GAP_PAIRING_MODE_WAIT_FOR_REQ;
    sec.bond_auth      = true;
    sec.password       = 0u;
    gap_security_param_init(&sec);

    gap_dev_name_set((uint8_t *)name, nlen);
    gap_set_mtu(128u);

#if !BRIDGE_BOARD_IS_DONGLE
    hid_gatt_add_service();
#else
    {
        gatt_client_t client;

        client.p_att_tb           = s_client_att_tb;
        client.att_nb             = 2u;
        client.att_tb_initialized = 0u;
        client.gatt_msg_handler   = fr_central_gatt_msg;
        s_client_id               = gatt_add_client(&client);
    }
#endif
    LOG_I(BRIDGE_MOD_BLE, "init name=%s", name);
}

int bridge_fr_ble_start(uint8_t radio)
{
    if ((radio != BRIDGE_RADIO_BLE) && (radio != BRIDGE_RADIO_2G4)) {
        return BRIDGE_ERR_PARAM;
    }

    s_radio   = radio;
    s_started = true;

#if BRIDGE_BOARD_IS_DONGLE
    fr_scan_start();
#else
    fr_adv_update();
    fr_adv_start();
#endif
    return BRIDGE_OK;
}

void bridge_fr_ble_stop(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    gap_stop_scan();
    if (s_conidx != FR_CONN_NONE) {
        gap_disconnect_req(s_conidx);
    }
#else
    gap_stop_advertising();
    if (s_conidx != FR_CONN_NONE) {
        gap_disconnect_req(s_conidx);
    }
#endif
    s_started = false;
    s_conidx  = FR_CONN_NONE;
    bridge_gatt_set_link(0u, false);
    bridge_fr2012a_set_connected(false);
    LOG_I(BRIDGE_MOD_BLE, "radio stopped");
}

int bridge_fr_ble_send(const uint8_t *payload, uint8_t len)
{
    if (payload == NULL || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_conidx == FR_CONN_NONE) {
        return BRIDGE_ERR_STATE;
    }

#if BRIDGE_BOARD_IS_DONGLE
    return fr_link_write(payload, len);
#else
    /* 私有桥链路（接收器）与 HOGP 直连主机的报表分派 */
    if (s_radio == BRIDGE_RADIO_2G4) {
        return bridge_gatt_notify(payload, len);
    }

    switch (len) {
    case BRIDGE_REPORT_KEYBOARD_LEN:
        hid_gatt_report_notify(s_conidx, FR_RPT_KEYBOARD, (uint8_t *)payload, len);
        return BRIDGE_OK;
    case BRIDGE_REPORT_CONSUMER_LEN:
        hid_gatt_report_notify(s_conidx, FR_RPT_CONSUMER, (uint8_t *)payload, len);
        return BRIDGE_OK;
    case BRIDGE_REPORT_MOUSE_LEN:
        hid_gatt_report_notify(s_conidx, FR_RPT_MOUSE, (uint8_t *)payload, len);
        return BRIDGE_OK;
    case BRIDGE_REPORT_SYSTEM_LEN:
        /* HOGP 报表集里无 System Control，退化为不支持（与 CH58x 一致） */
        return BRIDGE_ERR_UNSUPPORTED;
    default:
        return BRIDGE_ERR_UNSUPPORTED;
    }
#endif
}

int bridge_fr_ble_send_led(uint8_t leds)
{
    uint8_t frame[2];

    frame[0] = FR_LINK_DOWN_LED;
    frame[1] = leds;
    return bridge_fr_ble_send(frame, sizeof(frame));
}

int bridge_fr_ble_send_host_power(uint8_t state)
{
    uint8_t frame[2];

    frame[0] = FR_LINK_DOWN_HOST_PWR;
    frame[1] = state;
    return bridge_fr_ble_send(frame, sizeof(frame));
}

/* 把上报周期（ms）换算成 BLE 连接间隔；仅在链路已建立时下发参数更新 */
int bridge_fr_ble_set_interval_ms(uint16_t ms)
{
    uint32_t units = ((uint32_t)ms * 4u) / 5u;

    if (units < fr_itvl_min()) {
        units = fr_itvl_min();
    }
    if (units > FR_LINK_ITVL_MAX_UNITS) {
        units = FR_LINK_ITVL_MAX_UNITS;
    }
    if (s_conidx == FR_CONN_NONE) {
        return BRIDGE_OK;       /* 未连接：下次连接统一由 fr_link_tune() 压到下限 */
    }
    gap_conn_param_update(s_conidx, (uint16_t)units, (uint16_t)units,
                          0u, FR_LINK_TIMEOUT_UNITS);
    return BRIDGE_OK;
}

void bridge_fr_ble_pair_start(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    gap_bond_manager_delete_all();
    s_started = true;
    fr_scan_start();
#else
    if (s_conidx != FR_CONN_NONE) {
        gap_disconnect_req(s_conidx);
    }
    s_started = true;
    fr_adv_start();
#endif
    LOG_I(BRIDGE_MOD_BLE, "pair start");
}

void bridge_fr_ble_unpair(void)
{
    gap_bond_manager_delete_all();
    LOG_I(BRIDGE_MOD_BLE, "bond cleared");
}

void bridge_fr_ble_set_name(const uint8_t *name, uint8_t len)
{
    uint8_t n = (len > BRIDGE_NAME_MAX) ? (uint8_t)BRIDGE_NAME_MAX : len;

    if (name == NULL || n == 0u) {
        return;
    }
    gap_dev_name_set((uint8_t *)name, n);

#if !BRIDGE_BOARD_IS_DONGLE
    memcpy(s_adv_name, name, n);
    s_adv_name[n] = 0u;
    if (s_started) {
        gap_stop_advertising();
        fr_adv_update();
        fr_adv_start();
    }
#endif
}

void bridge_fr_ble_poll(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    if (s_started && (s_conidx == FR_CONN_NONE) &&
        ((port_millis() - s_last_scan_ms) >= 1000u)) {
        fr_scan_start();
    }
#endif
}

/* ============================ HID 报表 → core ============================ */

#if !BRIDGE_BOARD_IS_DONGLE
/* hid_service.c 的弱回调：主机写 Output Report（LED）时上抛 */
void bridge_fr_hid_out_report(uint8_t leds)
{
    bridge_fr2012a_hid_in(&leds, 1u);
}
#endif
