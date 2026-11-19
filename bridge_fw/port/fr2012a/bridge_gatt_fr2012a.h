/* FR2012A 私有桥 GATT 服务（键盘模组 ↔ 接收器）
 *
 * 服务 0xFFF0：
 *   0xFFF1  Write       接收器 → 模组（主机 LED / 挂起状态）
 *   0xFFF2  Notify      模组 → 接收器（HID 报文）
 *
 * 与 CH58x 的 2.4G 私有链路等价：本芯片只有 BLE 射频，故 `BRIDGE_RADIO_2G4`
 * 用这条 BLE 私有链路承载（见 port/fr2012a/bridge_port_fr2012a.c 的说明）。
 */
#ifndef BRIDGE_GATT_FR2012A_H
#define BRIDGE_GATT_FR2012A_H

#include <stdint.h>
#include <stdbool.h>

#define BRIDGE_GATT_SVC_UUID   0xFFF0u
#define BRIDGE_GATT_RX_UUID    0xFFF1u
#define BRIDGE_GATT_TX_UUID    0xFFF2u

/* 单帧最大载荷：桥协议 payload 上限 64B */
#define BRIDGE_GATT_PAYLOAD_MAX  64u

void bridge_gatt_init(void);

/* 链路状态（BLE 适配层在连上/断开时调用） */
void bridge_gatt_set_link(uint8_t conidx, bool up);

/* 模组侧：向已订阅的接收器发通知；未订阅/未连接返回 BRIDGE_ERR_STATE */
int  bridge_gatt_notify(const uint8_t *data, uint8_t len);

#endif /* BRIDGE_GATT_FR2012A_H */
