#include "bridge_power.h"
#include "bridge_log.h"

#define BRIDGE_MOD_PWR "pwr"

static uint8_t s_pct;
static bool    s_charger;
static bool    s_ext_valid;

void bridge_power_init(void)
{
    s_pct       = 0u;
    s_charger   = false;
    s_ext_valid = false;
    LOG_I(BRIDGE_MOD_PWR, "init (external battery source only)");
}

uint8_t bridge_power_battery_percent(void)
{
    return s_pct;
}

bool bridge_power_charger_active(void)
{
    return s_charger;
}

bool bridge_power_has_external(void)
{
    return s_ext_valid;
}

void bridge_power_set_external(uint8_t percent, bool charging)
{
    if (percent > 100u) {
        percent = 100u;
    }
    if (!s_ext_valid || s_pct != percent || s_charger != charging) {
        s_pct       = percent;
        s_charger   = charging;
        s_ext_valid = true;
        LOG_I(BRIDGE_MOD_PWR, "batt cmd pct=%u chrg=%u",
              percent, (unsigned)charging);
    }
}
