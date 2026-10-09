#include "bridge_uart.h"
#include "bridge_crc16.h"
#include "bridge_ringbuf.h"
#include "bridge_port.h"
#include "bridge_log.h"
#include <string.h>

#define BRIDGE_MOD_UART "uart"

/* ---------------- RX ---------------- */
typedef enum {
    ST_SOF0 = 0,
    ST_SOF1,
    ST_HDR,
    ST_PAYLOAD,
    ST_CRC
} rx_state_t;

static bridge_ringbuf_t s_rx;
static uint8_t          s_rx_storage[BRIDGE_RX_RING_SIZE];

static rx_state_t s_st;
static uint8_t    s_hdr[BRIDGE_HDR_LEN];
static uint8_t    s_hdr_idx;
static uint8_t    s_pay[BRIDGE_MAX_PAYLOAD];
static uint8_t    s_pay_idx;
static uint8_t    s_crc_bytes[BRIDGE_CRC_LEN];
static uint8_t    s_crc_idx;
static uint16_t   s_crc_run;
static uint8_t    s_frame_len;
static uint32_t   s_last_byte_ms;

static uint16_t   s_crc_err_cnt;
static uint16_t   s_drop_cnt;

/* ---------------- TX ---------------- */
static uint8_t  s_tx_storage[BRIDGE_TX_RING_SIZE];
static uint32_t s_last_tx_ms;
static bool     s_tx_sent_once;
static uint8_t  s_seq;

/* HID 去重缓存（只记录**已成功发出**的报文） */
static uint8_t  s_dup_cmd;
static uint8_t  s_dup_pay[BRIDGE_MAX_PAYLOAD];
static uint8_t  s_dup_len;
static bool     s_dup_valid;

/* ---------------- 链路活性 ---------------- */
static uint32_t s_last_rx_ms;
static uint8_t  s_missed_checks;
static bool     s_peer_seen;

/* PING 发送待重试（GAP/BUSY 时置位） */
static bool     s_ping_pending;

/* 心跳周期开关（CMD_SET_HEARTBEAT）。关闭后不发送 PING、不做失联判定、不回 PONG。 */
static bool     s_hb_enabled;

static void rx_reset(void)
{
    s_st       = ST_SOF0;
    s_hdr_idx  = 0u;
    s_pay_idx  = 0u;
    s_crc_idx  = 0u;
    s_crc_run  = 0xFFFFu;
    s_frame_len = 0u;
}

void bridge_uart_init(uint32_t baudrate)
{
    bridge_ringbuf_init(&s_rx, s_rx_storage, BRIDGE_RX_RING_SIZE);
    (void)s_tx_storage; /* TX 环由 port 层持有，core 只做时机控制 */
    rx_reset();

    s_crc_err_cnt   = 0u;
    s_drop_cnt      = 0u;
    s_last_tx_ms    = 0u;
    s_tx_sent_once  = false;
    s_seq           = 0u;
    s_dup_valid     = false;
    s_dup_len       = 0u;
    s_last_rx_ms    = port_millis();
    s_missed_checks = 0u;
    s_peer_seen     = false;
    s_ping_pending  = false;
    s_hb_enabled    = true;

    port_uart_init(baudrate);
    port_uart_irq_enable(true);

    LOG_I(BRIDGE_MOD_UART, "init baud=%lu", (unsigned long)baudrate);
}

uint16_t bridge_uart_encode(uint8_t *out, uint16_t out_size, uint8_t cmd, uint8_t seq,
                            uint8_t flags, const uint8_t *payload, uint8_t len)
{
    if (out == 0) {
        return 0u;
    }
    if (len > BRIDGE_MAX_PAYLOAD) {
        return 0u;
    }
    uint16_t total = (uint16_t)(BRIDGE_FRAME_OVERHEAD + len);
    if (total > out_size) {
        return 0u;
    }

    uint16_t i = 0u;
    out[i++] = BRIDGE_SOF0;
    out[i++] = BRIDGE_SOF1;
    out[i++] = len;
    out[i++] = cmd;
    out[i++] = seq;
    out[i++] = flags;
    for (uint8_t k = 0u; k < len; k++) {
        out[i++] = payload[k];
    }
    /* CRC 覆盖 LEN + CMD + SEQ + FLAGS + PAYLOAD */
    uint16_t crc = bridge_crc16_ccitt(&out[BRIDGE_SOF_LEN], (uint16_t)(BRIDGE_HDR_LEN + len));
    out[i++] = (uint8_t)(crc & 0xFFu);
    out[i++] = (uint8_t)((crc >> 8) & 0xFFu);
    return i;
}

bool bridge_uart_tx_ready(void)
{
    if (port_uart_tx_busy()) {
        return false;
    }
    if (s_tx_sent_once &&
        (uint32_t)(port_millis() - s_last_tx_ms) < BRIDGE_FRAME_MIN_GAP_MS) {
        return false;
    }
    return true;
}

uint32_t bridge_uart_last_tx_ms(void)
{
    return s_last_tx_ms;
}

int bridge_uart_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                     const uint8_t *payload, uint8_t len)
{
    if (port_uart_tx_busy()) {
        LOG_W(BRIDGE_MOD_UART, "tx busy, drop cmd=0x%02X", cmd);
        return BRIDGE_ERR_BUSY;
    }
    if (s_tx_sent_once &&
        (uint32_t)(port_millis() - s_last_tx_ms) < BRIDGE_FRAME_MIN_GAP_MS) {
        /* D14：帧间隔不足，交由调用方排队重试 */
        return BRIDGE_ERR_GAP;
    }

    uint8_t  raw[BRIDGE_FRAME_OVERHEAD + BRIDGE_MAX_PAYLOAD];
    uint16_t n = bridge_uart_encode(raw, (uint16_t)sizeof(raw), cmd, seq, flags, payload, len);
    if (n == 0u) {
        return BRIDGE_ERR_PARAM;
    }

    port_uart_write(raw, n);
    s_last_tx_ms   = port_millis();
    s_tx_sent_once = true;
    return BRIDGE_OK;
}

int bridge_uart_send_hid(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    if (len > BRIDGE_MAX_PAYLOAD) {
        return BRIDGE_ERR_PARAM;
    }
#if BRIDGE_HID_DEDUP
    if (s_dup_valid && s_dup_cmd == cmd && s_dup_len == len &&
        (len == 0u || memcmp(s_dup_pay, payload, len) == 0)) {
        return BRIDGE_ERR_DUP;
    }
#endif
    int rc = bridge_uart_send(cmd, s_seq++, 0u, payload, len);
    if (rc == BRIDGE_OK) {
        s_dup_cmd = cmd;
        s_dup_len = len;
        s_dup_valid = true;
        if (len > 0u) {
            memcpy(s_dup_pay, payload, len);
        }
    }
    return rc;
}

void bridge_uart_isr_feed(uint8_t byte)
{
    if (!bridge_ringbuf_push(&s_rx, byte)) {
        s_drop_cnt++;
    }
}

static void rx_on_frame_complete(bridge_frame_t *out)
{
    out->len   = s_hdr[0];
    out->cmd   = s_hdr[1];
    out->seq   = s_hdr[2];
    out->flags = s_hdr[3];
    if (out->len > 0u) {
        memcpy(out->payload, s_pay, out->len);
    }
    bridge_uart_on_rx_frame();
}

bool bridge_uart_get_frame(bridge_frame_t *out)
{
    if (out == 0) {
        return false;
    }

    /* 半帧超时：@921600 单字节 ≈10.9us，5ms 可正确切分半帧 */
    if (s_st != ST_SOF0 &&
        (uint32_t)(port_millis() - s_last_byte_ms) > BRIDGE_RX_HALF_FRAME_MS) {
        LOG_W(BRIDGE_MOD_UART, "half frame timeout, drop");
        rx_reset();
        s_drop_cnt++;
    }

    uint8_t b;
    while (bridge_ringbuf_pop(&s_rx, &b)) {
        s_last_byte_ms = port_millis();

        switch (s_st) {
            case ST_SOF0:
                if (b == BRIDGE_SOF0) {
                    s_st = ST_SOF1;
                }
                break;

            case ST_SOF1:
                if (b == BRIDGE_SOF1) {
                    s_st      = ST_HDR;
                    s_hdr_idx = 0u;
                } else if (b == BRIDGE_SOF0) {
                    /* 保持在 SOF1，兼容 AA AA 55 */
                } else {
                    rx_reset();
                }
                break;

            case ST_HDR:
                s_hdr[s_hdr_idx++] = b;
                if (s_hdr_idx == BRIDGE_HDR_LEN) {
                    s_frame_len = s_hdr[0];
                    if (s_frame_len > BRIDGE_MAX_PAYLOAD) {
                        LOG_W(BRIDGE_MOD_UART, "len %u over max, drop", s_frame_len);
                        rx_reset();
                        s_drop_cnt++;
                        break;
                    }
                    s_crc_run = bridge_crc16_ccitt(s_hdr, BRIDGE_HDR_LEN);
                    s_pay_idx = 0u;
                    s_st      = (s_frame_len == 0u) ? ST_CRC : ST_PAYLOAD;
                    if (s_st == ST_CRC) {
                        s_crc_idx = 0u;
                    }
                }
                break;

            case ST_PAYLOAD:
                s_pay[s_pay_idx++] = b;
                s_crc_run = bridge_crc16_update(s_crc_run, b);
                if (s_pay_idx == s_frame_len) {
                    s_st      = ST_CRC;
                    s_crc_idx = 0u;
                }
                break;

            case ST_CRC:
                s_crc_bytes[s_crc_idx++] = b;
                if (s_crc_idx == BRIDGE_CRC_LEN) {
                    uint16_t rx_crc = (uint16_t)((uint16_t)s_crc_bytes[0] |
                                                 ((uint16_t)s_crc_bytes[1] << 8));
                    if (rx_crc == s_crc_run) {
                        rx_on_frame_complete(out);
                        rx_reset();
                        return true;
                    }
                    s_crc_err_cnt++;
                    LOG_W(BRIDGE_MOD_UART, "crc error, cnt=%u", s_crc_err_cnt);
                    rx_reset();
                }
                break;

            default:
                rx_reset();
                break;
        }
    }
    return false;
}

void bridge_uart_note_rx_activity(void)
{
    s_last_rx_ms = port_millis();
}

void bridge_uart_on_rx_frame(void)
{
    bridge_uart_note_rx_activity();
    /* 单向活性：收到任意合法完整帧（含 PONG/ACK 应答）即视为对端在线。 */
    s_peer_seen = true;
}

void bridge_uart_ping(void)
{
    if (!s_hb_enabled) {
        return;
    }
    int rc = bridge_uart_send(CMD_PING, s_seq++, 0u, 0, 0u);
    if (rc == BRIDGE_ERR_GAP || rc == BRIDGE_ERR_BUSY) {
        s_ping_pending = true; /* 下一 tick 重试，不丢失心跳 */
        LOG_D(BRIDGE_MOD_UART, "ping deferred rc=%d", rc);
    }
}

void bridge_uart_poll(void)
{
    if (!s_hb_enabled) {
        s_ping_pending = false;   /* 心跳关闭：取消任何待发 PING */
        return;
    }
    if (!s_ping_pending) {
        return;
    }
    int rc = bridge_uart_send(CMD_PING, s_seq++, 0u, 0, 0u);
    if (rc == BRIDGE_OK) {
        s_ping_pending = false;
    }
}

void bridge_uart_heartbeat_enable(bool en)
{
    s_hb_enabled = en;
    if (!en) {
        s_ping_pending  = false;
        s_peer_seen     = false;
        s_missed_checks = 0u;
    }
}

bool bridge_uart_heartbeat_enabled(void)
{
    return s_hb_enabled;
}

bool bridge_uart_link_check(void)
{
    /* 心跳关闭：失联判定一并停止，链路恒视为正常（不触发断链入睡）。 */
    if (!s_hb_enabled) {
        s_peer_seen     = false;
        s_missed_checks = 0u;
        return true;
    }
    /* 单向判据：桥只发 PING，对端回 PONG 或任意合法完整帧即刷新 s_peer_seen。
     * 注意：不能用本端 PING 是否发出（s_ping_pending）来豁免判定，
     * 否则本端 TX 拥塞会掩盖对端真实失联。*/
    if (s_peer_seen) {
        s_peer_seen     = false;
        s_missed_checks = 0u;
        return true;
    }
    if (s_missed_checks < 0xFFu) {
        s_missed_checks++;
    }
    if (s_missed_checks >= BRIDGE_LINK_LOST_CNT) {
        LOG_W(BRIDGE_MOD_UART, "link lost, missed=%u", s_missed_checks);
        return false;
    }
    LOG_W(BRIDGE_MOD_UART, "no frame, missed=%u", s_missed_checks);
    return true;
}

bool bridge_uart_is_link_alive(void)
{
    return (s_missed_checks < BRIDGE_LINK_LOST_CNT);
}

uint32_t bridge_uart_rx_gap_ms(void)
{
    return (uint32_t)(port_millis() - s_last_rx_ms);
}

uint16_t bridge_uart_crc_error_count(void)
{
    return s_crc_err_cnt;
}

uint16_t bridge_uart_drop_count(void)
{
    return s_drop_cnt;
}
