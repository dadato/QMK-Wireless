/* Wireless bridge client — board decoupling seam.
 *
 * A keyboard hooks the portable wb_session / wb_uart library to the its own
 * hardware by defining two global const objects that this library externs:
 *
 *   extern const wb_board_config_t wb_board_config;   (pins, baud, identity...)
 *   extern const wb_board_ops_t     wb_board_ops;     (abstract behaviour)
 *
 * In effect that is the closest-to-“like QMK OLED” integration: the keyboard
 * supplies the two structs and calls only wb_init() (in keyboard_post_init_kb)
 * and wb_task() (in housekeeping_task_kb); every getter / control is a zero-
 * argument call on the portable API.
 *
 * Any line that a design does not wire must be PAL_NOLINE (e.g. the module
 * WAKE/LINK lines).  mode_line / pair_line / nreset_line are read/written by
 * wb_session through hal.h PAL calls, exactly like the original bridge_pins.h.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "hal.h"   /* ioline_t / PAL_LINE / PAL_NOLINE */

/* Cached wireless/bridge state snapshot delivered to on_state().  Fields are
 * the same values the wb_*() getters expose, gathered into one probe so a
 * board can react without pulling each getter. */
typedef struct wb_state_s {
    uint8_t  mode;      /* WB_MODE_* effective host-path mode */
    uint8_t  link;      /* 0 disconnected / 1 linked */
    uint8_t  profile;   /* active BLE host slot (1..4) */
    uint8_t  wl;        /* WB_WL_* aggregate wireless state */
    uint8_t  led;       /* last EVT_LEDS keyboard-LED bitmap */
    uint16_t batt_mv;   /* last battery voltage reading (0 = unknown) */
    uint8_t  batt_pct;  /* last battery percentage */
    bool     charging;  /* battery is charging (EVT_BATTERY flags bit0) */
} wb_state_t;

/* Board wiring + fixed parameters.  Zeroed / PAL_NOLINE fields fall back to
 * library defaults (see wb_session.c / wb_uart.c). */
typedef struct wb_board_config_s {
    ioline_t tx_line;      /* USART TX, or PAL_NOLINE */
    ioline_t rx_line;      /* USART RX, or PAL_NOLINE */
    ioline_t mode_line;    /* input+pulldown mode select, or PAL_NOLINE */
    ioline_t pair_line;    /* input+pulldown pair trigger, or PAL_NOLINE */
    ioline_t nreset_line;  /* output active-low module reset, or PAL_NOLINE */
    uint32_t uart_pal_mode; /* USART TX/RX pin alternate-function PAL mode; 0 -> default */
    uint32_t baud;         /* USART baud; 0 -> WB_BAUD default */
    uint16_t usb_vid;      /* wired VID pushed to the module / dongle */
    uint16_t usb_pid;      /* wired PID pushed to the module / dongle */
    uint16_t hid_gap_ms;   /* min inter-frame gap; 0 -> WB_HID_GAP_MS default */
    uint8_t  max_ble_profile; /* BLE host-slot ceiling (1..4); 0 -> 4 */
    uint8_t  batt_low_pct;    /* SK32-side low threshold; 0 -> WB_BATT_LOW_PCT */
    uint8_t  batt_crit_pct;   /* SK32-side crit threshold; 0 -> WB_BATT_CRIT_PCT */
} wb_board_config_t;

/* Abstract board behaviour the portable session calls instead of touching
 * OLED / deep-sleep / USB / ADC hardware directly.  Pointers may be NULL; a
 * NULL callback is treated as a no-op / “not supported” default. */
typedef struct wb_board_ops_s {
    /* Delivers a full cached-state snapshot on every meaningful change. */
    void (*on_state)(const wb_state_t *st);
    /* Reports the last EVT_ERROR code (WB_ECODE_*). */
    void (*on_error)(uint8_t code);
    /* Enter the board's own deep sleep (e.g. SK32 STOP) once the module has
     * been commanded to MODE_SLEEP.  Returns true if a real (non-tick) wake
     * occurred (a key), on which the session resyncs the module.  NULL skips
     * board deep sleep (the module still sleeps on CMD_SLEEP). */
    bool (*deep_sleep)(void);
    /* True while a USB host is enumerated (drives wired-vs-wireless and the
     * sleep-permit policy).  NULL => USB treated as never present. */
    bool (*usb_present)(void);
    /* SK32-side battery sampling: *mv = millivolts, *pct = percent,
     * *charging = charger active.  Returns false if the board owns no battery
     * / sampling is unavailable (the session then keeps reporting the
     * last-known percentage).  *charging is left untouched when sampling
     * fails. */
    bool (*batt_sample)(uint16_t *mv, uint8_t *pct, bool *charging);
} wb_board_ops_t;

/* Provided by the keyboard. */
extern const wb_board_config_t wb_board_config;
extern const wb_board_ops_t     wb_board_ops;
