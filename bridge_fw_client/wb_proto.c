/* Wireless bridge client frame layer (pure logic, no platform dependency). */
#include "wb_proto.h"

/* ---------------- CRC-16/CCITT-FALSE ---------------- */
uint16_t wb_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0u; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0u; j < 8u; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* Compile-time sanity on the wire layout. */
_Static_assert(WB_FRAME_OVERHEAD == 8u, "bridge frame overhead must be 8");
_Static_assert(WB_SOF0 == 0xAAu && WB_SOF1 == 0x55u, "bridge SOF marker");

uint16_t wb_encode(uint8_t *out, uint16_t cap, uint8_t cmd, uint8_t seq,
                   uint8_t flags, const uint8_t payload[], uint8_t len) {
    /* LEN is 8 bits; the CRC is computed over LEN+CMD+SEQ+FLAGS+PAYLOAD. */
    uint16_t total = WB_FRAME_OVERHEAD + (uint16_t)len;
    if (cap < total) {
        return 0u;
    }
    out[0] = WB_SOF0;
    out[1] = WB_SOF1;
    out[2] = len;
    out[3] = cmd;
    out[4] = seq;
    out[5] = flags;
    if (len > 0u && payload != NULL) {
        for (uint8_t i = 0u; i < len; i++) {
            out[WB_SOF_LEN + WB_HDR_LEN + i] = payload[i];
        }
    }
    /* CRC over LEN..PAYLOAD (out[2] .. out[2+HDR_LEN-1+len]) */
    uint16_t crc = wb_crc16(&out[WB_SOF_LEN], WB_HDR_LEN + len);
    out[total - 2] = (uint8_t)(crc & 0xFFu);
    out[total - 1] = (uint8_t)(crc >> 8);
    return total;
}

/* ---------------- incremental RX state machine ---------------- */
typedef enum {
    RX_SOF0 = 0,
    RX_SOF1,
    RX_HDR,
    RX_PAYLOAD,
    RX_CRC,
    RX_DONE_FRAME
} rx_state_t;

static rx_state_t s_rx_state;
static uint8_t    s_rx_buf[WB_FRAME_MAX];
static uint8_t    s_rx_idx;   /* next write index into s_rx_buf */
static uint8_t    s_rx_len;   /* expected payload length */
static uint8_t    s_rx_hdr;   /* bytes of LEN+CMD+SEQ+FLAGS collected */

static uint8_t s_seq = 0u;

void wb_rx_reset(void) {
    s_rx_state = RX_SOF0;
    s_rx_idx   = 0u;
    s_rx_len   = 0u;
    s_rx_hdr   = 0u;
}

uint8_t wb_seq_peek(void) {
    return s_seq;
}

uint8_t wb_seq_next(void) {
    s_seq++;
    if (s_seq == 0u) {
        s_seq = 1u;   /* keep 0 as "no seq yet" sentinel out of circulation */
    }
    return s_seq;
}

bool wb_rx_byte(uint8_t b, wb_frame_t *out) {
    switch (s_rx_state) {
        case RX_SOF0:
            if (b == WB_SOF0) {
                s_rx_buf[0] = b;
                s_rx_idx   = 1u;
                s_rx_state = RX_SOF1;
            } else {
                s_rx_state = RX_SOF0;   /* stay looking for SOF0 */
            }
            return false;

        case RX_SOF1:
            if (b == WB_SOF1) {
                s_rx_buf[1] = b;
                s_rx_idx   = 2u;
                s_rx_hdr   = 0u;
                s_rx_len   = 0u;
                s_rx_state = RX_HDR;
            } else {
                /* not a valid SOF pair; resync on the current byte */
                s_rx_state = (b == WB_SOF0) ? RX_SOF1 : RX_SOF0;
                s_rx_buf[0] = b;
                s_rx_idx   = 1u;
            }
            return false;

        case RX_HDR: {
            s_rx_buf[s_rx_idx] = b;
            s_rx_idx++;
            s_rx_hdr++;
            if (s_rx_hdr == 1u) {
                /* first hdr byte is LEN (payload length) */
                s_rx_len = b;
            }
            if (s_rx_hdr >= WB_HDR_LEN) {
                if (s_rx_len > WB_MAX_PAYLOAD) {
                    wb_rx_reset();
                } else if (s_rx_len == 0u) {
                    s_rx_state = RX_CRC;   /* no payload; jump straight to CRC */
                } else {
                    s_rx_state = RX_PAYLOAD;
                }
            }
            return false;
        }

        case RX_PAYLOAD:
            s_rx_buf[s_rx_idx] = b;
            s_rx_idx++;
            if (s_rx_idx >= WB_SOF_LEN + WB_HDR_LEN + s_rx_len) {
                s_rx_state = RX_CRC;
            }
            return false;

        case RX_CRC: {
            s_rx_buf[s_rx_idx] = b;
            s_rx_idx++;
            if (s_rx_idx >= WB_SOF_LEN + WB_HDR_LEN + s_rx_len + WB_CRC_LEN) {
                /* full frame received; verify CRC over LEN..PAYLOAD */
                uint16_t crc_calc = wb_crc16(&s_rx_buf[WB_SOF_LEN],
                                             WB_HDR_LEN + s_rx_len);
                uint8_t crc_lo = s_rx_buf[WB_SOF_LEN + WB_HDR_LEN + s_rx_len];
                uint8_t crc_hi = s_rx_buf[WB_SOF_LEN + WB_HDR_LEN + s_rx_len + 1];
                uint16_t crc_rx = (uint16_t)crc_lo | ((uint16_t)crc_hi << 8);
                bool     ok     = (crc_calc == crc_rx);
                /* Copy the valid frame out *before* wb_rx_reset(): the reset
                 * zeroes s_rx_len, so resetting first left every delivered
                 * frame with len == 0 and no payload at all (which silently
                 * disabled all EVT / status handling on the SK32 side). */
                if (ok && out) {
                    out->cmd   = s_rx_buf[3];
                    out->seq   = s_rx_buf[4];
                    out->flags = s_rx_buf[5];
                    out->len   = s_rx_len;
                    for (uint8_t i = 0u; i < s_rx_len; i++) {
                        out->payload[i] = s_rx_buf[WB_SOF_LEN + WB_HDR_LEN + i];
                    }
                }
                wb_rx_reset();
                return ok;
            }
            return false;
        }

        default:
            wb_rx_reset();
            return false;
    }
}
