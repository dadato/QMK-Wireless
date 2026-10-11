/* Custom host_driver_t that forwards HID reports to the wireless bridge. */
#pragma once

#include <stdbool.h>

/* Switch the active host driver between USB (false) and the wireless bridge
 * (true).  Only the HID path is swapped; USB enumeration is untouched. */
void wb_host_use_wireless(bool on);

bool wb_host_is_wireless(void);
