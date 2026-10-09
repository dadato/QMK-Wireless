#include "bridge_power.h"
#include "bridge_port.h"
#include "bridge_log.h"
#include "bridge_proto.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"

#define BRIDGE_MOD_PWR "pwr"

static uint16_t s_mv;
static uint8_t  s_pct;
static uint8_t  s_alarm;
static bool     s_charger;
static uint32_t s_last_sample_ms;
static uint8_t  s_ext_pct;      /* 主控下发的电量（0x87） */
static bool     s_ext_valid;

void bridge_power_init(void)
{
    s_mv             = 0u;
    s_pct            = 0u;
    s_alarm          = 0u;
    s_charger        = false;
    s_ext_pct        = 0u;
    s_ext_valid      = false;
    s_last_sample_ms = port_millis();

    bridge_power_sample();
    LOG_I(BRIDGE_MOD_PWR, "init mv=%u pct=%u chrg=%u",
          s_mv, s_pct, (unsigned)s_charger);
}

uint8_t bridge_power_mv_to_percent(uint16_t mv)
{
    /* 单节锂电近似曲线（空载），投板后按实测标定 */
    static const struct { uint16_t mv; uint8_t pct; } k_curve[] = {
        { 4200u, 100u }, { 4060u, 90u }, { 3980u, 80u }, { 3920u, 70u },
        { 3870u, 60u },  { 3830u, 50u }, { 3790u, 40u }, { 3750u, 30u },
        { 3700u, 20u },  { 3650u, 10u }, { 3400u, 5u },  { 3000u, 0u }
    };
    const uint8_t n = (uint8_t)(sizeof(k_curve) / sizeof(k_curve[0]));

    if (mv >= k_curve[0].mv) {
        return 100u;
    }
    for (uint8_t i = 0u; i < (uint8_t)(n - 1u); i++) {
        if (mv >= k_curve[i + 1u].mv) {
            uint16_t hi = k_curve[i].mv;
            uint16_t lo = k_curve[i + 1u].mv;
            uint8_t  ph = k_curve[i].pct;
            uint8_t  pl = k_curve[i + 1u].pct;
            /* 线性插值 */
            uint32_t num = (uint32_t)(mv - lo) * (uint32_t)(ph - pl);
            return (uint8_t)(pl + (uint8_t)(num / (uint32_t)(hi - lo)));
        }
    }
    return 0u;
}

void bridge_power_sample(void)
{
    uint8_t src = bridge_cfg_get()->battery_src;

    /* 电量来源选择（SET_CONFIG key 0x05）：
     *   LOCAL    —— 仅本地 ADC；
     *   EXTERNAL —— 仅 SK32 上传（未收到时保持上次值），且不必读本地 ADC；
     *   AUTO     —— 外部有效则用外部，否则本地 ADC（默认，兼容旧行为）。 */
    if (src != BRIDGE_BATT_SRC_EXTERNAL) {
        s_mv = port_bat_read_mv();
    }

    bool use_ext = (src != BRIDGE_BATT_SRC_LOCAL) && s_ext_valid;
    s_pct = use_ext ? s_ext_pct : bridge_power_mv_to_percent(s_mv);
    s_charger = port_charger_active();
    s_last_sample_ms = port_millis();

    uint8_t prev = s_alarm;
    s_alarm = 0u;
    if (s_pct <= BRIDGE_BATT_ALARM_CRIT_PCT) {
        s_alarm |= BRIDGE_BATT_ALARM_CRIT;
    } else if (s_pct <= BRIDGE_BATT_ALARM_LOW_PCT) {
        s_alarm |= BRIDGE_BATT_ALARM_LOW;
    }

    if (s_alarm != prev) {
        LOG_W(BRIDGE_MOD_PWR, "batt alarm 0x%02X mv=%u pct=%u",
              s_alarm, s_mv, s_pct);
    } else {
        LOG_D(BRIDGE_MOD_PWR, "batt mv=%u pct=%u", s_mv, s_pct);
    }
}

void bridge_power_poll(void)
{
    uint32_t period = BRIDGE_BATT_SAMPLE_MS;
    if (bridge_mode_get() == MODE_SLEEP) {
        period *= 4u;   /* 休眠降频（规格 9.4） */
    }
    if ((uint32_t)(port_millis() - s_last_sample_ms) >= period) {
        bridge_power_sample();
        bridge_evt_battery(s_mv, s_pct);
    }
}

uint16_t bridge_power_battery_mv(void)
{
    return s_mv;
}

uint8_t bridge_power_battery_percent(void)
{
    return s_pct;
}

uint8_t bridge_power_alarm_flags(void)
{
    return s_alarm;
}

bool bridge_power_charger_active(void)
{
    return s_charger;
}

void bridge_power_note_charger(bool active)
{
    if (s_charger != active) {
        s_charger = active;
        LOG_I(BRIDGE_MOD_PWR, "charger %s", active ? "on" : "off");
    }
}

void bridge_power_set_external(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }
    if (!s_ext_valid || s_ext_pct != percent) {
        s_ext_pct   = percent;
        s_ext_valid = true;
        s_pct       = percent;
        LOG_I(BRIDGE_MOD_PWR, "batt ext pct=%u", percent);
    }
}

bool bridge_power_has_external(void)
{
    return s_ext_valid;
}
