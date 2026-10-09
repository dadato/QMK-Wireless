/* SRRC 认证定频测试模式（D9 / 规格 8.5）
 *
 * 由 SET_TEST_MODE(0x0B) 进入：mode=1 定频连续发射，mode=0 退出。
 * 功率档固定，不得超出申报值；2.4G 侧能力取决于厂商用例是否暴露接口。
 */
#ifndef BRIDGE_TEST_H
#define BRIDGE_TEST_H

#include "bridge_config.h"
#include "bridge_types.h"

#define BRIDGE_TEST_CH_MIN     0u
#define BRIDGE_TEST_CH_MAX     39u   /* BLE 信道 0..39 */
#define BRIDGE_TEST_POWER_MAX  3u    /* 功率档，0=最低 */

/* mode=1 进入定频，mode=0 退出；返回 BRIDGE_OK / BRIDGE_ERR_* */
int  bridge_test_set_mode(uint8_t mode, uint8_t channel, uint8_t power);
bool bridge_test_is_active(void);
uint8_t bridge_test_channel(void);

#endif /* BRIDGE_TEST_H */
