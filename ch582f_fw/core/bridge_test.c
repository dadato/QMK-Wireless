#include "bridge_test.h"
#include "bridge_port.h"
#include "bridge_log.h"
#include "bridge_mode.h"
#include "bridge_hid.h"

#define BRIDGE_MOD_TEST "test"

static bool    s_active;
static uint8_t s_channel;

int bridge_test_set_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
#if !BRIDGE_FEATURE_SRRC_TEST
    (void)mode;
    (void)channel;
    (void)power;
    return BRIDGE_ERR_UNSUPPORTED;
#else
    if (mode == 0u) {
        if (!s_active) {
            return BRIDGE_OK;
        }
        /* 退出：关闭定频。用 BRIDGE_RADIO_NONE 明确表示「停止」——channel 0 是
         * 合法 BLE 信道，不能拿它当退出哨兵；底层据此结束 LL 测试并归还共享射频 */
        if (port_radio_test_mode(BRIDGE_RADIO_NONE, 0u, 0u) != BRIDGE_OK) {
            LOG_E(BRIDGE_MOD_TEST, "exit test mode failed");
            return BRIDGE_ERR_IO;
        }
        s_active = false;
        LOG_W(BRIDGE_MOD_TEST, "SRRC test mode off");
        return BRIDGE_OK;
    }

    if (mode != 1u) {
        return BRIDGE_ERR_PARAM;
    }
    if (channel > BRIDGE_TEST_CH_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    if (power > BRIDGE_TEST_POWER_MAX) {
        return BRIDGE_ERR_PARAM;
    }

    /* 定频发射期间不得与正常转发并存 */
    (void)bridge_hid_release_all();
    if (port_radio_test_mode(BRIDGE_RADIO_BLE, channel, power) != BRIDGE_OK) {
        LOG_E(BRIDGE_MOD_TEST, "enter test mode failed ch=%u", channel);
        return BRIDGE_ERR_IO;
    }

    s_active  = true;
    s_channel = channel;
    LOG_W(BRIDGE_MOD_TEST, "SRRC test mode on ch=%u pwr=%u", channel, power);
    return BRIDGE_OK;
#endif
}

bool bridge_test_is_active(void)
{
    return s_active;
}

uint8_t bridge_test_channel(void)
{
    return s_channel;
}
