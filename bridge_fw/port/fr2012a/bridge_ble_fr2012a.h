/* FR2012A BLE 射频适配层（键盘模组 / 接收器共用）
 *
 * 与 CH58x 的对应关系：
 *   bridge_ch58x_sdk.c 的 sdk_ble_*   → 本文件 bridge_fr_ble_*
 *   bridge_radio_ch58x.c 的 2.4G 通路 → 本文件私有 GATT 链路（0xFFF0）
 *
 * 角色差异（BRIDGE_BOARD_IS_DONGLE）：
 *   键盘模组：BLE 外设（HOGP + 私有桥服务），可连接主机或接收器；
 *   接收器  ：BLE 主机（GATT client），扫描并连接模组，收 HID 报文转 USB。
 */
#ifndef BRIDGE_BLE_FR2012A_H
#define BRIDGE_BLE_FR2012A_H

#include <stdint.h>
#include <stdbool.h>

/* 注册 GAP 回调、安全参数、名称与 GATT 服务（协议栈初始化之后调用） */
void bridge_fr_ble_init(void);

/* 按模式开射频：BRIDGE_RADIO_BLE / BRIDGE_RADIO_2G4 */
int  bridge_fr_ble_start(uint8_t radio);
void bridge_fr_ble_stop(void);

bool bridge_fr_ble_is_connected(void);

/* HID 报文出站（键盘侧：按长度选 HOGP 报表；接收器侧：写私有桥特征） */
int  bridge_fr_ble_send(const uint8_t *payload, uint8_t len);

/* 接收器 → 模组：主机 LED / 主机挂起状态（私有桥下行帧） */
int  bridge_fr_ble_send_led(uint8_t leds);
int  bridge_fr_ble_send_host_power(uint8_t state);

/* 配对窗口：键盘重新开广播；接收器清绑定并重新扫描 */
void bridge_fr_ble_pair_start(void);
void bridge_fr_ble_unpair(void);

/* 上报周期（ms）→ 连接间隔（下限 7.5ms）；未连接时忽略 */
int  bridge_fr_ble_set_interval_ms(uint16_t ms);

void bridge_fr_ble_set_name(const uint8_t *name, uint8_t len);

/* 主循环服务：接收器断链后自动重新扫描 */
void bridge_fr_ble_poll(void);

#endif /* BRIDGE_BLE_FR2012A_H */
