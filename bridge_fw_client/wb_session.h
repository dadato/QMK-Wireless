/* Wireless bridge client — L2 session engine.
 *
 * Portable core that owns: boot bring-up, the periodic housekeeping task (RX
 * drain, module recovery, USB/wireless switch, idle sleep), and mode / EVT /
 * pairing / battery / tunnel(VIA) handling.  It does NOT touch OLED, deep
 * sleep or other board hardware directly — those go through the board ops in
 * wb_board.h, and the pins / timing come from the board config.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "wb_proto.h"
#include "wb_board.h"

/* Init the bridge client.  Call from keyboard_post_init_kb(). */
void wb_init(void);

/* Main periodic driver.  Call from housekeeping_task_kb() every loop. */
void wb_task(void);

/* Send a command frame via the transport (throttled by the inter-frame gap). */
void wb_cmd(uint8_t cmd, const uint8_t payload[], uint8_t len);

/* Send a HID / control payload immediately, respecting the frame gap.
 * Returns the number of bytes written (0 if the gap throttled it). */
uint16_t wb_tx_hid(uint8_t cmd, const uint8_t payload[], uint8_t len);

/* Handle a decoded frame that came back from the bridge. */
void wb_on_frame(const wb_frame_t *f);

/* ---- getters (cache of the last EVT_STATE / EVT_BATTERY, etc.) ---- */
uint8_t  wb_mode(void);            /* WB_MODE_* */
bool     wb_linked(void);
uint8_t  wb_wl(void);              /* WB_WL_*  */
uint8_t  wb_led(void);
uint16_t wb_batt_mv(void);
uint8_t  wb_batt_pct(void);
bool     wb_batt_charging(void);
bool     wb_batt_low(void);
bool     wb_batt_crit(void);
bool     wb_host_awake(void);
bool     wb_usb_wireless(void);
uint8_t  wb_profile(void);         /* active BLE host slot 1..4 */
bool     wb_ecode(uint8_t *code);          /* WB_ECODE_* while the window is live */
bool     wb_pair_result(uint8_t *result, uint8_t *profile);
bool     wb_sleep_countdown(uint32_t *remaining_ms);

/* ---- controls ---- */
void wb_mode_request(uint8_t mode);
void wb_profile_request(uint8_t profile);
void wb_profile_next(void);
void wb_profile_prev(void);
void wb_profile_unpair(void);
void wb_pair(void);                /* enter pairing for the active radio */
