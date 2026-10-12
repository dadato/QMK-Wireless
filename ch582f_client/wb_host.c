/* Custom host_driver_t that forwards HID reports to the wireless bridge. */
#include <string.h>

#include "wb_host.h"
#include "wb_proto.h"
#include "wb_session.h"

#include "action.h"   /* clear_keyboard() */
#include "host.h"
#include "quantum/keycode_config.h"  /* keymap_config_t */
#include "report.h"

/* keymap_config (the global runtime settings incl. NKRO flag) is defined and
 * extern-declared elsewhere in QMK; pull in the variable here. */
extern keymap_config_t keymap_config;

static bool s_wireless = false;

/* cached last LED state from EVT_LEDS (bridge reports it) */
static uint8_t s_led_cache = 0u;

/* saved original host driver (the QMK USB one) so we can restore it and the
 * prior NKRO flag when returning to the wired path. */
static host_driver_t *s_saved_driver = NULL;
static bool           s_saved_nkro   = false;

/* ------------------------------------------------------------------ */
/* HID payload mapping to bridge frames (see bridge_types.h)           */

static void wb_kbd(report_keyboard_t *report) {
    /* report_keyboard_t is [mods][reserved][keys6] == bridge 8-byte layout. */
    wb_tx_hid(WB_CMD_HID_KEYBOARD, (const uint8_t *)report, WB_REPORT_KBD_LEN);
}

static void wb_nkro(report_nkro_t *report) {
    /* report_nkro_t is [report_id][mods][bits16]; bridge wants [mods][bits15]
     * = 16 bytes.  Drop report_id, keep mods + 15 bitmap bytes. */
    uint8_t pl[WB_REPORT_NKRO_LEN];
    pl[0] = report->mods;
    memcpy(&pl[1], report->bits, WB_REPORT_NKRO_LEN - 1u);
    wb_tx_hid(WB_CMD_HID_NKRO, pl, WB_REPORT_NKRO_LEN);
}

static void wb_extra(report_extra_t *report) {
    uint8_t pl[2u];
    if (report->report_id == REPORT_ID_CONSUMER) {
        pl[0] = (uint8_t)(report->usage & 0xFFu);
        pl[1] = (uint8_t)(report->usage >> 8);
        wb_tx_hid(WB_CMD_HID_CONSUMER, pl, WB_REPORT_CONSUMER_LEN);
    } else if (report->report_id == REPORT_ID_SYSTEM) {
        pl[0] = (uint8_t)(report->usage & 0xFFu);
        wb_tx_hid(WB_CMD_HID_SYSTEM, pl, WB_REPORT_SYSTEM_LEN);
    }
}

static void wb_mouse(report_mouse_t *report) {
    uint8_t pl[WB_REPORT_MOUSE_LEN];
    pl[0] = report->buttons;
    pl[1] = (uint8_t)(report->x & 0xFFu);
    pl[2] = (uint8_t)(report->y & 0xFFu);
    pl[3] = (uint8_t)(report->v & 0xFFu);
    wb_tx_hid(WB_CMD_HID_MOUSE, pl, WB_REPORT_MOUSE_LEN);
}

static uint8_t wb_leds(void) {
    return s_led_cache;
}

#ifdef RAW_ENABLE
static void wb_raw(uint8_t *data, uint8_t len) {
    /* Bridge tunnel for VIA / raw HID.  Cap to the tunnel max length. */
    if (len > WB_TUNNEL_MAX_LEN) {
        len = WB_TUNNEL_MAX_LEN;
    }
    /* Route by the active wireless mode so the far end matches the host link:
     *   2.4G / wired  -> WB_CMD_TUNNEL_USB, terminating at the Dongle's USB
     *                    raw HID endpoint.
     *   BLE (incl. the pairing window) -> WB_CMD_TUNNEL_BLE, which the module
     *                    emits as its HOGP vendor report (RID 0 implicit, QMK Raw HID)
     *                    so usevia.app sees the reply over WebHID. */
    uint8_t mode = wb_mode();
    if (mode == WB_MODE_BLE || mode == WB_MODE_BLE_PAIR) {
        wb_tx_hid(WB_CMD_TUNNEL_BLE, data, len);
    } else {
        wb_tx_hid(WB_CMD_TUNNEL_USB, data, len);
    }
}
#endif

static host_driver_t s_wb_driver = {
    .keyboard_leds = wb_leds,
    .send_keyboard = wb_kbd,
    .send_nkro     = wb_nkro,
    .send_mouse    = wb_mouse,
    .send_extra    = wb_extra,
#ifdef RAW_ENABLE
    .send_raw_hid  = wb_raw,
#endif
};

/* ------------------------------------------------------------------ */

void wb_host_use_wireless(bool on) {
    if (s_wireless == on) {
        return;
    }
    if (on) {
        /* Save the wired (USB) driver and NKRO flag, then install ours. */
        s_saved_driver = host_get_driver();
        s_saved_nkro   = keymap_config.nkro;
        keymap_config.nkro = false; /* bridge sends 6KRO over BLE path */
        s_wireless = true;
        host_set_driver(&s_wb_driver);
    } else {
        s_wireless = false;
        /* Restore the original driver (and its NKRO preference). */
        host_set_driver(s_saved_driver);
        keymap_config.nkro = s_saved_nkro;
    }
    clear_keyboard();
}

bool wb_host_is_wireless(void) {
    return s_wireless;
}
