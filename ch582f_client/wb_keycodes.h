/* Custom keycodes for the wireless bridge client (optional layer). */
#pragma once

#include "quantum_keycodes.h"

/* The keycode names use a WB_KC_ prefix only where the bare name would collide
 * with a wire-mode macro in wb_proto.h.  The WB_MODE_* values in wb_proto.h
 * are plain #define constants; the values below are distinct QMK keycode
 * enum entries. */
enum wb_keycodes {
    WB_MODE_NEXT = SAFE_RANGE, /* cycle USB -> BLE -> 2.4G -> USB */
    WB_KC_USB,                 /* switch to wired USB mode */
    WB_KC_BLE,                 /* switch to BLE radio */
    WB_KC_2G4,                 /* switch to 2.4G radio */
    WB_PAIR,                   /* enter pairing for the active radio */
    WB_UNPAIR,                 /* unpair the active profile */
    WB_RESET,                  /* reset the wireless module */
    /* BLE multi-host (stage 2): switch the active host slot. */
    WB_HOST_NEXT,              /* BLE host slot -> next */
    WB_HOST_PREV,              /* BLE host slot -> previous */
    WB_PROFILE_1,              /* BLE host slot 1 (explicit) */
    WB_PROFILE_2,              /* BLE host slot 2 (explicit) */
    WB_PROFILE_3,              /* BLE host slot 3 (explicit) */
    WB_PROFILE_4,              /* BLE host slot 4 (explicit) */
    WB_UNPAIR_ALL,             /* erase all BLE bond slots */
};
