/* Wireless bridge client - L2 session engine.
 *
 * Owns boot init, the periodic housekeeping task (RX drain, module recovery,
 * USB/wireless switch, idle sleep) and the mode/EVT handling.  Board hardware
 * (USB presence, battery sampling, OLED, deep sleep) is reached exclusively
 * through wb_board_ops / wb_board_config, never directly.
 */

#include <string.h>

#include "wb_session.h"
#include "wb_host.h"
#include "wb_proto.h"
#include "wb_transport.h"

#include "action.h"          /* clear_keyboard() */
#include "host.h"

#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"  /* rgb_matrix_set_suspend_state() */
#endif

#ifdef RAW_ENABLE
#    include "raw_hid.h"     /* raw_hid_receive() - VIA tunnel endpoint */
#endif

/* ---- SK32-side tuning (library defaults; a keyboard may predefine any of
 *      these or override them through wb_board_config). ---- */

#ifndef WB_MODE_TIMEOUT_MS
#define WB_MODE_TIMEOUT_MS 300u
#endif
#ifndef WB_WL_POLL_MS
#define WB_WL_POLL_MS     1000u
#endif
#ifndef WB_WL_POLL_FAST_MS
#define WB_WL_POLL_FAST_MS 150u
#endif
#ifndef WB_IDLE_SLEEP_MS
#define WB_IDLE_SLEEP_MS  300000u /* 5 min idle -> deep sleep */
#endif
#ifndef WB_BATT_PERIOD_MS
#define WB_BATT_PERIOD_MS 1000u   /* battery report while working */
#endif
#ifndef WB_MODULE_RECOVER_MS
#define WB_MODULE_RECOVER_MS 5000u /* no module reply -> reset + re-handshake */
#endif
#ifndef WB_CRIT_SLEEP_MS
#define WB_CRIT_SLEEP_MS  10000u  /* crit battery: deep sleep after 10 s */
#endif
#ifndef WB_HOST_SUSPEND_SLEEP_MS
#define WB_HOST_SUSPEND_SLEEP_MS 30000u /* host suspended: 30 s idle */
#endif
#ifndef WB_ERR_SHOW_MS
#define WB_ERR_SHOW_MS    10000u  /* EVT_ERROR shown on the OLED */
#endif
#ifndef WB_PAIR_SHOW_MS
#define WB_PAIR_SHOW_MS   5000u   /* EVT_PAIR_RESULT shown on the OLED */
#endif
#ifndef WB_IDENT_SETTLE_MS
#define WB_IDENT_SETTLE_MS 150u  /* give the 2.4G relay link time to settle */
#endif
#ifndef WB_DONGLE_NAME_MAX
#define WB_DONGLE_NAME_MAX 21u   /* module BRIDGE_NAME_MAX */
#endif
/* USB product name pushed to the receiver; defaults to the wired product name
 * (QMK PRODUCT == keyboard_name) so both paths enumerate identically. */
#ifndef WB_DONGLE_NAME
#define WB_DONGLE_NAME PRODUCT
#endif

/* battery thresholds: wb_board_config overrides when non-zero */
#define BATT_LOW_PCT()  (wb_board_config.batt_low_pct  ? wb_board_config.batt_low_pct  : WB_BATT_LOW_PCT)
#define BATT_CRIT_PCT() (wb_board_config.batt_crit_pct ? wb_board_config.batt_crit_pct : WB_BATT_CRIT_PCT)

/* ---------------- cached state ---------------- */
static uint8_t  s_mode      = WB_MODE_BLE; /* effective host path mode */
static uint8_t  s_link      = 0u;          /* 0 disconnected / 1 linked */
static uint8_t  s_profile   = 0u;
static uint8_t  s_batt_pct  = 0u;
static uint16_t s_batt_mv   = 0u;
static uint8_t  s_led       = 0u;

static uint32_t s_last_key_ms  = 0u; /* last key activity (wired+wifi) */

static bool s_wireless_path = false; /* true when wireless host driver active */
static bool s_link_lost     = false; /* kept false: no link-loss judgement */
static bool s_inited        = false;

/* wireless aggregate state + host power, refreshed by CMD_GET_WIRELESS_STATE
 * ACK replies and EVT_HOST_POWER events. */
static uint8_t  s_wl           = WB_WL_UNKNOWN;
static bool     s_host_awake   = true;
static uint32_t s_last_wl_q_ms = 0u;
static uint8_t  s_sleep_lvl    = 0u; /* last sleep level commanded to the module */
/* Idle-to-deep-sleep countdown for the OLED: current (dynamic) threshold and
 * whether the countdown is running (wireless, no USB, not yet asleep). */
static uint32_t s_sleep_thresh_ms = WB_IDLE_SLEEP_MS;
static bool     s_sleep_counting  = false;

/* RGB suspend state while the SK32 is in wireless deep sleep (edge-triggered). */
static bool     s_rgb_suspended = false;

/* hardware-driven mode tracking + pairing button edge tracking */
static uint8_t  s_hw_last      = 0xFFu; /* last hardware-derived mode sent */
static uint8_t  s_mode_want    = WB_MODE_BLE; /* desired mode: hw default or keycode */
static bool     s_usb_prev     = false;
static bool     s_pair_prev    = false;
static uint32_t s_last_batt_ms = 0u;  /* last SET_BATTERY report time */

/* EVT_ERROR / battery low-crit state (SK32-side) */
static uint8_t  s_ecode      = 0u;
static uint32_t s_ecode_ms   = 0u;
static bool     s_batt_low   = false;
static bool     s_batt_crit  = false;

/* Last EVT_PAIR_RESULT: [result u8][profile u8].  result 0 = paired, 1 = failed
 * or pairing-window timeout.  Kept with a short display window for the OLED. */
static uint8_t  s_pair_result  = 0xFFu;
static uint8_t  s_pair_profile = 0u;
static uint32_t s_pair_ms      = 0u;

/* Pending receiver-identity push after a successful 2.4G pairing:
 * 0 = idle, 1 = send SET_USB_ID, 2 = send SET_NAME.  s_ident_ms is the earliest
 * time the current stage may go out (settle delay / UART gap). */
static uint8_t  s_ident_step = 0u;
static uint32_t s_ident_ms   = 0u;

/* latest-only tx slot (only one frame in flight between ticks) */
static uint8_t  s_tx_payload[WB_MAX_PAYLOAD];
static uint8_t  s_tx_len;
static uint8_t  s_tx_cmd;
static bool     s_tx_pending = false;

/* Bench diagnostics (readable over SWD): last decoded frame + recovery count. */
volatile uint32_t wb_dbg_frames     = 0u;
volatile uint8_t  wb_dbg_last_cmd   = 0u;
volatile uint8_t  wb_dbg_last_flags = 0u;
volatile uint8_t  wb_dbg_last_len   = 0u;
volatile uint8_t  wb_dbg_last_seq   = 0u;
volatile uint8_t  wb_dbg_last_pl0   = 0u;
volatile uint32_t wb_dbg_recover    = 0u;

/* ---- battery threshold defaults (overridable by wb_board_config) ---- */
#ifndef WB_BATT_LOW_PCT
#define WB_BATT_LOW_PCT 20u
#endif
#ifndef WB_BATT_CRIT_PCT
#define WB_BATT_CRIT_PCT 5u
#endif

/* ---------------- wiring helpers (board pins from wb_board_config) ---- */

static bool board_usb_present(void) {
    return wb_board_ops.usb_present ? wb_board_ops.usb_present() : false;
}

/* PAL_NOLINE = line not wired: high falls back to low (BLE). */
static bool board_mode_line_high(void) {
    return wb_board_config.mode_line != PAL_NOLINE && palReadLine(wb_board_config.mode_line);
}

static bool board_pair_pressed(void) {
    return wb_board_config.pair_line != PAL_NOLINE && palReadLine(wb_board_config.pair_line);
}

static uint8_t board_max_profile(void) {
    uint8_t m = wb_board_config.max_ble_profile;
    return (m >= 1u && m <= 4u) ? m : 4u;
}

/* ---------------- helpers ---------------- */

void wb_cmd(uint8_t cmd, const uint8_t payload[], uint8_t len) {
    if (!s_inited) {
        return;
    }
    wb_uart_send(cmd, wb_seq_next(), 0u, payload, len);
}

uint16_t wb_tx_hid(uint8_t cmd, const uint8_t payload[], uint8_t len) {
    if (!s_wireless_path) {
        return 0u;
    }
    if (len > WB_MAX_PAYLOAD) {
        len = WB_MAX_PAYLOAD;
    }
    /* any outbound HID activity counts as keyboard use (idle tracker) */
    s_last_key_ms = wb_now_ms();
    /* latest-only: if a previous frame is still waiting on the gap, overwrite. */
    memcpy(s_tx_payload, payload, len);
    s_tx_cmd = cmd;
    s_tx_len = len;
    s_tx_pending = true;
    return len; /* enqueued; flush happens in wb_task */
}

static void wb_set_wireless(bool wireless) {
    if (wireless == s_wireless_path) {
        return;
    }
    wb_host_use_wireless(wireless);
    s_wireless_path = wireless;
    s_last_key_ms   = wb_now_ms();
}

static void wb_emit_state(void) {
    if (!wb_board_ops.on_state) {
        return;
    }
    wb_state_t st;
    st.mode     = s_mode;
    st.link     = s_link;
    st.profile  = s_profile;
    st.wl       = s_wl;
    st.led      = s_led;
    st.batt_mv  = s_batt_mv;
    st.batt_pct = s_batt_pct;
    wb_board_ops.on_state(&st);
}

void wb_mode_request(uint8_t mode) {
    /* Runtime mode switch: tell the bridge via CMD_SET_MODE and repoint the
     * host path accordingly.  The module mode is fully command-driven now
     * (the MODE_SEL hardware lines are not connected).  s_mode_want records
     * the choice so a later recovery-reset (wb_module_setup) re-issues the
     * same mode instead of the hardware default.  s_hw_last is not touched, so
     * the hardware-driven state machine only re-issues a mode when the USB
     * state or the mode-line level actually changes. */
    s_mode_want = mode;
    uint8_t persist[2];
    persist[0] = mode;
    persist[1] = 1u; /* persist on the module (USB without VBUS won't persist) */
    wb_cmd(WB_CMD_SET_MODE, persist, 2u);
    /* repoint host path: only BLE/2G4 use the wireless driver */
    wb_set_wireless((mode == WB_MODE_BLE || mode == WB_MODE_BLE_PAIR ||
                     mode == WB_MODE_2G4 || mode == WB_MODE_2G4_PAIR));
}

void wb_pair(void) {
    /* 配对统一走 CMD_PAIR_START：模块按当前无线模式自选 BLE/2.4G 配对目标。 */
    wb_cmd(WB_CMD_PAIR_START, NULL, 0u);
}

/* ---------------- EVT handling ---------------- */

/* Evaluate low/crit battery from the current cached percentage.  SK32-side
 * thresholds: low <= LOW_PCT, crit <= CRIT_PCT. */
static void wb_batt_eval(void) {
    s_batt_low  = (s_batt_pct <= BATT_LOW_PCT()) && (s_batt_pct > BATT_CRIT_PCT());
    s_batt_crit = (s_batt_pct <= BATT_CRIT_PCT());
}

static void handle_evt_state(const wb_frame_t *f) {
    if (f->len >= 4u) {
        uint8_t mode = f->payload[0];
        /* MODE_SLEEP is a transient the module reports while it sleeps (and
         * when commanded DEEP).  Keep the last real mode so the host-path and
         * mode-cycling logic never mistake the board for being in SLEEP. */
        if (mode != WB_MODE_SLEEP) {
            s_mode = mode;
        }
        s_link     = f->payload[1];
        s_profile  = f->payload[2];
        s_batt_pct = f->payload[3];
        wb_batt_eval();
    }
    s_link_lost = false;
    wb_emit_state();
}

static void handle_evt_leds(const wb_frame_t *f) {
    if (f->len >= 1u) {
        s_led = f->payload[0];
        wb_emit_state();
    }
}

static void handle_evt_battery(const wb_frame_t *f) {
    if (f->len >= 3u) {
        s_batt_mv  = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
        s_batt_pct = f->payload[2];
        wb_batt_eval();
        wb_emit_state();
    }
}

/* Last EVT_ERROR code, cached with a display window so the OLED can show it
 * without the user catching the frame live, and bubbled to the board op. */
static void handle_evt_error(const wb_frame_t *f) {
    if (f->len >= 1u) {
        s_ecode    = f->payload[0];
        s_ecode_ms = wb_now_ms();
        if (wb_board_ops.on_error) {
            wb_board_ops.on_error(s_ecode);
        }
    }
}

/* EVT_PAIR_RESULT [result][profile]: 0 = pairing succeeded, 1 = failed or the
 * pairing window timed out.  Cached so the OLED can show "PAIR SUCCESS" /
 * "PAIR FAILED" for a few seconds after the fact. */
static void handle_evt_pair_result(const wb_frame_t *f) {
    if (f->len >= 1u) {
        s_pair_result  = f->payload[0];
        s_pair_profile = (f->len >= 2u) ? f->payload[1] : 0u;
        s_pair_ms      = wb_now_ms();

        /* On a successful 2.4G pairing, queue the receiver-identity push.  The
         * pairing radio cannot be told from the event payload (its second byte
         * is ble_profile, not the radio), so it is read from the session mode:
         * s_mode still holds the pairing mode here because the 2.4G EVT_STATE
         * that follows is handled on the next tick.  s_mode_want is a fallback
         * for a lost EVT_STATE. */
        bool is_2g4 = (s_mode == WB_MODE_2G4 || s_mode == WB_MODE_2G4_PAIR ||
                       s_mode_want == WB_MODE_2G4 || s_mode_want == WB_MODE_2G4_PAIR);
        if (s_pair_result == 0u && is_2g4) {
            s_ident_step = 1u;
            s_ident_ms   = wb_now_ms() + WB_IDENT_SETTLE_MS;
        }
    }
}

/* Host power state from the module (USB suspend/resume, or a BLE HID control
 * point change).  On resume it counts as activity: restart the idle tracker and
 * release any sleep permit so the module stays awake with the host. */
static void handle_evt_host_power(const wb_frame_t *f) {
    if (f->len < 1u) {
        return;
    }
    s_host_awake = (f->payload[0] != WB_HOST_SUSPEND);
    if (s_host_awake) {
        s_last_key_ms = wb_now_ms();
        if (s_sleep_lvl != 0u) {
            s_sleep_lvl = 0u;
            wb_cmd(WB_CMD_SLEEP, (const uint8_t[1]){WB_SLEEP_WAKE}, 1u);
        }
    }
}

#ifdef RAW_ENABLE
/* discards driven by malformed tunnel reports (length != RAW_EPSIZE); kept for
 * field diagnosis, mirrors the design doc risk table. */
static uint16_t s_tunnel_drop = 0u;

/* Inbound VIA raw-HID bytes tunnelled from the module must reach the normal
 * QMK raw-HID handler.  VIA's reply is emitted with raw_hid_send(), which the
 * wireless host driver (wb_raw) routes back over the same tunnel.  Both
 * wireless paths carry this: 2.4G terminates at the Dongle's USB raw HID
 * endpoint, and BLE rides the module's HOGP vendor report (RID 5, QMK Raw HID)
 * layout for usevia.app over WebHID. */
static void handle_evt_tunnel(const wb_frame_t *f) {
    /* VIA reports are always RAW_EPSIZE (== 32) bytes; anything else would
     * over-read the fixed raw-HID buffer, so drop and count it. */
    if (f->len != WB_TUNNEL_MAX_LEN) {
        s_tunnel_drop++;
        return;
    }
    raw_hid_receive((uint8_t *)f->payload, WB_TUNNEL_MAX_LEN);
}
#endif

/* ACK an event frame that requested acknowledgement (only state/leds/pair/
 * error carry ACK_REQ from the bridge). */
static void ack_event(const wb_frame_t *f) {
    if (f->flags & WB_FLAG_ACK_REQ) {
        /* echo the same (cmd, seq) with the ACK flag set, empty payload */
        wb_uart_send(f->cmd, f->seq, WB_FLAG_ACK, NULL, 0u);
    }
}

void wb_on_frame(const wb_frame_t *f) {
    if (!s_inited) {
        return;
    }
    /* any valid frame refreshes link liveness */
    wb_uart_note_rx();

    wb_dbg_frames++;
    wb_dbg_last_cmd   = f->cmd;
    wb_dbg_last_flags = f->flags;
    wb_dbg_last_len   = f->len;
    wb_dbg_last_seq   = f->seq;
    wb_dbg_last_pl0   = (f->len >= 1u) ? f->payload[0] : 0u;

    switch (f->cmd) {
        case WB_EVT_STATE:       handle_evt_state(f);   ack_event(f); break;
        case WB_EVT_LEDS:        handle_evt_leds(f);    ack_event(f); break;
        case WB_EVT_BATTERY:     handle_evt_battery(f); break;
        case WB_EVT_PAIR_RESULT: handle_evt_pair_result(f); ack_event(f); break;
        case WB_EVT_ERROR:       handle_evt_error(f); ack_event(f); break;
        case WB_EVT_HOST_POWER:  handle_evt_host_power(f); ack_event(f); break;
#ifdef RAW_ENABLE
        case WB_EVT_TUNNEL_USB:  handle_evt_tunnel(f);  break;
        case WB_EVT_TUNNEL_BLE:  handle_evt_tunnel(f);  break;
#endif
        case WB_CMD_GET_STATUS:
            /* ACK reply to our own CMD_GET_STATUS (sent at init / on demand):
             * [mode][connected][profile][battery%][sleep_policy].  The battery
             * mv value is not in this reply; it comes from EVT_BATTERY. */
            if (((f->flags & WB_FLAG_ACK) != 0u) && f->len >= 5u) {
                if (f->payload[0] != WB_MODE_SLEEP) {
                    s_mode = f->payload[0];
                }
                s_link     = f->payload[1];
                s_profile  = f->payload[2];
                s_batt_pct = f->payload[3];
                s_link_lost = false;
                wb_emit_state();
            }
            break;
        case WB_CMD_GET_WIRELESS_STATE:
            /* ACK reply to our own CMD_GET_WIRELESS_STATE query: [wl_state] */
            if (((f->flags & WB_FLAG_ACK) != 0u) && f->len >= 1u) {
                s_wl = f->payload[0];
                wb_emit_state();
            }
            break;
        case WB_EVT_PONG:
            /* Heartbeat is disabled on the module, so PONG should never arrive
             * (no PING sent).  Harmless if it does. */
            s_link_lost = false;
            break;
        default:
            break;
    }
}

/* ---------------- init ---------------- */

/* Timestamp of the last module bring-up, used by the recovery watchdog. */
static uint32_t s_last_recover_ms = 0u;

/* Hardware-derived host mode the SK32 should command the module into. */
static uint8_t wb_hw_mode(void) {
    bool usb = board_usb_present();
    return usb ? WB_MODE_USB
               : (board_mode_line_high() ? WB_MODE_2G4 : WB_MODE_BLE);
}

/* Module bring-up: hard-reset, then push the full configuration.
 *
 * Heartbeat OFF (SK32 owns sleep/link judgement, so the module must not PING or
 * fall into MODE_SLEEP on its own), MANUAL sleep policy (only the SLEEP command
 * may put the module to sleep), battery reported by SK32 (EXTERNAL source), and
 * the module USB identity forced to the wired VID/PID so the 2.4G dongle
 * enumerates exactly like the wired board.  The module persists the identity in
 * NV and relays it to the dongle, so re-sending it is harmless / idempotent.
 * The UART honours the inter-frame gap and wb_cmd() is fire-and-forget, so
 * every command is paced.
 *
 * Called once from wb_init() and again by the recovery watchdog in wb_task()
 * when the module stays silent: a module that booted into sleep (AUTO policy
 * left in NV) or dropped the first handshake only recovers after a fresh reset
 * + handshake. */
static void wb_module_setup(void) {
    if (wb_board_config.nreset_line != PAL_NOLINE) {
        palWriteLine(wb_board_config.nreset_line, 0);
        wb_delay_ms(10u);
        palWriteLine(wb_board_config.nreset_line, 1);
        wb_delay_ms(200u); /* let the module boot */
    }

    /* Establish the host mode *before* the handshake (see original notes: a
     * module fresh out of reset sits in its boot mode with radio down).
     * Re-issuing the *desired* mode (s_mode_want) rather than the raw hardware
     * default keeps a keycode override (WB_KC_2G4 / WB_MODE_NEXT) from being
     * dropped back to BLE on a watchdog reset and tearing the 2.4G link the
     * VIA tunnel depends on. */
    uint8_t hw = wb_hw_mode();
    wb_mode_request(s_mode_want);
    s_hw_last  = hw;
    s_usb_prev = (hw == WB_MODE_USB);
    wb_delay_ms(6u);

    wb_cmd(WB_CMD_SET_HEARTBEAT, (const uint8_t[1]){0u}, 1u);
    wb_delay_ms(6u);
    wb_cmd(WB_CMD_SET_CONFIG,
           (const uint8_t[2]){WB_CFG_KEY_SLEEP_POLICY, WB_SLEEP_POLICY_MANUAL}, 2u);
    wb_delay_ms(6u);
    wb_cmd(WB_CMD_SET_CONFIG,
           (const uint8_t[2]){WB_CFG_KEY_BATTERY_SRC, WB_BATT_SRC_EXTERNAL}, 2u);
    wb_delay_ms(6u);
    /* USB identity = wired identity (keyboard.json usb.vid/pid), little-endian. */
    wb_cmd(WB_CMD_SET_USB_ID,
           (const uint8_t[4]){(uint8_t)(wb_board_config.usb_vid & 0xFFu),
                              (uint8_t)(wb_board_config.usb_vid >> 8),
                              (uint8_t)(wb_board_config.usb_pid & 0xFFu),
                              (uint8_t)(wb_board_config.usb_pid >> 8)},
           4u);
    wb_delay_ms(6u);
    wb_cmd(WB_CMD_GET_VERSION, NULL, 0u);
    wb_delay_ms(6u);
    wb_cmd(WB_CMD_GET_STATUS, NULL, 0u);
}

void wb_init(void) {
    if (s_inited) {
        return;
    }

    /* control GPIOs: mode select + pairing trigger are inputs with internal
     * pull-down (default BLE / no pairing); nRESET is an output.  Lines not
     * wired (PAL_NOLINE) are skipped. */
    if (wb_board_config.mode_line != PAL_NOLINE) {
        palSetLineMode(wb_board_config.mode_line, PAL_MODE_INPUT_PULLDOWN);
    }
    if (wb_board_config.pair_line != PAL_NOLINE) {
        palSetLineMode(wb_board_config.pair_line, PAL_MODE_INPUT_PULLDOWN);
    }
    if (wb_board_config.nreset_line != PAL_NOLINE) {
        palSetLineMode(wb_board_config.nreset_line, PAL_MODE_OUTPUT_PUSHPULL);
    }

    /* bring up the transport first so the bring-up frames can go out */
    wb_uart_start();

    /* Mark initialised *before* the handshake: wb_cmd() is gated on s_inited,
     * so calling wb_module_setup() first (as before) would swallow the whole
     * boot handshake. */
    s_inited = true;

    /* desired mode defaults to the hardware-derived one; a keycode override
     * (wb_mode_request) replaces it and is preserved across recovery resets. */
    s_mode_want = wb_hw_mode();

    /* reset + configure the module */
    wb_module_setup();

    s_last_recover_ms = wb_now_ms();
}

/* ---------------- periodic task ---------------- */

/* forward the pending latest-only HID frame, respecting the inter-frame gap */
static bool tx_flush_latest(void) {
    if (!s_tx_pending || !s_wireless_path) {
        return true; /* nothing waiting / not in wireless */
    }
    /* send only if the gap allows; otherwise keep pending for next tick */
    uint16_t n = wb_uart_send(s_tx_cmd, 0u, 0u, s_tx_payload, s_tx_len);
    if (n != 0u) {
        s_tx_pending = false;
        return true;
    }
    return false;
}

void wb_task(void) {
    if (!s_inited) {
        wb_init();
        return;
    }

    uint32_t now = wb_now_ms();

    /* 1. drain RX -> one frame per call */
    wb_frame_t f;
    if (wb_uart_poll(&f)) {
        wb_on_frame(&f);
    }

    /* 1b. module recovery watchdog: no valid frame has arrived since the last
     * handshake -> re-run the reset + handshake.  Covers a module that booted
     * into sleep (AUTO policy left in NV) or lost the initial handshake before
     * its UART was ready.
     *
     * Liveness is "any valid frame received recently" (wb_uart_rx_age_ms), NOT
     * "the module answered CMD_GET_WIRELESS_STATE": s_wl only ever changes on
     * that one reply, so gating on it hard-reset the module every 5 s even
     * while it was answering the other frames perfectly. */
    if ((now - s_last_recover_ms) >= WB_MODULE_RECOVER_MS &&
        wb_uart_rx_age_ms() >= WB_MODULE_RECOVER_MS) {
        s_last_recover_ms = now;
        wb_dbg_recover++;
        wb_module_setup();
    }

    /* 2. mode is SK32-driven: USB when enumerated, otherwise the mode-line
     * switch (low = BLE, high = 2.4G).  Edge-triggered on the USB state / mode
     * line so a manual keycode override (wb_mode_request) survives until the
     * hardware actually moves. */
    bool usb = board_usb_present();
    uint8_t hw_want = wb_hw_mode();
    if ((hw_want != s_hw_last) || (usb != s_usb_prev)) {
        s_hw_last = hw_want;
        wb_mode_request(hw_want);
    }
    s_usb_prev = usb;
    if (usb) {
        s_wl = WB_WL_USB; /* wired: radio is off, report the USB state */
    }

    /* 2b. pairing-line rising edge (wireless only): start the pairing window.
     * The module exits the window after 30 s by itself (pairing timeout). */
    bool pair = board_pair_pressed();
    if (pair && !s_pair_prev && !usb) {
        wb_cmd(WB_CMD_PAIR_START, NULL, 0u);
    }
    s_pair_prev = pair;

    /* 3. flush any throttled HID frame */
    tx_flush_latest();

    /* 3b. receiver-identity push queued by a successful 2.4G pairing: send the
     * dongle's USB VID/PID, then its USB product name, over the 2.4G relay.
     * One stage per tick; a stage advances only once the transport accepts the
     * frame. */
    if (s_ident_step != 0u && (int32_t)(now - s_ident_ms) >= 0) {
        bool sent = false;
        if (s_ident_step == 1u) {
            sent = wb_uart_send(WB_CMD_SET_USB_ID, wb_seq_next(), 0u,
                                (const uint8_t[4]){(uint8_t)(wb_board_config.usb_vid & 0xFFu),
                                                   (uint8_t)(wb_board_config.usb_vid >> 8),
                                                   (uint8_t)(wb_board_config.usb_pid & 0xFFu),
                                                   (uint8_t)(wb_board_config.usb_pid >> 8)},
                                4u) != 0u;
        } else {
            uint8_t nlen = (uint8_t)strlen(WB_DONGLE_NAME);
            if (nlen > WB_DONGLE_NAME_MAX) {
                nlen = WB_DONGLE_NAME_MAX;
            }
            uint8_t pl[1u + WB_DONGLE_NAME_MAX];
            pl[0] = WB_NAME_TARGET_USB;
            memcpy(&pl[1], WB_DONGLE_NAME, nlen);
            sent = wb_uart_send(WB_CMD_SET_NAME, wb_seq_next(), 0u, pl,
                                (uint8_t)(1u + nlen)) != 0u;
        }
        if (sent) {
            if (s_ident_step >= 2u) {
                s_ident_step = 0u; /* push complete */
            } else {
                s_ident_step++;
                s_ident_ms = now;  /* next stage waits on the UART gap */
            }
        }
    }

    /* 4. wireless-state poll for the OLED.  No heartbeat at all: the module
     * heartbeat was disabled at init and the session does not judge link-loss
     * (the wireless state + EVT_STATE carry the link info).  Suppressed while
     * the board is in deep sleep. */
    if (s_wireless_path && s_sleep_lvl < 2u) {
        /* While the module has never replied, poll fast: its LIGHT sleep only
         * samples the UART during a short post-sleep listen window, so a slow
         * poll would rarely land inside it and the module would stay silent. */
        uint32_t poll_ms = (s_wl == WB_WL_UNKNOWN) ? WB_WL_POLL_FAST_MS : WB_WL_POLL_MS;
        if ((now - s_last_wl_q_ms) >= poll_ms) {
            if (wb_uart_send(WB_CMD_GET_WIRELESS_STATE, wb_seq_next(), 0u, NULL, 0u) != 0u) {
                s_last_wl_q_ms = now;
            }
        }
    }

    /* 4b. battery: the SK32 owns the reading (board op), reports to the module
     * once per second while working, not while sleeping. */
    if (s_sleep_lvl < 2u) {
        if ((now - s_last_batt_ms) >= WB_BATT_PERIOD_MS) {
            s_last_batt_ms = now;
            uint16_t mv  = s_batt_mv;
            uint8_t  pct = 0u;
            if (wb_board_ops.batt_sample && wb_board_ops.batt_sample(&mv, &pct)) {
                s_batt_mv  = mv;
                s_batt_pct = pct;
            }
            wb_batt_eval();
            wb_cmd(WB_CMD_SET_BATTERY, (const uint8_t[1]){s_batt_pct}, 1u);
        }
    }

    /* 5. wireless idle sleep collaboration.  Edge-triggered on the level so
     * CMD_SLEEP is sent only on a transition, not every tick.  The idle
     * threshold is dynamic (the SK32 decides): crit battery shortens it to a
     * shutdown guard, a suspended host to 30 s, otherwise 5 min. */
    uint32_t idle_ms = now - s_last_key_ms;
    uint32_t sleep_ms = WB_IDLE_SLEEP_MS;
    if (s_batt_crit) {
        sleep_ms = WB_CRIT_SLEEP_MS;
    } else if (!s_host_awake) {
        sleep_ms = WB_HOST_SUSPEND_SLEEP_MS;
    }
    s_sleep_thresh_ms = sleep_ms; /* exposed to the OLED countdown */
    /* sleep only on the wireless path with no USB host */
    bool sleep_ok = s_wireless_path && !board_usb_present();
    s_sleep_counting = false;
    if (sleep_ok) {
        uint8_t lvl = (idle_ms >= sleep_ms) ? 2u : 0u;
        if (lvl != s_sleep_lvl) {
            s_sleep_lvl = lvl;
            if (lvl >= 2u) {
                /* Deep: the module sleeps only on this command (MANUAL policy). */
                wb_cmd(WB_CMD_SLEEP, (const uint8_t[1]){WB_SLEEP_DEEP}, 1u);
            } else {
                /* Back to active. */
                wb_cmd(WB_CMD_SLEEP, (const uint8_t[1]){WB_SLEEP_WAKE}, 1u);
#ifdef RGB_MATRIX_ENABLE
                if (s_rgb_suspended) {
                    rgb_matrix_set_suspend_state(false);
                    s_rgb_suspended = false;
                }
#endif
            }
        }
        /* Once the module is in MODE_SLEEP and the board stays idle, drop the
         * SK32 itself into the board's deep sleep (ops.deep_sleep, e.g. STOP).
         * The STOP is self-recovering; a key press wakes it, then the module is
         * resynced (SLEEP WAKE) and the state is refreshed immediately. */
        if (s_sleep_lvl >= 2u) {
#ifdef RGB_MATRIX_ENABLE
            if (!s_rgb_suspended) {
                rgb_matrix_set_suspend_state(true);
                s_rgb_suspended = true;
            }
#endif
            if (wb_board_ops.deep_sleep && wb_board_ops.deep_sleep()) {
                /* A key woke the board: that is user activity, so restart the
                 * idle tracker here.  Without it the next tick still sees
                 * idle_ms past the threshold, re-enters deep sleep, and the key
                 * press never reaches the host. */
                s_last_key_ms = wb_now_ms();
                /* Key wake: resync the module and refresh the cached state so
                 * the OLED does not keep showing the stale SLEEP entry. */
                wb_cmd(WB_CMD_SLEEP, (const uint8_t[1]){WB_SLEEP_WAKE}, 1u);
                wb_delay_ms(6u);
                wb_cmd(WB_CMD_GET_STATUS, NULL, 0u);
                s_last_wl_q_ms = 0u; /* next tick re-polls the wireless state */
            }
        }
        /* Countdown visible on the OLED while awake and not yet asleep. */
        s_sleep_counting = (s_sleep_lvl < 2u);
    } else if (s_sleep_lvl != 0u) {
        /* Left the wireless path or USB came back: make sure the module is awake. */
        s_sleep_lvl = 0u;
        wb_cmd(WB_CMD_SLEEP, (const uint8_t[1]){WB_SLEEP_WAKE}, 1u);
#ifdef RGB_MATRIX_ENABLE
        if (s_rgb_suspended) {
            rgb_matrix_set_suspend_state(false);
            s_rgb_suspended = false;
        }
#endif
    }
}

/* ---------------- public getters ---------------- */

uint8_t wb_mode(void)     { return s_mode; }
bool    wb_linked(void)   { return !s_link_lost; }
uint8_t wb_led(void)      { return s_led; }
uint16_t wb_batt_mv(void) { return s_batt_mv; }
uint8_t wb_batt_pct(void) { return s_batt_pct; }
bool    wb_batt_low(void) { return s_batt_low; }
bool    wb_batt_crit(void){ return s_batt_crit; }

bool wb_usb_wireless(void) { return s_wireless_path; }

uint8_t wb_wl(void)      { return s_wl; }

bool wb_host_awake(void) { return s_host_awake; }

/* True while a recent EVT_ERROR is still shown-worthy; *code gets the WB_ECODE_*
 * value.  Returns false once the WB_ERR_SHOW_MS window has elapsed. */
bool wb_ecode(uint8_t *code) {
    if (s_ecode_ms == 0u) {
        return false;
    }
    if ((wb_now_ms() - s_ecode_ms) > WB_ERR_SHOW_MS) {
        return false;
    }
    if (code) {
        *code = s_ecode;
    }
    return true;
}

/* Last EVT_PAIR_RESULT while its display window is active: *result is 0 on
 * success, 1 on failure / pairing timeout; *profile is the slot involved. */
bool wb_pair_result(uint8_t *result, uint8_t *profile) {
    if (s_pair_ms == 0u) {
        return false;
    }
    if ((wb_now_ms() - s_pair_ms) > WB_PAIR_SHOW_MS) {
        return false;
    }
    if (result) {
        *result = s_pair_result;
    }
    if (profile) {
        *profile = s_pair_profile;
    }
    return true;
}

/* Remaining ms until the wireless idle deep-sleep threshold.  Returns false
 * when no countdown is running (wired, or already in deep sleep). */
bool wb_sleep_countdown(uint32_t *remaining_ms) {
    if (!s_sleep_counting) {
        return false;
    }
    uint32_t idle = wb_now_ms() - s_last_key_ms;
    if (remaining_ms) {
        *remaining_ms = (idle >= s_sleep_thresh_ms) ? 0u : (s_sleep_thresh_ms - idle);
    }
    return true;
}

/* ---------------- BLE multi-host (stage 2) ---------------- */

uint8_t wb_profile(void) {
    return s_profile;
}

void wb_profile_request(uint8_t profile) {
    uint8_t max = board_max_profile();
    if (profile < 1u || profile > max) {
        return;
    }
    /* Only meaningful for the BLE radio path. */
    uint8_t pl[1];
    pl[0] = profile;
    wb_cmd(WB_CMD_SET_PROFILE, pl, 1u);
    /* The bridge replies ACK then switches; the resulting EVT_STATE refreshes
     * s_profile. */
}

void wb_profile_next(void) {
    /* 1 -> 2 -> 3 -> 4 -> 1 */
    uint8_t max = board_max_profile();
    uint8_t p   = wb_profile();
    if (p < 1u || p >= max) {
        p = 0u;
    }
    wb_profile_request((uint8_t)(p + 1u));
}

void wb_profile_prev(void) {
    /* 1 -> 4 -> 3 -> 2 -> 1 */
    uint8_t max = board_max_profile();
    uint8_t p   = wb_profile();
    if (p < 2u) {
        p = max;
    }
    wb_profile_request((uint8_t)(p - 1u));
}

void wb_profile_unpair(void) {
    /* Erase all bonded slots (profile 0xFF = all). */
    uint8_t pl[1];
    pl[0] = 0xFFu;
    wb_cmd(WB_CMD_UNPAIR, pl, 1u);
}
