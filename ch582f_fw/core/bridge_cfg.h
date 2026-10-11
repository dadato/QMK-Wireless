/* 持久化配置（DataFlash 掉电保存）
 *
 * 存储结构带 magic + 版本 + CRC，CRC 不符即回落默认值（不使用外挂 Flash，D8）。
 */
#ifndef BRIDGE_CFG_H
#define BRIDGE_CFG_H

#include "bridge_config.h"
#include "bridge_types.h"

#define BRIDGE_CFG_MAGIC     0x334Du   /* "3M" */
#define BRIDGE_CFG_VERSION   4u

/* SET_CONFIG（0x09）的 key */
#define BRIDGE_CFG_KEY_REPORT_PERIOD  0x01u   /* value: u16 LE, ms */
#define BRIDGE_CFG_KEY_LOG_LEVEL      0x02u   /* value: u8 */
#define BRIDGE_CFG_KEY_ADV_NAME_MAC   0x03u   /* value: u8, 1=广播名带 MAC 后缀 */
#define BRIDGE_CFG_KEY_CONN_INTERVAL  0x04u   /* value: u8, 单位 1.25ms */
#define BRIDGE_CFG_KEY_BATTERY_SRC    0x05u   /* value: u8, 见 BRIDGE_BATT_SRC_* */
#define BRIDGE_CFG_KEY_SLEEP_POLICY   0x06u   /* value: u8, 见 BRIDGE_SLEEP_POLICY_* */

/* 名称与配置分开存放（各自一个 NV 槽），此处只记录长度与 USB 标识 */
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  last_mode;         /* bridge_mode_t */
    uint8_t  ble_profile;       /* 1..4 */
    uint8_t  log_level;         /* 0..4 */
    uint16_t rf24_report_ms;    /* 默认 BRIDGE_RF24_REPORT_HZ → 1ms */
    uint8_t  adv_name_with_mac;
    uint8_t  conn_interval;
    uint8_t  battery_src;       /* BRIDGE_BATT_SRC_* */
    uint8_t  sleep_policy;      /* BRIDGE_SLEEP_POLICY_* */
    uint16_t sleep_idle_s;      /* AUTO 策略：浅睡(IDLE)门限，单位 s，0=禁用自动浅睡 */
    uint16_t sleep_deep_s;      /* AUTO 策略：深睡(DEEP)门限，单位 s，0=禁用自动深睡 */
    uint8_t  ble_name_len;      /* 有效长度 0..BRIDGE_NAME_MAX，0 = 用默认名 */
    uint8_t  usb_name_len;
    uint16_t usb_vid;
    uint16_t usb_pid;
    uint16_t crc;
} bridge_cfg_t;

/* 读取配置；校验失败则写入默认值。返回 BRIDGE_OK / BRIDGE_ERR_IO */
int                 bridge_cfg_init(void);
const bridge_cfg_t *bridge_cfg_get(void);

/* 修改并落盘；返回 BRIDGE_OK / BRIDGE_ERR_PARAM / BRIDGE_ERR_IO */
int  bridge_cfg_set_mode(uint8_t mode);
int  bridge_cfg_set_profile(uint8_t profile);
int  bridge_cfg_set_log_level(uint8_t level);
int  bridge_cfg_set_rf24_report_ms(uint16_t ms);
int  bridge_cfg_set_adv_name_mac(uint8_t en);
int  bridge_cfg_set_conn_interval(uint8_t units_1p25ms);
int  bridge_cfg_set_battery_src(uint8_t src);
int  bridge_cfg_set_sleep_policy(uint8_t policy);
/* AUTO 自动休眠策略下的时间门限（单位 s，0 = 禁用该项自动休眠），见 CMD_SLEEP_CFG */
int  bridge_cfg_set_sleep_times(uint16_t idle_s, uint16_t deep_s);

/* 可改写名称（target = BRIDGE_NAME_TARGET_*）；名称以字节计，不带 '\0' */
int  bridge_cfg_set_name(uint8_t target, const uint8_t *name, uint8_t len);
const char *bridge_cfg_ble_name(void);       /* 永远返回合法 C 字符串 */
const char *bridge_cfg_usb_name(void);
uint8_t      bridge_cfg_ble_name_len(void);
uint8_t      bridge_cfg_usb_name_len(void);

/* USB 标识（VID/PID）：写入后需重新枚举才生效（由 port 层负责） */
int  bridge_cfg_set_usb_ids(uint16_t vid, uint16_t pid);

/* SET_CONFIG 统一入口：key + 原始 value 指针 */
int  bridge_cfg_apply(uint8_t key, const uint8_t *value, uint8_t len);

#endif /* BRIDGE_CFG_H */
