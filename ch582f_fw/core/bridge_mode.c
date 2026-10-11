#include "bridge_mode.h"
#include "bridge_hid.h"
#include "bridge_cfg.h"
#include "bridge_port.h"
#include "bridge_log.h"
#include "bridge_proto.h"
#include "bridge_test.h"
#include "bridge_uart.h"

#define BRIDGE_MOD_MODE "mode"

static bridge_mode_t s_mode;
static bool          s_vbus;
static bool          s_connected;
static uint32_t      s_last_activity_ms;
static uint8_t       s_gpio_sel;
static bool          s_radio_up;
static bool          s_sleep_permit_cmd;   /* MANUAL 策略下由 CMD_SLEEP(IDLE/DEEP) 置位 */
static bool          s_host_awake = true;  /* 主机电源状态：true=运行/false=挂起 */
static uint32_t      s_pair_started_ms;    /* 配对窗口起始时刻（超时判定用） */
static uint16_t      s_pair_timeout_s;     /* 配对窗口持续时间（秒），0=不自动关闭 */

const char *bridge_mode_name(bridge_mode_t mode)
{
    switch (mode) {
        case MODE_USB:      return "USB";
        case MODE_BLE:      return "BLE";
        case MODE_BLE_PAIR: return "BLE_PAIR";
        case MODE_2G4:      return "2G4";
        case MODE_2G4_PAIR: return "2G4_PAIR";
        case MODE_OTA:      return "OTA";
        case MODE_SLEEP:    return "SLEEP";
        default:            return "?";
    }
}

uint8_t bridge_mode_default(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    return (uint8_t)MODE_2G4;       /* Dongle 无 BLE：出厂默认 2.4G 接收 */
#else
    return (uint8_t)MODE_BLE;
#endif
}

uint8_t bridge_mode_restore_target(uint8_t remembered)
{
    if (remembered == (uint8_t)MODE_BLE_PAIR) {
        return (uint8_t)MODE_BLE;
    }
    if (remembered == (uint8_t)MODE_2G4_PAIR) {
        return (uint8_t)MODE_2G4;
    }
    return remembered;
}

static uint8_t radio_of(bridge_mode_t mode)
{
    switch (mode) {
        case MODE_BLE:
        case MODE_BLE_PAIR:
            return BRIDGE_RADIO_BLE;
        case MODE_2G4:
        case MODE_2G4_PAIR:
            return BRIDGE_RADIO_2G4;
        default:
            return BRIDGE_RADIO_NONE;
    }
}

static bool mode_is_paired_window(bridge_mode_t mode)
{
    return (mode == MODE_BLE_PAIR || mode == MODE_2G4_PAIR);
}

/* 进入/刷新配对窗口时重置超时计时起点（见 bridge_mode_poll 的超时判定） */
static void pair_window_stamp(void)
{
    s_pair_started_ms = port_millis();
}

/* 关闭当前射频（幂等） */
static void radio_down(void)
{
    if (!s_radio_up) {
        return;
    }
    uint8_t r = radio_of(s_mode);
    if (r != BRIDGE_RADIO_NONE) {
        (void)port_radio_deinit(r);
    }
    s_radio_up = false;
    s_connected = false;
    bridge_hid_note_link(false);
}

/* 6 步切换事务（规格 5.3 + 6.3）：
 *   1. HID_RELEASE_ALL 防卡键
 *   2. 断开当前射频
 *   3. 初始化目标射频(或保持关闭)
 *   4. 更新模式并落盘(按需)
 *   5. 按连接状态驱动 LINK / WAKE 引脚
 *   6. 上报 EVT_STATE
 */
static int mode_transaction(bridge_mode_t target, bool persist)
{
    uint8_t radio;

    bridge_hid_release_all();          /* 1 */
    radio_down();                      /* 2 */

    radio = radio_of(target);
    if (radio != BRIDGE_RADIO_NONE) {  /* 3 */
        if (port_radio_init(radio) != BRIDGE_OK) {
            LOG_E(BRIDGE_MOD_MODE, "radio_init(%u) failed", radio);
            return BRIDGE_ERR_IO;
        }
        s_radio_up = true;
        /* 广播/可配对只在显式配对窗口存在：常规无线模式（BLE/2.4G）永不广播，
         * 使已配对设备掉线（主机关闭蓝牙 / Dongle 关闭）后模块不再回到可配对状态。
         * BLE 常规模式所用槽地址已由 GAPROLE_STARTED(sdk_ble_gap_ready) 应用，
         * 无需再借 connect_profile 重开广播。 */
        if (mode_is_paired_window(target)) {
            (void)port_radio_pair_start(radio);
        }
    }

    s_mode = target;                   /* 4 */
    if (mode_is_paired_window(target)) {
        pair_window_stamp();
    }
    if (persist && target != MODE_SLEEP && target != MODE_OTA) {
        (void)bridge_cfg_set_mode((uint8_t)target);
    }

    port_gpio_set_link(s_connected);   /* 5 */
    port_gpio_set_wake(true);

    bridge_evt_state();                /* 6 */

    LOG_I(BRIDGE_MOD_MODE, "mode -> %s", bridge_mode_name(target));
    return BRIDGE_OK;
}

uint8_t bridge_mode_from_sel(uint8_t sel)
{
    switch (sel & 0x03u) {
        case 0u:  return (uint8_t)MODE_USB;
        case 1u:  return (uint8_t)MODE_BLE;
        case 2u:  return (uint8_t)MODE_2G4;
        default: return (uint8_t)MODE_BLE_PAIR;   /* 11 = 保留 → 配对窗口 */
    }
}

void bridge_mode_init(void)
{
    const bridge_cfg_t *cfg = bridge_cfg_get();
    (void)cfg;   /* 仅 LOG_I 使用；LOG=0 时展开为空，避免 -Wunused-variable */

    s_last_activity_ms = port_millis();
    s_pair_started_ms  = port_millis();
    s_pair_timeout_s   = BRIDGE_PAIR_TIMEOUT_S;
    s_radio_up         = false;
    s_connected        = false;
    s_vbus             = port_vbus_present();
    s_gpio_sel         = port_gpio_read_mode_sel();

    /* 优先级：VBUS > GPIO SEL。SEL=00（含下拉悬空）即 USB 默认模式，
     * 01/10/11 分别为 BLE / 2.4G / BLE_PAIR。 */
    bridge_mode_t boot_mode;
    if (s_vbus) {
        boot_mode = MODE_USB;
    } else {
        boot_mode = (bridge_mode_t)bridge_mode_from_sel(s_gpio_sel);
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 BLE：旧固件 / 默认配置若记忆为 BLE，上电强制回 2.4G */
    if (boot_mode == MODE_BLE || boot_mode == MODE_BLE_PAIR ||
        boot_mode == MODE_USB) {
        boot_mode = MODE_2G4;
    }
#endif

    s_mode = boot_mode;
    LOG_I(BRIDGE_MOD_MODE, "init vbus=%u sel=%u cfg_mode=%u -> %s",
          (unsigned)s_vbus, s_gpio_sel, cfg->last_mode,
          bridge_mode_name(boot_mode));

    (void)mode_transaction(boot_mode, false);
    s_last_activity_ms = port_millis();
}

bridge_mode_t bridge_mode_get(void)
{
    return s_mode;
}

uint8_t bridge_mode_radio_id(void)
{
    if (bridge_test_is_active()) {
        return BRIDGE_RADIO_NONE;   /* 定频模式下不转发 */
    }
    if (!s_radio_up || !s_connected) {
        return BRIDGE_RADIO_NONE;
    }
    return radio_of(s_mode);
}

bool bridge_mode_is_connected(void)
{
    return s_connected && s_radio_up;
}

uint8_t bridge_mode_wireless_state(void)
{
    bool connected = bridge_mode_is_connected();

    switch (s_mode) {
        case MODE_USB:      return BRIDGE_WL_USB;
        case MODE_OTA:      return BRIDGE_WL_OTA;
        case MODE_SLEEP:    return BRIDGE_WL_SLEEP;
        case MODE_BLE_PAIR: return BRIDGE_WL_BLE_PAIRING;
        case MODE_2G4_PAIR: return BRIDGE_WL_2G4_PAIRING;
        case MODE_BLE:
            return connected ? BRIDGE_WL_BLE_LINKED : BRIDGE_WL_BLE_DISCONN;
        case MODE_2G4:
            return connected ? BRIDGE_WL_2G4_LINKED : BRIDGE_WL_2G4_DISCONN;
        default:            return BRIDGE_WL_IDLE;
    }
}

bool bridge_mode_is_wireless(void)
{
    return (s_mode == MODE_BLE || s_mode == MODE_2G4 ||
            s_mode == MODE_BLE_PAIR || s_mode == MODE_2G4_PAIR);
}

int bridge_mode_request(uint8_t mode, bool persist)
{
    if (mode > (uint8_t)MODE_SLEEP) {
        LOG_W(BRIDGE_MOD_MODE, "bad mode %u", mode);
        return BRIDGE_ERR_PARAM;
    }

    /* VBUS 在场时只允许 USB（规格 5.2） */
    if (s_vbus && mode != (uint8_t)MODE_USB) {
        LOG_W(BRIDGE_MOD_MODE, "vbus present, refuse mode %u", mode);
        return BRIDGE_ERR_VBUS;
    }
    if (!s_vbus && mode == (uint8_t)MODE_USB) {
        /* VBUS 不在却请求 USB：允许（SK32 可能未同步），但不落盘 */
        persist = false;
    }

    if ((uint8_t)s_mode == mode) {
        if (mode_is_paired_window(s_mode)) {
            /* 重复配对请求：刷新配对窗口（含超时计时起点） */
            uint8_t r = radio_of(s_mode);
            if (r != BRIDGE_RADIO_NONE) {
                (void)port_radio_pair_start(r);
            }
            pair_window_stamp();
        }
        return BRIDGE_OK;
    }

    bridge_mode_note_activity();
    return mode_transaction((bridge_mode_t)mode, persist);
}

void bridge_mode_set_vbus(bool present)
{
    if (s_vbus == present) {
        return;
    }
    s_vbus = present;
    LOG_I(BRIDGE_MOD_MODE, "vbus %s", present ? "in" : "out");

    if (present) {
        if (s_mode != MODE_USB) {
            (void)mode_transaction(MODE_USB, false);
        }
    } else {
        /* 拔出后恢复记忆模式（规格 5.2） */
        bridge_mode_t m = (bridge_mode_t)bridge_mode_restore_target(
            bridge_cfg_get()->last_mode);
        if (m == MODE_SLEEP || m == MODE_OTA || m == MODE_USB) {
            m = (bridge_mode_t)bridge_mode_default();
        }
        (void)mode_transaction(m, false);
    }
}

bool bridge_mode_vbus_present(void)
{
    return s_vbus;
}

void bridge_mode_on_radio_event(uint8_t evt)
{
    switch (evt) {
        case BRIDGE_RADIO_EVT_CONNECTED:
            s_connected = true;
            bridge_hid_note_link(true);
            /* 配对窗口 → 连接态，必须同步落盘：否则 last_mode 停在 *_PAIR，
             * 深睡唤醒后会恢复到配对窗口（只广播、不 connect_profile），
             * 表现为"唤醒后连不上、需手动切模式再切回"（真机实测）。 */
            if (s_mode == MODE_BLE_PAIR) {
                s_mode = MODE_BLE;
                (void)bridge_cfg_set_mode((uint8_t)MODE_BLE);
            } else if (s_mode == MODE_2G4_PAIR) {
                s_mode = MODE_2G4;
                (void)bridge_cfg_set_mode((uint8_t)MODE_2G4);
            }
            /* 已连接即撤下广播/配对意图：否则链路随后中断时协议栈会在 GAPROLE_WAITING
             * 自动重开广播，令已配对设备掉线瞬间又进入可配对状态。 */
            (void)port_radio_pair_stop(radio_of(s_mode));
            break;

        case BRIDGE_RADIO_EVT_DISCONNECTED:
            bridge_hid_release_all();
            s_connected = false;
            bridge_hid_note_link(false);
            /* 已配对后掉线（主机关闭蓝牙 / Dongle 关闭等）：保持当前常规模式、射频静默，
             * 绝不重开广播或重入配对窗口——广播/可配对只在显式配对窗口存在（PAIR_START）。
             * 2.4G 由心跳/link_id 校验在对端恢复时自动重连；BLE 需重新 PAIR_START 重连。 */
            if (!mode_is_paired_window(s_mode)) {
                uint8_t r = radio_of(s_mode);
                if (r != BRIDGE_RADIO_NONE) {
                    (void)port_radio_pair_stop(r);
                }
            }
            break;

        case BRIDGE_RADIO_EVT_PAIRED:
            s_connected = true;
            bridge_hid_note_link(true);
            /* 同 CONNECTED：配对成功即脱离配对窗口并落盘，避免唤醒回到 *_PAIR。 */
            if (s_mode == MODE_BLE_PAIR) {
                s_mode = MODE_BLE;
                (void)bridge_cfg_set_mode((uint8_t)MODE_BLE);
            } else if (s_mode == MODE_2G4_PAIR) {
                s_mode = MODE_2G4;
                (void)bridge_cfg_set_mode((uint8_t)MODE_2G4);
            }
            bridge_evt_pair_result(0u, bridge_cfg_get()->ble_profile);
            break;

        case BRIDGE_RADIO_EVT_PAIR_FAILED:
            bridge_evt_pair_result(1u, bridge_cfg_get()->ble_profile);
            bridge_evt_error(BRIDGE_ECODE_PAIR);
            break;

        default:
            return;
    }

    port_gpio_set_link(s_connected);
    bridge_evt_state();
    bridge_mode_note_activity();
}

void bridge_mode_note_activity(void)
{
    s_last_activity_ms = port_millis();
}

uint32_t bridge_mode_idle_ms(void)
{
    return (uint32_t)(port_millis() - s_last_activity_ms);
}

bool bridge_mode_sleep_permitted(void)
{
    /* 主机挂起即「PC 对模组的休眠控制」：无论策略如何都许可休眠，
     * 使模组可浅睡（保持链路）等待主机恢复。 */
    if (!s_host_awake) {
        return true;
    }
    if (bridge_cfg_get()->sleep_policy == BRIDGE_SLEEP_POLICY_AUTO) {
        return true;        /* 自动策略：模块自行判闲休眠 */
    }
    return s_sleep_permit_cmd;
}

void bridge_mode_sleep_permit(bool en)
{
    s_sleep_permit_cmd = en;
}

bool bridge_mode_shallow_permitted(void)
{
    /* 主机挂起：许可浅睡（保持链路等待恢复），与 sleep_permitted 一致 */
    if (!s_host_awake) {
        return true;
    }
    if (bridge_cfg_get()->sleep_policy != BRIDGE_SLEEP_POLICY_AUTO) {
        return s_sleep_permit_cmd;      /* MANUAL：仅指令许可 */
    }
    /* AUTO：需距最近一次串口接收活动达 sleep_idle_s 秒；0 = 禁用自动浅睡。
     * 判据用 bridge_uart_rx_gap_ms()（仅由 UART 接收刷新），不可用 idle_ms
     * （BLE 连接事件会持续刷新）。port 层另有 BRIDGE_RX_HOLD_AWAKE_MS 硬下限，
     * 故有效门限 = max(BRIDGE_RX_HOLD_AWAKE_MS, sleep_idle_s*1000)。 */
    uint16_t idle_s = bridge_cfg_get()->sleep_idle_s;
    if (idle_s == 0u) {
        return false;
    }
    return bridge_uart_rx_gap_ms() >= (uint32_t)idle_s * 1000u;
}

void bridge_mode_set_pair_timeout(uint16_t s)
{
    s_pair_timeout_s = s;
}

void bridge_mode_set_host_awake(bool awake)
{
    if (s_host_awake == awake) {
        return;
    }
    s_host_awake = awake;
    LOG_I(BRIDGE_MOD_MODE, "host %s", awake ? "awake" : "suspend");
    if (awake) {
        bridge_mode_note_activity();   /* 主机恢复视为活动，抑制深睡 */
    }
    bridge_evt_host_power(awake ? BRIDGE_HOST_AWAKE : BRIDGE_HOST_SUSPEND);
}

bool bridge_mode_host_awake(void)
{
    return s_host_awake;
}

void bridge_mode_poll(void)
{
    /* MODE_SEL 运行期变化（GPIO 硬线设定，UART 命令优先） */
    uint8_t sel = port_gpio_read_mode_sel();
    if (sel != s_gpio_sel) {
        s_gpio_sel = sel;
        uint8_t want = bridge_mode_from_sel(sel);
        if (!s_vbus || want == (uint8_t)MODE_USB) {
            (void)bridge_mode_request(want, true);
        }
    }

    /* VBUS 直采（SK32 未同步时的兜底） */
    bool vbus = port_vbus_present();
    if (vbus != s_vbus) {
        bridge_mode_set_vbus(vbus);
    }

    /* 配对窗口超时：窗口内未配对成功则停配对动作并回落到对应非配对无线模式
     * （BLE_PAIR→BLE / 2G4_PAIR→2G4）。复用 EVT_PAIR_RESULT(1) 上报一次瞬时的
     * 「配对失败/超时」事件。回落同时解除 mode_is_paired_window() 对自动深睡的
     * 门闩，使 2.4G 在超时且无连接时也能按期深睡（原缺陷：停在 2G4_PAIR 永不深睡）。
     * 持续时间由 PAIR_START 的可选参数设置（bridge_mode_set_pair_timeout），
     * 0 = 不自动关闭（窗口恒开，仅连接成功或显式切模式退出）。 */
    if (mode_is_paired_window(s_mode) && s_pair_timeout_s != 0u &&
        (uint32_t)(port_millis() - s_pair_started_ms) >=
            (uint32_t)s_pair_timeout_s * 1000u) {
        uint8_t       r        = radio_of(s_mode);
        bridge_mode_t fallback = (s_mode == MODE_BLE_PAIR) ? MODE_BLE : MODE_2G4;

        LOG_I(BRIDGE_MOD_MODE, "pair window timeout -> %s",
              bridge_mode_name(fallback));
        if (r != BRIDGE_RADIO_NONE) {
            (void)port_radio_pair_stop(r);
        }
        bridge_evt_pair_result(1u, bridge_cfg_get()->ble_profile);
        (void)mode_transaction(fallback, false);
        bridge_mode_note_activity();
    }

    /* 空闲休眠（规格 9.2）：Dongle 无 SK32 无活动来源，且常供电接收，禁睡。
     * 已连接（链路保持）时不深睡——浅睡由 port 层主循环自动进行，
     * 深睡会关射频导致掉线（规格 9.2「无连接且无活动」）。
     * 仅「自动」策略下自动深睡；「指令控制」策略下不自动进睡，
     * 深睡只由 CMD_SLEEP(DEEP) 显式触发。 */
#if !BRIDGE_BOARD_IS_DONGLE
    {
        const bridge_cfg_t *cfg = bridge_cfg_get();
        if (cfg->sleep_policy == BRIDGE_SLEEP_POLICY_AUTO &&
            cfg->sleep_deep_s != 0u &&
            s_mode != MODE_SLEEP && !mode_is_paired_window(s_mode) &&
            !bridge_mode_is_connected() &&
            bridge_mode_idle_ms() >= (uint32_t)cfg->sleep_deep_s * 1000u) {
            LOG_I(BRIDGE_MOD_MODE, "idle %lus -> sleep",
                  (unsigned long)(bridge_mode_idle_ms() / 1000u));
            (void)mode_transaction(MODE_SLEEP, false);
        }
    }
#endif
}
