/* Wireless bridge client - top-level public API (cross-keyboard reusable).
 *
 * "Zero-argument, like the QMK OLED driver": a keyboard pulls this in, defines
 * a global const wb_board_config_t and const wb_board_ops_t (pins, identity,
 * behaviour), then calls only
 *
 *   wb_init();   (in keyboard_post_init_kb())
 *   wb_task();   (in housekeeping_task_kb())
 *
 * and reads any getter / issues any control below.  Optional WB_* keycodes
 * come from wb_keycodes.h when the keyboard opts into them.
 */
#pragma once

#include "wb_proto.h"
#include "wb_board.h"
#include "wb_session.h"

/* ---- init / periodic / inbound frame ---- */
void wb_init(void);
void wb_task(void);
void wb_on_frame(const wb_frame_t *f);  /* internal, rarely called directly */

/* ---- getters (cached module state) ---- */
uint8_t  wb_mode(void);            /* WB_MODE_* */
bool     wb_linked(void);
uint8_t  wb_wl(void);              /* WB_WL_* */
uint8_t  wb_led(void);
uint16_t wb_batt_mv(void);
uint8_t  wb_batt_pct(void);
bool     wb_batt_low(void);
bool     wb_batt_crit(void);
bool     wb_host_awake(void);
bool     wb_usb_wireless(void);
uint8_t  wb_profile(void);
bool     wb_ecode(uint8_t *code);
bool     wb_pair_result(uint8_t *result, uint8_t *profile);
bool     wb_sleep_countdown(uint32_t *remaining_ms);

/* ---- controls ---- */
void wb_mode_request(uint8_t mode);
void wb_profile_request(uint8_t profile);
void wb_profile_next(void);
void wb_profile_prev(void);
void wb_profile_unpair(void);
void wb_pair(void);
