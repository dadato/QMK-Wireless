#include "bridge_hid.h"
#include "bridge_mode.h"
#include "bridge_port.h"
#include <string.h>

/* 本文件**不得**包含 bridge_log.h：规格 11.1 禁止高频转发路径打日志 */

#define HID_IDX(cmd)  ((uint8_t)((cmd) - CMD_HID_KEYBOARD))
#define HID_CMD_CNT   6u   /* 0x10..0x15 */

typedef struct {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[BRIDGE_MAX_PAYLOAD];
    bool     valid;
} hid_last_t;

static hid_last_t s_last[HID_CMD_CNT];
static uint8_t    s_active_mask;   /* bit i = 该类报文发过非零值 */
static uint8_t    s_led_state;
static uint16_t   s_fwd_cnt;
static uint16_t   s_dup_cnt;

static const uint8_t k_report_len[HID_CMD_CNT] = {
    BRIDGE_REPORT_KEYBOARD_LEN,  /* 0x10 */
    BRIDGE_REPORT_NKRO_LEN,      /* 0x11 */
    BRIDGE_REPORT_CONSUMER_LEN,  /* 0x12 */
    BRIDGE_REPORT_MOUSE_LEN,     /* 0x13 */
    BRIDGE_REPORT_SYSTEM_LEN,    /* 0x14 */
    0u                           /* 0x15 RELEASE_ALL，无载荷 */
};

static bool payload_is_zero(const uint8_t *p, uint8_t len)
{
    for (uint8_t i = 0u; i < len; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

static void cache_clear(void)
{
    for (uint8_t i = 0u; i < HID_CMD_CNT; i++) {
        s_last[i].valid = false;
        s_last[i].len   = 0u;
    }
}

void bridge_hid_init(void)
{
    memset(s_last, 0, sizeof(s_last));
    s_active_mask = 0u;
    s_led_state   = 0u;
    s_fwd_cnt     = 0u;
    s_dup_cnt     = 0u;
    cache_clear();
}

void bridge_hid_set_leds(uint8_t led_state)
{
    s_led_state = led_state;
}

uint8_t bridge_hid_get_leds(void)
{
    return s_led_state;
}

void bridge_hid_note_link(bool connected)
{
    /* 链路变化后上一帧状态不再可信，必须清缓存，
     * 否则重连后"与断链前相同"的首帧会被误抑制。 */
    cache_clear();
    if (!connected) {
        s_active_mask = 0u;
    }
}

uint8_t bridge_hid_nkro_to_boot(const uint8_t *nkro, uint8_t *boot8)
{
    if (nkro == 0 || boot8 == 0) {
        return 0u;
    }

    memset(boot8, 0, BRIDGE_REPORT_KEYBOARD_LEN);
    boot8[0] = nkro[0];   /* 修饰键直接沿用 */

    uint8_t n = 0u;
    /* NKRO 位图从 byte1 bit0 起，bit k 对应 usage = BRIDGE_NKRO_USAGE_BASE + k */
    for (uint8_t byte = 1u; byte < BRIDGE_REPORT_NKRO_LEN; byte++) {
        uint8_t bits = nkro[byte];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            if ((bits & (uint8_t)(1u << bit)) == 0u) {
                continue;
            }
            uint16_t usage = (uint16_t)BRIDGE_NKRO_USAGE_BASE +
                             (uint16_t)((byte - 1u) * 8u) + bit;
            if (usage > BRIDGE_BOOT_USAGE_MAX) {
                continue;   /* Boot 报文表达不了，丢弃 */
            }
            if (n < BRIDGE_NKRO_MAX_KEYS) {
                boot8[2u + n] = (uint8_t)usage;
                n++;
            } else {
                boot8[1] = BRIDGE_HID_ROLLOVER_ERROR;   /* 超过 6 键 → Rollover Error */
                return n;
            }
        }
    }
    return n;
}

/* 返回实际用于转发的命令码（可能因降级而改变） */
static uint8_t resolve_cmd(uint8_t cmd, const uint8_t *payload, uint8_t len,
                           uint8_t *out_payload, uint8_t *out_len)
{
    memcpy(out_payload, payload, len);
    *out_len = len;

#if !BRIDGE_FEATURE_NKRO_BLE
    if (cmd == CMD_HID_NKRO && bridge_mode_get() != MODE_2G4) {
        /* D3：BLE（及非 2.4G）模式下降级为 6KRO */
        bridge_hid_nkro_to_boot(payload, out_payload);
        *out_len = BRIDGE_REPORT_KEYBOARD_LEN;
        return CMD_HID_KEYBOARD;
    }
#endif
    return cmd;
}

int bridge_hid_send(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    if (cmd < CMD_HID_KEYBOARD || cmd > CMD_HID_RELEASE) {
        return BRIDGE_ERR_PARAM;
    }
    if (cmd == CMD_HID_RELEASE) {
        return bridge_hid_release_all();
    }
    if (len > BRIDGE_MAX_PAYLOAD || payload == 0) {
        return BRIDGE_ERR_PARAM;
    }
    /* 长度必须与命令严格匹配，否则射频侧上报长度错乱 */
    if (len != k_report_len[HID_IDX(cmd)]) {
        return BRIDGE_ERR_PARAM;
    }

    uint8_t radio = bridge_mode_radio_id();
    if (radio == BRIDGE_RADIO_NONE || !port_radio_is_connected(radio)) {
        return BRIDGE_ERR_STATE;
    }

    uint8_t fwd_payload[BRIDGE_MAX_PAYLOAD];
    uint8_t fwd_len = len;
    uint8_t fwd_cmd = resolve_cmd(cmd, payload, len, fwd_payload, &fwd_len);
    uint8_t idx     = HID_IDX(fwd_cmd);

    /* 去重：与上一帧完全相同则抑制（D14） */
    if (s_last[idx].valid && s_last[idx].len == fwd_len &&
        memcmp(s_last[idx].data, fwd_payload, fwd_len) == 0) {
        s_dup_cnt++;
        return BRIDGE_ERR_DUP;
    }

    int rc = port_radio_send(radio, fwd_payload, fwd_len);
    if (rc != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }

    s_last[idx].valid = true;
    s_last[idx].cmd   = fwd_cmd;
    s_last[idx].len   = fwd_len;
    memcpy(s_last[idx].data, fwd_payload, fwd_len);

    if (!payload_is_zero(fwd_payload, fwd_len)) {
        s_active_mask |= (uint8_t)(1u << idx);
    } else {
        s_active_mask &= (uint8_t)~(1u << idx);
    }

    s_fwd_cnt++;
    return BRIDGE_OK;
}

int bridge_hid_release_all(void)
{
    uint8_t radio = bridge_mode_radio_id();
    uint8_t zeros[BRIDGE_MAX_PAYLOAD];
    memset(zeros, 0, sizeof(zeros));

    /* 先清缓存：即使射频未连接，也必须让下一次"相同按键"能重新上报 */
    cache_clear();

    if (radio == BRIDGE_RADIO_NONE || !port_radio_is_connected(radio)) {
        s_active_mask = 0u;
        return BRIDGE_OK;
    }

    for (uint8_t i = 0u; i < HID_CMD_CNT; i++) {
        if ((s_active_mask & (uint8_t)(1u << i)) == 0u) {
            continue;
        }
        uint8_t len = k_report_len[i];
        if (len == 0u) {
            continue;
        }
        if (port_radio_send(radio, zeros, len) != BRIDGE_OK) {
            /* 继续尝试其余类型，最后统一返回错误 */
            s_active_mask &= (uint8_t)~(1u << i);
            continue;
        }
        s_active_mask &= (uint8_t)~(1u << i);
    }
    s_active_mask = 0u;
    return BRIDGE_OK;
}

uint16_t bridge_hid_forward_count(void)
{
    return s_fwd_cnt;
}

uint16_t bridge_hid_dup_count(void)
{
    return s_dup_cnt;
}
