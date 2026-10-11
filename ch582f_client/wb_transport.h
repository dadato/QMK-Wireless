/* Wireless bridge client — abstract transport interface.
 *
 * L1 boundary between the portable wb_session / wb_proto layers and the
 * platform transport.  The concrete SK32/HAL implementation lives in
 * wb_uart.c (this machine: USART1 on the pins/baud chosen by
 * wb_board_config).  Swap wb_uart.c for a different port and no session or
 * protocol code changes.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "wb_proto.h"

/* Bring the transport up (pins + receiver + serial).  Idempotent. */
void wb_uart_start(void);

/* Re-bring-up after STOP (peripheral clocks were torn down; PAL AF is re-armed
 * and the serial driver restarted). */
void wb_uart_restart(void);

/* Drain RX and feed bytes into the frame state machine.  Returns true when a
 * complete CRC-valid frame was produced (copied to *out). */
bool wb_uart_poll(wb_frame_t *out);

/* Enqueue one frame, honouring the minimum inter-frame gap.  Returns the
 * number of frame bytes written, or 0 if the gap has not elapsed (caller
 * retries next tick). */
uint16_t wb_uart_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                      const uint8_t payload[], uint8_t len);

/* Refresh link liveness: call once per received valid frame. */
void wb_uart_note_rx(void);

/* Last time (ms) any valid frame was received. */
uint32_t wb_uart_last_rx_ms(void);

/* Milliseconds since the last valid frame, evaluated against a fresh time
 * source.  Use this (not "now - last_rx_ms()") for liveness tests: the rx
 * timestamp is refreshed inside the same task tick, so a caller-captured "now"
 * is already stale and the subtraction underflows. */
uint32_t wb_uart_rx_age_ms(void);

uint32_t wb_uart_last_tx_ms(void);

/* Monotonic millisecond source injected into the protocol/session layers. */
uint32_t wb_now_ms(void);

/* Short blocking delay (milliseconds). */
void wb_delay_ms(uint32_t ms);
