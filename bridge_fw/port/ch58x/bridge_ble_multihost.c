/* BLE 多主机槽管理实现 —— 见 bridge_ble_multihost.h 顶部设计说明
 *
 * 本文件**不 #include WCH SDK 头**，仅通过 core/bridge_port.h 的 port_nv_read/write
 * 读写应用 NV 槽区（NV 槽 4），故可纳入主机端测试（sdk_stub 顶替 SDK、port_host.c 顶替 port_nv）。
 *
 * 只维护一张小索引表：status[1..4]（已配对标志）+ last_active（活跃槽）+ magic/version/CRC。
 * 绑定数据本身由 WCH 闭源 GAPBondMgr 保存在 BLE SNV 区（SDK BLE_SNV_NUM=4），应用侧不感知。
 */
#include <string.h>

#include "bridge_port.h"         /* port_nv_read/write */
#include "bridge_crc16.h"
#include "bridge_types.h"        /* BRIDGE_OK/ERR_* */
#include "bridge_ble_multihost.h"

/* ---------------- 内部常量 ---------------- */
#define BRIDGE_MH_VERSION   1u

static bridge_mh_map_t s_map;

/* ---------------- 索引表 ---------------- */

static void map_zero(void)
{
    memset(&s_map, 0, sizeof(s_map));
    s_map.magic[0] = (uint8_t)(BRIDGE_MH_MAP_MAGIC & 0xFFu);
    s_map.magic[1] = (uint8_t)(BRIDGE_MH_MAP_MAGIC >> 8);
    s_map.version  = BRIDGE_MH_VERSION;
}

static bridge_mh_rc_t map_commit(void)
{
    uint16_t crc = bridge_crc16_ccitt((const uint8_t *)&s_map,
                                      (uint16_t)(sizeof(s_map) - 2u));
    s_map.crc = crc;
    return (port_nv_write(BRIDGE_NV_ID_MH_IDX, &s_map, (uint16_t)sizeof(s_map)) == BRIDGE_OK)
               ? BRIDGE_MH_OK
               : BRIDGE_MH_ERR_IO;
}

static void map_load(void)
{
    bridge_mh_map_t tmp;
    uint16_t        crc;
    uint16_t        stored;

    map_zero();                                   /* 默认空表 */
    if (port_nv_read(BRIDGE_NV_ID_MH_IDX, &tmp, (uint16_t)sizeof(tmp)) != BRIDGE_OK) {
        (void)map_commit();                       /* 首次上电：写一份干净空表 */
        return;
    }
    if (tmp.magic[0] != (uint8_t)(BRIDGE_MH_MAP_MAGIC & 0xFFu) ||
        tmp.magic[1] != (uint8_t)(BRIDGE_MH_MAP_MAGIC >> 8)) {
        (void)map_commit();
        return;
    }
    crc    = bridge_crc16_ccitt((const uint8_t *)&tmp, (uint16_t)(sizeof(tmp) - 2u));
    stored = tmp.crc;
    if (crc != stored) {
        (void)map_commit();                       /* 校验失败：重建空表 */
        return;
    }
    memcpy(&s_map, &tmp, sizeof(s_map));
}

/* ---------------- 接口 ---------------- */

void bridge_mh_init(void)
{
    map_load();
}

bool bridge_mh_has(uint8_t profile)
{
    if (profile < 1u || profile > BRIDGE_MH_PROFILE_MAX) {
        return false;
    }
    return s_map.status[profile - 1u] == 1u;
}

uint8_t bridge_mh_active(void)
{
    return s_map.last_active;
}

bridge_mh_rc_t bridge_mh_mark_bound(uint8_t profile)
{
    if (profile < 1u || profile > BRIDGE_MH_PROFILE_MAX) {
        return BRIDGE_MH_ERR_PARAM;
    }
    s_map.status[profile - 1u] = 1u;
    s_map.last_active          = profile;
    return map_commit();
}

bridge_mh_rc_t bridge_mh_erase(uint8_t profile)
{
    if (profile < 1u || profile > BRIDGE_MH_PROFILE_MAX) {
        return BRIDGE_MH_ERR_PARAM;
    }
    s_map.status[profile - 1u] = 0u;
    if (s_map.last_active == profile) {
        s_map.last_active = 0u;
    }
    return map_commit();
}

bridge_mh_rc_t bridge_mh_erase_all(void)
{
    for (uint8_t p = 0u; p < BRIDGE_MH_PROFILE_MAX; p++) {
        s_map.status[p] = 0u;
    }
    s_map.last_active = 0u;
    return map_commit();
}

bridge_mh_rc_t bridge_mh_set_active(uint8_t profile)
{
    if (profile < 1u || profile > BRIDGE_MH_PROFILE_MAX) {
        return BRIDGE_MH_ERR_PARAM;
    }
    s_map.last_active = profile;
    return map_commit();
}
