/* 电源与电量（规格 9.4）
 *
 * 本板不接电池 ADC 与充电检测线：电量百分比与充电状态**只由
 * CMD_SET_BATTERY(0x1C) 命令输入**（SK32 侧采样后下发），模组不做本地采样，
 * 也不再向上回报 EVT_BATTERY。
 */
#ifndef BRIDGE_POWER_H
#define BRIDGE_POWER_H

#include "bridge_config.h"
#include "bridge_types.h"

void     bridge_power_init(void);

uint8_t  bridge_power_battery_percent(void);
bool     bridge_power_charger_active(void);

/* 主控下发电量（借鉴市售模组 0x87「下发电量信息」）：
 * 取值为 0..100（>100 截断）；charging 为该次上报的充电状态。 */
void     bridge_power_set_external(uint8_t percent, bool charging);
bool     bridge_power_has_external(void);

#endif /* BRIDGE_POWER_H */
