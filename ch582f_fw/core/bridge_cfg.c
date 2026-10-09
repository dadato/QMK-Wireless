#include "bridge_cfg.h"
#include "bridge_crc16.h"
#include "bridge_port.h"
#include "bridge_log.h"
#include "bridge_mode.h"
#include <string.h>

#define BRIDGE_MOD_CFG "cfg"

static bridge_cfg_t s_cfg;

/* 名称在 RAM 中保持 '\0' 结尾，落盘 blob 不带结尾符（长度另存） */
static char s_ble_name[BRIDGE_NAME_MAX + 1u];
static char s_usb_name[BRIDGE_NAME_MAX + 1u];

static void names_set_defaults(void)
{
    const char *b = BRIDGE_BLE_NAME_DEFAULT;
    const char *u = BRIDGE_USB_NAME_DEFAULT;
    uint8_t     bl = (uint8_t)strlen(b);
    uint8_t     ul = (uint8_t)strlen(u);

    if (bl > BRIDGE_NAME_MAX) {
        bl = BRIDGE_NAME_MAX;
    }
    if (ul > BRIDGE_NAME_MAX) {
        ul = BRIDGE_NAME_MAX;
    }
    memcpy(s_ble_name, b, bl);
    s_ble_name[bl] = '\0';
    memcpy(s_usb_name, u, ul);
    s_usb_name[ul] = '\0';
    s_cfg.ble_name_len = bl;
    s_cfg.usb_name_len = ul;
}

/* 名称 blob（NV 槽 64B）：[0]=ble_len [1]=usb_len
 * [2 .. 2+BRIDGE_NAME_MAX-1]=ble [2+BRIDGE_NAME_MAX .. ]=usb */
static int names_store(void)
{
    uint8_t blob[BRIDGE_NV_SLOT_SIZE];

    memset(blob, 0, sizeof(blob));
    blob[0] = s_cfg.ble_name_len;
    blob[1] = s_cfg.usb_name_len;
    memcpy(&blob[2], s_ble_name, s_cfg.ble_name_len);
    memcpy(&blob[2u + BRIDGE_NAME_MAX], s_usb_name, s_cfg.usb_name_len);
    return port_nv_write(BRIDGE_NV_ID_NAMES, blob, (uint16_t)sizeof(blob));
}

static bool names_load(void)
{
    uint8_t blob[BRIDGE_NV_SLOT_SIZE];
    uint8_t bl;
    uint8_t ul;

    if (port_nv_read(BRIDGE_NV_ID_NAMES, blob, (uint16_t)sizeof(blob)) != BRIDGE_OK) {
        return false;
    }
    bl = blob[0];
    ul = blob[1];
    if (bl == 0u || bl > BRIDGE_NAME_MAX || ul == 0u || ul > BRIDGE_NAME_MAX) {
        return false;
    }
    memcpy(s_ble_name, &blob[2], bl);
    s_ble_name[bl] = '\0';
    memcpy(s_usb_name, &blob[2u + BRIDGE_NAME_MAX], ul);
    s_usb_name[ul] = '\0';
    s_cfg.ble_name_len = bl;
    s_cfg.usb_name_len = ul;
    return true;
}

static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic            = BRIDGE_CFG_MAGIC;
    s_cfg.version          = BRIDGE_CFG_VERSION;
    s_cfg.last_mode        = bridge_mode_default();   /* Dongle=2G4，键盘=BLE */
    s_cfg.ble_profile      = 1u;
    s_cfg.log_level        = BRIDGE_LOG_LEVEL;
    s_cfg.rf24_report_ms   = (uint16_t)(1000u / BRIDGE_RF24_REPORT_HZ);
    s_cfg.adv_name_with_mac = 1u;
    s_cfg.conn_interval    = 6u;   /* 6 * 1.25ms = 7.5ms */
    s_cfg.battery_src      = BRIDGE_BATT_SRC_AUTO;
    s_cfg.sleep_policy     = BRIDGE_SLEEP_POLICY_AUTO;
    s_cfg.usb_vid          = (uint16_t)BRIDGE_USB_VID;
    s_cfg.usb_pid          = (uint16_t)BRIDGE_USB_PID;
    names_set_defaults();
}

static void cfg_seal(void)
{
    s_cfg.crc = bridge_crc16_ccitt((const uint8_t *)&s_cfg,
                                   (uint16_t)(sizeof(s_cfg) - sizeof(s_cfg.crc)));
}

static bool cfg_valid(const bridge_cfg_t *c)
{
    if (c->magic != BRIDGE_CFG_MAGIC || c->version != BRIDGE_CFG_VERSION) {
        return false;
    }
    uint16_t crc = bridge_crc16_ccitt((const uint8_t *)c,
                                      (uint16_t)(sizeof(*c) - sizeof(c->crc)));
    return (crc == c->crc);
}

static int cfg_store(void)
{
    cfg_seal();
    return port_nv_write(BRIDGE_NV_ID_CFG, &s_cfg, (uint16_t)sizeof(s_cfg));
}

int bridge_cfg_init(void)
{
    cfg_defaults();

    bridge_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    int rc = port_nv_read(BRIDGE_NV_ID_CFG, &tmp, (uint16_t)sizeof(tmp));
    if (rc == BRIDGE_OK && cfg_valid(&tmp)) {
        s_cfg = tmp;
        if (!names_load()) {
            /* 名称槽缺失/损坏（含从 v1 配置升级）：回落默认名并补齐 NV */
            LOG_W(BRIDGE_MOD_CFG, "names absent/invalid, write defaults");
            names_set_defaults();
            (void)cfg_store();
            (void)names_store();
        }
        LOG_I(BRIDGE_MOD_CFG, "loaded mode=%u profile=%u log=%u report=%ums vid=%04X pid=%04X",
              s_cfg.last_mode, s_cfg.ble_profile, s_cfg.log_level, s_cfg.rf24_report_ms,
              (unsigned)s_cfg.usb_vid, (unsigned)s_cfg.usb_pid);
        return BRIDGE_OK;
    }

    LOG_W(BRIDGE_MOD_CFG, "invalid/absent cfg (rc=%d), write defaults", rc);
    (void)cfg_store();
    (void)names_store();
    return BRIDGE_OK;
}

const bridge_cfg_t *bridge_cfg_get(void)
{
    return &s_cfg;
}

int bridge_cfg_set_mode(uint8_t mode)
{
    if (mode > (uint8_t)MODE_SLEEP) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.last_mode == mode) {
        return BRIDGE_OK;
    }
    s_cfg.last_mode = mode;
    return cfg_store();
}

int bridge_cfg_set_profile(uint8_t profile)
{
    if (profile < 1u || profile > (uint8_t)BRIDGE_MAX_BLE_PROFILE) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.ble_profile == profile) {
        return BRIDGE_OK;
    }
    s_cfg.ble_profile = profile;
    return cfg_store();
}

int bridge_cfg_set_log_level(uint8_t level)
{
#if BRIDGE_LOG_ENABLE
    if (level > BRIDGE_LOG_DEBUG) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.log_level == level) {
        return BRIDGE_OK;
    }
    s_cfg.log_level = level;
    bridge_log_set_level(level);
    return cfg_store();
#else
    (void)level;
    /* D13：编译期关闭日志时该命令返回"不支持" */
    return BRIDGE_ERR_UNSUPPORTED;
#endif
}

int bridge_cfg_set_rf24_report_ms(uint16_t ms)
{
    if (ms == 0u || ms > 100u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.rf24_report_ms == ms) {
        return BRIDGE_OK;
    }
    s_cfg.rf24_report_ms = ms;
    return cfg_store();
}

int bridge_cfg_set_adv_name_mac(uint8_t en)
{
    uint8_t v = (en != 0u) ? 1u : 0u;
    if (s_cfg.adv_name_with_mac == v) {
        return BRIDGE_OK;
    }
    s_cfg.adv_name_with_mac = v;
    return cfg_store();
}

int bridge_cfg_set_conn_interval(uint8_t units_1p25ms)
{
    /* 0 = 不覆盖（沿用协议栈/厂商默认）；BLE 规范下限 6 × 1.25ms = 7.5ms，
     * 1..5 非法。上限由 uint8_t 形参天然约束（GAPRole 接口同为 uint8_t）。 */
    if (units_1p25ms != 0u && units_1p25ms < 6u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.conn_interval == units_1p25ms) {
        return BRIDGE_OK;
    }
    s_cfg.conn_interval = units_1p25ms;
    return cfg_store();
}

int bridge_cfg_set_battery_src(uint8_t src)
{
    if (src > BRIDGE_BATT_SRC_EXTERNAL) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.battery_src == src) {
        return BRIDGE_OK;
    }
    s_cfg.battery_src = src;
    return cfg_store();
}

int bridge_cfg_set_sleep_policy(uint8_t policy)
{
    if (policy > BRIDGE_SLEEP_POLICY_MANUAL) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.sleep_policy == policy) {
        return BRIDGE_OK;
    }
    s_cfg.sleep_policy = policy;
    return cfg_store();
}

int bridge_cfg_set_name(uint8_t target, const uint8_t *name, uint8_t len)
{
    char   *dst;
    uint8_t *plen;

    if (name == 0 || len == 0u || len > BRIDGE_NAME_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    if (target == BRIDGE_NAME_TARGET_BLE) {
        dst  = s_ble_name;
        plen = &s_cfg.ble_name_len;
    } else if (target == BRIDGE_NAME_TARGET_USB) {
        dst  = s_usb_name;
        plen = &s_cfg.usb_name_len;
    } else {
        return BRIDGE_ERR_PARAM;
    }

    if (*plen == len && memcmp(dst, name, len) == 0) {
        return BRIDGE_OK;       /* 幂等：内容未变不落盘 */
    }
    memcpy(dst, name, len);
    dst[len] = '\0';
    *plen    = len;

    if (cfg_store() != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }
    return names_store();
}

const char *bridge_cfg_ble_name(void)
{
    return s_ble_name;
}

const char *bridge_cfg_usb_name(void)
{
    return s_usb_name;
}

uint8_t bridge_cfg_ble_name_len(void)
{
    return s_cfg.ble_name_len;
}

uint8_t bridge_cfg_usb_name_len(void)
{
    return s_cfg.usb_name_len;
}

int bridge_cfg_set_usb_ids(uint16_t vid, uint16_t pid)
{
    if (vid == 0u || pid == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_cfg.usb_vid == vid && s_cfg.usb_pid == pid) {
        return BRIDGE_OK;
    }
    s_cfg.usb_vid = vid;
    s_cfg.usb_pid = pid;
    return cfg_store();
}

int bridge_cfg_apply(uint8_t key, const uint8_t *value, uint8_t len)
{
    if (value == 0) {
        return BRIDGE_ERR_PARAM;
    }

    switch (key) {
        case BRIDGE_CFG_KEY_REPORT_PERIOD:
            if (len != 2u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_rf24_report_ms(
                (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8)));

        case BRIDGE_CFG_KEY_LOG_LEVEL:
            if (len != 1u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_log_level(value[0]);

        case BRIDGE_CFG_KEY_ADV_NAME_MAC:
            if (len != 1u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_adv_name_mac(value[0]);

        case BRIDGE_CFG_KEY_CONN_INTERVAL:
            if (len != 1u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_conn_interval(value[0]);

        case BRIDGE_CFG_KEY_BATTERY_SRC:
            if (len != 1u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_battery_src(value[0]);

        case BRIDGE_CFG_KEY_SLEEP_POLICY:
            if (len != 1u) {
                return BRIDGE_ERR_PARAM;
            }
            return bridge_cfg_set_sleep_policy(value[0]);

        default:
            return BRIDGE_ERR_UNSUPPORTED;
    }
}
