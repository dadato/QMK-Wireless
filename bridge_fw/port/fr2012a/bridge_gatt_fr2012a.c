/* FR2012A 私有桥 GATT 服务实现（服务端）
 *
 * 只做「收字节 / 发字节」，协议语义全部在 core/：
 *   0xFFF1 收到 → bridge_fr2012a_link_rx()
 *   0xFFF2 通知 ← bridge_gatt_notify()
 */
#include <string.h>

#include "gap_api.h"
#include "gatt_api.h"
#include "gatt_sig_uuid.h"

#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_log.h"
#include "bridge_gatt_fr2012a.h"
#include "bridge_fr2012a.h"

#define BRIDGE_MOD_GATT "gatt"

enum {
    BGS_IDX_SERVICE = 0,
    BGS_IDX_RX_DECL,
    BGS_IDX_RX_VAL,
    BGS_IDX_TX_DECL,
    BGS_IDX_TX_VAL,
    BGS_IDX_TX_CFG,
    BGS_IDX_NB,
};

static uint8_t s_svc_uuid[2] = { LO_UINT16(BRIDGE_GATT_SVC_UUID),
                                 HI_UINT16(BRIDGE_GATT_SVC_UUID) };
static uint8_t s_rx_value[BRIDGE_GATT_PAYLOAD_MAX];
static uint8_t s_tx_ccc[2];
static uint8_t s_svc_id;
static uint8_t s_conidx = 0xFFu;
static bool    s_ntf_enabled;
static gatt_service_t s_svc;

static const gatt_attribute_t s_att_table[BGS_IDX_NB] = {
    [BGS_IDX_SERVICE] = {
        { UUID_SIZE_2, UUID16_ARR(GATT_PRIMARY_SERVICE_UUID) },
        GATT_PROP_READ,
        UUID_SIZE_2,
        s_svc_uuid,
    },
    [BGS_IDX_RX_DECL] = {
        { UUID_SIZE_2, UUID16_ARR(GATT_CHARACTER_UUID) },
        GATT_PROP_READ,
        0,
        NULL,
    },
    [BGS_IDX_RX_VAL] = {
        { UUID_SIZE_2, UUID16_ARR(BRIDGE_GATT_RX_UUID) },
        GATT_PROP_WRITE,
        BRIDGE_GATT_PAYLOAD_MAX,
        NULL,
    },
    [BGS_IDX_TX_DECL] = {
        { UUID_SIZE_2, UUID16_ARR(GATT_CHARACTER_UUID) },
        GATT_PROP_READ,
        0,
        NULL,
    },
    [BGS_IDX_TX_VAL] = {
        { UUID_SIZE_2, UUID16_ARR(BRIDGE_GATT_TX_UUID) },
        GATT_PROP_NOTI,
        BRIDGE_GATT_PAYLOAD_MAX,
        NULL,
    },
    [BGS_IDX_TX_CFG] = {
        { UUID_SIZE_2, UUID16_ARR(GATT_CLIENT_CHAR_CFG_UUID) },
        GATT_PROP_READ | GATT_PROP_WRITE,
        2,
        NULL,
    },
};

static uint16_t bgs_msg_handler(gatt_msg_t *msg)
{
    switch (msg->msg_evt) {
    case GATTC_MSG_READ_REQ:
        if (msg->att_idx == BGS_IDX_TX_CFG) {
            memcpy(msg->param.msg.p_msg_data, s_tx_ccc, sizeof(s_tx_ccc));
            return (uint16_t)sizeof(s_tx_ccc);
        }
        if (msg->att_idx == BGS_IDX_RX_VAL) {
            return 0;
        }
        break;

    case GATTC_MSG_WRITE_REQ:
        if (msg->att_idx == BGS_IDX_RX_VAL) {
            bridge_fr2012a_link_rx(msg->param.msg.p_msg_data,
                                   (uint8_t)msg->param.msg.msg_len);
        } else if (msg->att_idx == BGS_IDX_TX_CFG) {
            memcpy(s_tx_ccc, msg->param.msg.p_msg_data, sizeof(s_tx_ccc));
            s_ntf_enabled = ((s_tx_ccc[0] & 0x01u) != 0u);
            LOG_I(BRIDGE_MOD_GATT, "bridge notif %s",
                  s_ntf_enabled ? "on" : "off");
        }
        break;

    case GATTC_MSG_LINK_CREATE:
        s_conidx      = msg->conn_idx;
        s_ntf_enabled = false;
        break;

    case GATTC_MSG_LINK_LOST:
        if (s_conidx == msg->conn_idx) {
            s_conidx      = 0xFFu;
            s_ntf_enabled = false;
        }
        break;

    default:
        break;
    }
    return 0;
}

void bridge_gatt_init(void)
{
    memset(s_rx_value, 0, sizeof(s_rx_value));
    memset(s_tx_ccc, 0, sizeof(s_tx_ccc));

    s_svc.p_att_tb       = s_att_table;
    s_svc.att_nb         = (uint8_t)BGS_IDX_NB;
    s_svc.gatt_msg_handler = bgs_msg_handler;
    s_svc_id             = gatt_add_service(&s_svc);

    LOG_I(BRIDGE_MOD_GATT, "bridge svc id=%u", (unsigned)s_svc_id);
}

void bridge_gatt_set_link(uint8_t conidx, bool up)
{
    s_conidx      = up ? conidx : 0xFFu;
    s_ntf_enabled = false;
}

int bridge_gatt_notify(const uint8_t *data, uint8_t len)
{
    gatt_ntf_t ntf;

    if (data == NULL || len == 0u || len > BRIDGE_GATT_PAYLOAD_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    if ((s_conidx == 0xFFu) || !s_ntf_enabled) {
        return BRIDGE_ERR_STATE;
    }

    ntf.conidx   = s_conidx;
    ntf.svc_id   = s_svc_id;
    ntf.att_idx  = BGS_IDX_TX_VAL;
    ntf.data_len = len;
    ntf.p_data   = (uint8_t *)data;
    gatt_notification(ntf);
    return BRIDGE_OK;
}
