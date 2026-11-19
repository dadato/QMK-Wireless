/* 协议层：帧结构、命令表、分发、应答/事件
 *
 * 帧格式（小端 CRC，CRC 覆盖 LEN..PAYLOAD）：
 *   +--------+-----+-----+-----+-------+---------+----------+
 *   | SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16    |
 *   | AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B（LE） |
 *   +--------+-----+-----+-----+-------+---------+----------+
 */
#ifndef BRIDGE_PROTO_H
#define BRIDGE_PROTO_H

#include "bridge_config.h"
#include "bridge_types.h"

#define BRIDGE_SOF0  0xAAu
#define BRIDGE_SOF1  0x55u
#define BRIDGE_SOF_LEN   2u
#define BRIDGE_CRC_LEN   2u
#define BRIDGE_HDR_LEN   4u  /* LEN + CMD + SEQ + FLAGS */
#define BRIDGE_FRAME_OVERHEAD (BRIDGE_SOF_LEN + BRIDGE_HDR_LEN + BRIDGE_CRC_LEN)

typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t flags;
    uint8_t len;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];
} bridge_frame_t;

/* ---------------- 生命周期 ---------------- */
void     bridge_proto_init(void);
/* 主循环调用：重发待发帧、ACK 超时重试 */
void     bridge_proto_poll(void);

/* 分发一帧；返回 0 成功，负数为错误码 */
int      bridge_proto_dispatch(const bridge_frame_t *frame);

/* 应用「经射频中继」的配置命令（键盘 → Dongle，见 BRIDGE_RADIO_PKT_CFG）。
 * 仅接受 CMD_SET_NAME / CMD_SET_USB_ID，其余返回 BRIDGE_ERR_UNSUPPORTED。 */
int      bridge_proto_apply_remote(uint8_t cmd, const uint8_t *payload, uint8_t len);

/* ---------------- 应答 / 事件 ---------------- */
void     bridge_proto_ack(uint8_t seq, uint8_t cmd);
/* 带载荷的应答（GET_VERSION / GET_STATUS / GET_MAC 用） */
void     bridge_proto_reply(uint8_t seq, uint8_t cmd, const uint8_t *payload, uint8_t len);
void     bridge_proto_error(uint8_t seq, uint8_t cmd, uint8_t code);
int      bridge_proto_send_event(uint8_t cmd, const uint8_t *payload, uint8_t len);

/* 事件构造函数 */
void     bridge_evt_state(void);
void     bridge_evt_leds(uint8_t led_state);
void     bridge_evt_pair_result(uint8_t result, uint8_t profile);
void     bridge_evt_error(uint8_t code);
void     bridge_evt_pong(uint8_t seq);
/* 透传数据到达（射频或 USB）→ 上报 SK32（EVT_TUNNEL_USB / EVT_TUNNEL_BLE） */
void     bridge_evt_tunnel(uint8_t kind, const uint8_t *payload, uint8_t len);
/* 主机电源状态变化（USB 挂起/恢复 或 BLE HID Control Point）→ 上报 SK32 */
void     bridge_evt_host_power(uint8_t state);

/* ---------------- 主循环驱动（目标侧 port 层） ---------------- */
/* 目标侧不使用 TMOS 任务事件驱动：串口帧占线短（921600 下 64B 载荷帧约 0.7ms），
 * 事件驱动会再叠加调度延迟，故由 port 层主循环直接调用
 * bridge_app_poll()（见 bridge_app.h），TMOS 仅服务 BLE 协议栈。 */

#endif /* BRIDGE_PROTO_H */
