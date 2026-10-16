/* 目标侧（CH58x）port 层对外接口
 *
 * 使用方：
 *   - app/main.c                     ：main 初始化与主循环
 *   - port/ch58x/bridge_ch58x_sdk.c  ：SDK 中断向量与厂商回调转发
 *
 * 分层：
 *   core/  ──bridge_port.h──►  bridge_port_ch58x.c  ──bridge_ch58x_sdk.h──► SDK
 *                                     ▲                                          │
 *                                     └──────── bridge_ch58x_radio_evt() ◄────────┘
 */
#ifndef BRIDGE_CH58X_H
#define BRIDGE_CH58X_H

#include <stdint.h>
#include <stdbool.h>

/* 板级配置（提供 BRIDGE_BOARD_* 等纯数据宏） */
#include "bridge_config.h"

/* ---------------- 主循环 ---------------- */
/* 桥初始化：USB(Dongle) → core（日志/GPIO/配置/串口/HID/电量/协议/模式）
 * 前置条件：sdk_sys_init() 与 sdk_ble_stack_init() 已完成。 */
void bridge_ch58x_init(void);

/* 每轮主循环调用：TMOS 服务 → 串口 TX 排空 → 配对键 → bridge_app_poll() */
void bridge_ch58x_loop(void);

/* 是否允许进入深睡（MODE_SLEEP 且无待发帧） */
bool bridge_ch58x_can_sleep(void);

/* 进入深睡（1s 窗口，内部调用 sdk_sleep()）；返回后继续主循环 */
void bridge_ch58x_sleep(void);

/* 浅睡（IDLE，保持链路）是否可用：非定频、无待发帧、非 Dongle */
bool bridge_ch58x_shallow_ok(void);

/* 主循环尾部调用：深睡 / 浅睡由本函数按当前状态择一进入 */
void bridge_ch58x_idle(void);

/* ---------------- 中断侧入口 ---------------- */
/* 由 UART1_IRQHandler（bridge_ch58x_sdk.c）调用：读空 RX FIFO 并喂给 core */
void bridge_ch58x_uart_isr(void);

/* ---------------- 射频事件 / 数据入口（任务上下文） ---------------- */
/* 射频状态事件：取值 BRIDGE_RADIO_EVT_*（见 bridge_mode.h） */
void bridge_ch58x_radio_evt(uint8_t evt);

/* 射频侧收到的 HID 报文：
 *   Dongle  侧 = 键盘上报的 HID 报文 → USB HID 转发主机
 *   键盘侧    = 主机下发的 Output Report（LED）→ EVT_LEDS 回传 SK32 */
void bridge_ch58x_radio_hid_in(const uint8_t *payload, uint8_t len);

/* 射频侧收到的透传数据（借鉴市售模组 0x88/0x89）：
 *   Dongle 侧 → 经 USB Raw HID 转给上位机；
 *   键盘侧   → EVT_TUNNEL_* 上报 SK32。
 * kind 取值 BRIDGE_TUNNEL_USB / BRIDGE_TUNNEL_BLE。 */
void bridge_ch58x_radio_tunnel_in(uint8_t kind, const uint8_t *payload, uint8_t len);

/* Dongle 侧收到的配置中继帧（键盘经 2.4G 下发，见 BRIDGE_RADIO_PKT_CFG）：
 * 载荷 = [CMD][payload...]，仅处理名称 / USB 标识，本地生效。 */
void bridge_ch58x_radio_cfg_in(const uint8_t *payload, uint8_t len);

/* 键盘侧收到的主机电源状态（Dongle 经 2.4G 下发，见 BRIDGE_RADIO_PKT_HOST）：
 * state 取值 BRIDGE_HOST_*，更新主机电源状态并上报 SK32。 */
void bridge_ch58x_radio_host_in(uint8_t state);

#endif /* BRIDGE_CH58X_H */
