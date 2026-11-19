/* FR2012A 端口内部接口：BLE / USB 适配层 → core 的上抛入口。
 *
 * 与 port/ch58x/bridge_ch58x.h 同构（仅芯片相关部分不同）：
 *   射频事件与数据都由 BLE 回调（任务上下文）经这里交给 core，
 *   避免 core 反向依赖任何厂商头文件。
 */
#ifndef BRIDGE_FR2012A_H
#define BRIDGE_FR2012A_H

#include <stdint.h>
#include <stdbool.h>

#include "bridge_config.h"

/* ---------------- 连接状态（BLE 适配层维护，port 层查询） ---------------- */
void bridge_fr2012a_set_connected(bool on);
bool bridge_fr2012a_connected(void);

/* ---------------- 射频事件 / 数据入口（BLE 回调上下文） ---------------- */
/* 取值 BRIDGE_RADIO_EVT_*（见 bridge_mode.h） */
void bridge_fr2012a_radio_evt(uint8_t evt);

/* 对端下发的 HID 报文：
 *   Dongle → 键盘上报的 HID 报文，转 USB HID 给主机；
 *   键盘   → 主机 Output Report（LED），回传 SK32。 */
void bridge_fr2012a_hid_in(const uint8_t *payload, uint8_t len);

/* 私有桥链路（GATT 0xFFF0/0xFFF1）收到的字节：
 *   Dongle → 键盘上报的 HID 报文（同 hid_in）；
 *   键盘   → 接收器下发的主机 LED / 挂起状态。 */
void bridge_fr2012a_link_rx(const uint8_t *payload, uint8_t len);

#endif /* BRIDGE_FR2012A_H */
