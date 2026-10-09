/* BLE 多主机槽管理 —— 4 个 Profile 的「已绑定」标志与活跃槽（core 可测，仅依赖 bridge_port.h）
 *
 * 背景（spec §7.3 多主机管理）：键盘需记忆 **4 组** BLE 主机绑定，`SET_PROFILE(1~4)`
 * 切换；未配对的 Profile 切过去自动进入配对窗口。
 *
 * 设计（库原生多绑定 + 应用侧仅存槽标志）：
 *   - 绑定数据本身由 WCH 闭源 GAPBondMgr 保存在 BLE SNV 区，且 SDK 已把
 *     `BLE_SNV_NUM` 从 1 提高到 4（见 HAL/config.h），即库**原生记忆 4 个 peer 绑定**，
 *     切槽后库按主机地址自动匹配，无需应用层做整段 SNV 存档/还原。
 *   - 本模块只维护一张小索引表（NV 槽 4）：`status[1..4]` = 该 Profile 是否已配对，
 *     `last_active` = 最近活跃 Profile。用于：
 *       · `GET_BLE_SLOTS` 上报各槽绑定状态；
 *       · 切到未配对槽时自动进入配对窗口。
 *   - 每 Profile 使用各自独立的 BLE 广播地址（末字节 `0xC0|profile`，见
 *     bridge_ch58x_sdk.c 的 sdk_ble_profile_addr），使主机（PC）不会把不同槽误认作
 *     同一设备而抢连。
 *
 * 边界设计（保持可测性）：
 *   - 本文件**不接触 WCH SDK 头**，只通过 `port_nv_read/write`（core/bridge_port.h）读写
 *     应用 NV 槽区，故可纳入主机端测试（port_host.c 的 port_nv 桩）。
 *   - 真正的绑定数据由 SDK 闭源库管理，应用侧不感知。
 */
#ifndef BRIDGE_BLE_MULTIHOST_H
#define BRIDGE_BLE_MULTIHOST_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 槽位常量 ---------------- */
#define BRIDGE_MH_PROFILE_MAX          4u      /* 支持 4 组主机（spec §7.3） */

/* 应用 NV 槽 ID 分配（须与 bridge_config.h 全槽分配一致）：
 *   1..3    = CFG / NAMES / RF24_BOND（既有）
 *   4       = 多主机槽索引表（64B：magic+version+4×status+last_active+CRC）
 *   5..24   = 预留（原整段 SNV 存档槽区，已废弃；槽 8..11 = OTA 标志页 bridge_ota_layout.h）
 *   25..47  = 用户 NV（SK32 用）
 *   48..63  = BLE SNV 区（0x77C00，SDK 占用，4×256B）
 */
#define BRIDGE_NV_ID_MH_IDX          0x0004u

/* ---------------- 状态 ---------------- */
/* 槽索引表：status[1..4]=1 表示该 Profile 已配对，last_active=最近活跃 Profile。 */
#define BRIDGE_MH_MAP_MAGIC 0x4A4Du             /* 'MJ' 小端，索引表标识 */
typedef struct {
    uint8_t  magic[2];                          /* BRIDGE_MH_MAP_MAGIC 小端 */
    uint8_t  version;
    uint8_t  status[BRIDGE_MH_PROFILE_MAX];     /* 每 Profile：1 = 已配对，0 = 未配对 */
    uint8_t  last_active;                       /* 最近使用（当前活跃）的 Profile，1..4，0=无 */
    uint16_t crc;                               /* CRC16(magic..last_active) */
} bridge_mh_map_t;

/* 接口返回码 */
typedef enum {
    BRIDGE_MH_OK = 0,
    BRIDGE_MH_ERR_PARAM,
    BRIDGE_MH_ERR_IO,
    BRIDGE_MH_ERR_CRC,          /* 读到但 CRC 校验失败：视为空槽 */
    BRIDGE_MH_ERR_EMPTY         /* 槽为初始 0xFF / 未配对 */
} bridge_mh_rc_t;

/* ---------------- 接口 ---------------- */

/* 上电：装载槽索引表。无效则清空为全空（无任何已配对 Profile）。 */
void     bridge_mh_init(void);

/* Profile 槽是否已配对 */
bool     bridge_mh_has(uint8_t profile);

/* 当前活跃（最近使用）的 Profile；无任何有效记录返回 0 */
uint8_t  bridge_mh_active(void);

/* 标记某 Profile 已配对（连接建立 / 配对完成时调用），并置为活跃 */
bridge_mh_rc_t bridge_mh_mark_bound(uint8_t profile);

/* 删除单槽标记（UNPAIR(profile)） */
bridge_mh_rc_t bridge_mh_erase(uint8_t profile);

/* 删除全部槽（UNPAIR(0xFF)），清空索引 */
bridge_mh_rc_t bridge_mh_erase_all(void);

/* 标记某 Profile 为最近活跃（切槽成功后调用） */
bridge_mh_rc_t bridge_mh_set_active(uint8_t profile);

#endif /* BRIDGE_BLE_MULTIHOST_H */
