/* 电源与电量（规格 9.4）
 *
 * 周期 ADC 采样 → 电压 → 电量百分比 → 阈值告警（20% 一次，5% 重复）
 */
#ifndef BRIDGE_POWER_H
#define BRIDGE_POWER_H

#include "bridge_config.h"
#include "bridge_types.h"

#define BRIDGE_BATT_ALARM_LOW_PCT   20u
#define BRIDGE_BATT_ALARM_CRIT_PCT   5u
#define BRIDGE_BATT_SAMPLE_MS      10000u

void     bridge_power_init(void);
void     bridge_power_poll(void);         /* 周期采样 + 告警 */

uint16_t bridge_power_battery_mv(void);
uint8_t  bridge_power_battery_percent(void);
uint8_t  bridge_power_alarm_flags(void);

bool     bridge_power_charger_active(void);
void     bridge_power_note_charger(bool active);

/* 主控下发电量（借鉴市售模组 0x87「下发电量信息」）：
 * SK32 自算电量优先于本地 ADC 换算，取值为 0..100（>100 截断）。 */
void     bridge_power_set_external(uint8_t percent);
bool     bridge_power_has_external(void);

/* 电压 → 电量百分比（锂电曲线，可标定） */
uint8_t  bridge_power_mv_to_percent(uint16_t mv);

/* 强制采样（测试 / 主动查询） */
void     bridge_power_sample(void);

#define BRIDGE_BATT_ALARM_LOW   0x01u
#define BRIDGE_BATT_ALARM_CRIT  0x02u

#endif /* BRIDGE_POWER_H */
