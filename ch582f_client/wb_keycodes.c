/* Custom keycodes for the wireless bridge client (optional layer). */
#include "wb_keycodes.h"
#include "wb_proto.h"
#include "wb_session.h"

#include "quantum.h"

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        switch (keycode) {
            case WB_MODE_NEXT: {
                /* cycle USB -> BLE -> 2G4 -> USB ... */
                uint8_t m = wb_mode();
                uint8_t next;
                switch (m) {
                    case WB_MODE_BLE:
                    case WB_MODE_BLE_PAIR:
                        next = WB_MODE_2G4;
                        break;
                    case WB_MODE_2G4:
                    case WB_MODE_2G4_PAIR:
                        next = WB_MODE_USB;
                        break;
                    default: /* USB */
                        next = WB_MODE_BLE;
                        break;
                }
                wb_mode_request(next);
                return false;
            }
            case WB_KC_USB:
                wb_mode_request(WB_MODE_USB);
                return false;
            case WB_KC_BLE:
                wb_mode_request(WB_MODE_BLE);
                return false;
            case WB_KC_2G4:
                wb_mode_request(WB_MODE_2G4);
                return false;
            case WB_PAIR:
                wb_pair();
                return false;
            case WB_UNPAIR: {
                /* Unpair the active profile slot: protocol wants [profile]
                 * (1..4), 0xFF = all.  Unknown slot (no EVT_STATE yet) falls
                 * back to slot 1 rather than accidentally clearing everything. */
                uint8_t p = wb_profile();
                uint8_t pl[1];
                pl[0] = (p >= 1u && p <= 4u) ? p : 1u;
                wb_cmd(WB_CMD_UNPAIR, pl, 1u);
                return false;
            }
            case WB_RESET:
                wb_cmd(WB_CMD_RESET, NULL, 0u);
                return false;
            /* BLE multi-host (stage 2): host slot switching. */
            case WB_HOST_NEXT:
                wb_profile_next();
                return false;
            case WB_HOST_PREV:
                wb_profile_prev();
                return false;
            case WB_PROFILE_1:
                wb_profile_request(1u);
                return false;
            case WB_PROFILE_2:
                wb_profile_request(2u);
                return false;
            case WB_PROFILE_3:
                wb_profile_request(3u);
                return false;
            case WB_PROFILE_4:
                wb_profile_request(4u);
                return false;
            case WB_UNPAIR_ALL:
                wb_profile_unpair();
                return false;
            default:
                break;
        }
    }
    return process_record_user(keycode, record);
}
