/* USART1 transport for the wireless bridge client (SK32 HAL port).
 *
 * Concrete implementation of wb_transport.h.  The pins (tx/rx), their PAL
 * alternate-function mode and the baud / inter-frame gap all come from the
 * board-supplied wb_board_config, so the same wb_uart.c drives any SK32 board
 * without a keyboard-local bridge_pins.h.  Also provides the monotonic
 * millisecond source (wb_now_ms) and blocking delay (wb_delay_ms) that
 * wb_proto / wb_session consume.
 */
#include <string.h>

#include "wb_board.h"
#include "wb_transport.h"

#include "ch.h"
#include "hal.h"

/* USART alternate-function default (SK32 USART1 = PA9/PA10, AF10 mid-speed) is
 * used when wb_board_config.uart_pal_mode == 0. */
#ifndef WB_UART_PAL_MODE
#define WB_UART_PAL_MODE (PAL_MODE_ALTERNATE(10) | PAL_SK32_OSPEED_MID)
#endif

#ifndef WB_BAUD
#define WB_BAUD 921600u
#endif

/* Gap between any two frames on the wire (matches bridge_config.h
 * BRIDGE_FRAME_MIN_GAP_MS).  At 921600 a max-size frame takes ~0.75 ms. */
#ifndef WB_HID_GAP_MS
#define WB_HID_GAP_MS 5u
#endif

static SerialConfig s_uart_cfg = {
    .speed = WB_BAUD,
    .cr1   = 0u,
    .cr2   = USART_CR2_STOP1_BITS, /* 1 stop bit, 8N1 */
    .cr3   = 0u,
};

static uint32_t s_last_rx_ms = 0u;
static uint32_t s_last_tx_ms = 0u;
static bool     s_started   = false;

/* Bench diagnostics: raw byte / frame counters, readable over SWD so the link
 * can be judged without a logic analyser (COM10 may be off the module). */
volatile uint32_t wb_dbg_rx_bytes  = 0u;
volatile uint32_t wb_dbg_rx_frames = 0u;

/* Rolling window of the last 16 raw bytes pulled off USART1, so a frame can be
 * decoded by hand over SWD when the peer firmware's reply format is unknown. */
volatile uint8_t  wb_dbg_raw[16];
volatile uint8_t  wb_dbg_raw_idx = 0u;

/* Last successfully transmitted command + total TX frames + a short history of
 * the recent TX commands (bench visibility for one-shot frames). */
volatile uint8_t  wb_dbg_tx_cmd  = 0u;
volatile uint32_t wb_dbg_tx_cnt  = 0u;
volatile uint8_t  wb_dbg_tx_hist[8];
volatile uint8_t  wb_dbg_tx_hist_idx = 0u;

uint32_t wb_now_ms(void) {
    return TIME_I2MS(chVTGetSystemTime());
}

void wb_delay_ms(uint32_t ms) {
    chThdSleep(TIME_MS2I(ms));
}

static uint32_t uart_baud(void) {
    return wb_board_config.baud ? wb_board_config.baud : WB_BAUD;
}

static uint32_t uart_hid_gap(void) {
    return wb_board_config.hid_gap_ms ? wb_board_config.hid_gap_ms : WB_HID_GAP_MS;
}

static uint32_t uart_pal_mode(void) {
    return wb_board_config.uart_pal_mode ? wb_board_config.uart_pal_mode
                                         : (uint32_t)WB_UART_PAL_MODE;
}

void wb_uart_start(void) {
    if (s_started) {
        return;
    }
    /* USART1 on the configured TX/RX pins, AF10 (mid-speed OSPEED). */
    if (wb_board_config.tx_line != PAL_NOLINE) {
        palSetLineMode(wb_board_config.tx_line, uart_pal_mode());
    }
    if (wb_board_config.rx_line != PAL_NOLINE) {
        palSetLineMode(wb_board_config.rx_line, uart_pal_mode());
    }
    /* bind the board-supplied baud before starting the driver */
    s_uart_cfg.speed = uart_baud();
    sdStart(&SD1, &s_uart_cfg);
    wb_rx_reset();
    s_started = true;
}

void wb_uart_restart(void) {
    if (s_started) {
        sdStop(&SD1);
        s_started = false;
    }
    wb_uart_start();
}

uint16_t wb_uart_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                      const uint8_t payload[], uint8_t len) {
    uint32_t now = wb_now_ms();
    if ((now - s_last_tx_ms) < uart_hid_gap()) {
        return 0u; /* min inter-frame gap not met yet */
    }
    uint8_t  buf[WB_FRAME_MAX];
    uint16_t n = wb_encode(buf, sizeof(buf), cmd, seq, flags, payload, len);
    if (n == 0u) {
        return 0u;
    }
    /* Serial write; the SK32 serial driver buffers out to a ring. */
    msg_t res = chnWriteTimeout(&SD1, buf, n, TIME_MS2I(10u));
    if (res < (msg_t)n) {
        return 0u; /* ring full / timed out; caller retries next tick */
    }
    s_last_tx_ms = now;
    wb_dbg_tx_cmd = cmd;
    wb_dbg_tx_cnt++;
    wb_dbg_tx_hist[wb_dbg_tx_hist_idx & 0x07u] = cmd;
    wb_dbg_tx_hist_idx++;
    return n;
}

bool wb_uart_poll(wb_frame_t *out) {
    msg_t   r;
    bool    got = false;
    /* Drain as much as is ready (non-blocking-ish small timeout). */
    while ((r = sdGetTimeout(&SD1, TIME_I2MS(0u))) != Q_TIMEOUT) {
        if (r < (msg_t)0) {
            break; /* error state */
        }
        wb_dbg_rx_bytes++;
        wb_dbg_raw[wb_dbg_raw_idx & 0x0Fu] = (uint8_t)r;
        wb_dbg_raw_idx++;
        if (wb_rx_byte((uint8_t)r, out)) {
            got    = true;
            wb_dbg_rx_frames++;
            s_last_rx_ms = wb_now_ms();
            break; /* one frame per poll is enough; rest drained next tick */
        }
    }
    return got;
}

void wb_uart_note_rx(void) {
    s_last_rx_ms = wb_now_ms();
}

uint32_t wb_uart_last_rx_ms(void) {
    return s_last_rx_ms;
}

uint32_t wb_uart_rx_age_ms(void) {
    return wb_now_ms() - s_last_rx_ms;
}

uint32_t wb_uart_last_tx_ms(void) {
    return s_last_tx_ms;
}
