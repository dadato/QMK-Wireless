/* 键盘侧 UART OTA 状态机（WCH IAP 分区，布局见 port/ch58x/bridge_ota_layout.h）
 *
 * 协议（命令码见 bridge_types.h）：
 *   FW_BEGIN [total_len u32][crc16 u16] → 应答 [chunk_max u8]
 *   FW_DATA  [offset u32][data ≤60B，4 对齐且顺序追加]
 *   FW_END   校验通过 → ACK → 置升级标志 + 复位（进 IAP 搬运）
 *   FW_ABORT 放弃本次传输
 *
 * 分工：本模块只做参数/顺序/CRC 校验与状态推进；
 *       实际擦写与标志位由 port 层（port_fw_*）落到 ImageB/DataFlash。
 */
#ifndef BRIDGE_OTA_H
#define BRIDGE_OTA_H

#include "bridge_config.h"
#include "bridge_types.h"

/* 开始：校验长度/CRC 格式并通知端口层准备暂存区；返回 0 / 负错误码 */
int  bridge_ota_begin(const uint8_t *payload, uint8_t len);

/* 数据块：offset 必须等于已写入量（顺序写），长度 4 对齐且 ≤ BRIDGE_FW_CHUNK_MAX */
int  bridge_ota_data(const uint8_t *payload, uint8_t len);

/* 结束校验（不改标志、不复位）：长度齐全且 CRC 相符返回 0，否则负错误码 */
int  bridge_ota_end(const uint8_t *payload, uint8_t len);

/* 应用升级：置 IAP 标志并复位（正常不复返；测试桩下返回） */
void bridge_ota_apply(void);

/* 放弃传输（端口层清理暂存状态） */
void bridge_ota_abort(void);

bool bridge_ota_active(void);

#endif /* BRIDGE_OTA_H */