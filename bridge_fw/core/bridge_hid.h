/* HID 转发层：把 SK32 经 UART 送来的 HID 报文转交射频（BLE / 2.4G）
 *
 * 硬约束（规格 11.1 / 6.3）：
 *   1. 本文件所在转发路径（0x10~0x15）**禁止任何 LOG_***，避免破坏上报时序
 *   2. 相同状态重复上报必须抑制（D14：抑制冗余流量，降低链路与射频占用）
 *   3. BLE 模式下 NKRO 不直传，必须降级为 6KRO（D3）
 *   4. 任何模式切换 / 断链 / 复位前必须先 release_all，防止卡键
 */
#ifndef BRIDGE_HID_H
#define BRIDGE_HID_H

#include "bridge_config.h"
#include "bridge_types.h"

void     bridge_hid_init(void);

/* 把一帧 HID 命令转发到射频。返回 BRIDGE_OK / BRIDGE_ERR_DUP / BRIDGE_ERR_STATE */
int      bridge_hid_send(uint8_t cmd, const uint8_t *payload, uint8_t len);

/* 全键释放：对本轮已用过的报文类型各发一次全 0 报文，并清空去重缓存 */
int      bridge_hid_release_all(void);

/* 主机 LED 状态（HID Output Report → EVT_LEDS） */
void     bridge_hid_set_leds(uint8_t led_state);
uint8_t  bridge_hid_get_leds(void);

/* 射频链路状态变化时告知（断开时自动清去重缓存） */
void     bridge_hid_note_link(bool connected);

/* NKRO(16B) → Boot 键盘报文(8B) 降级；返回实际写入的键数（0~6） */
uint8_t  bridge_hid_nkro_to_boot(const uint8_t *nkro, uint8_t *boot8);

/* 诊断计数 */
uint16_t bridge_hid_forward_count(void);
uint16_t bridge_hid_dup_count(void);

#endif /* BRIDGE_HID_H */
