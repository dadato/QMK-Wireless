/* 桥数据面串口：帧收发、CRC、去重、最小帧间隔
 *
 * D14（BRIDGE_UART_BAUD=921600）时序约束在此实现：
 *   - 两帧最小间隔 BRIDGE_FRAME_MIN_GAP_MS（不足则返回 BRIDGE_ERR_GAP）
 *   - HID 报文去重（与上一帧完全相同则返回 BRIDGE_ERR_DUP）
 */
#ifndef BRIDGE_UART_H
#define BRIDGE_UART_H

#include "bridge_config.h"
#include "bridge_proto.h"

void     bridge_uart_init(uint32_t baudrate);

/* 通用发送：返回 BRIDGE_OK / BRIDGE_ERR_BUSY / BRIDGE_ERR_GAP */
int      bridge_uart_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                          const uint8_t *payload, uint8_t len);

/* HID 发送：在通用发送之上增加去重；同样返回 GAP / BUSY / DUP */
int      bridge_uart_send_hid(uint8_t cmd, const uint8_t *payload, uint8_t len);

bool     bridge_uart_tx_ready(void);
uint32_t bridge_uart_last_tx_ms(void);

/* ISR 侧入口：喂入一个字节 */
void     bridge_uart_isr_feed(uint8_t byte);

/* 任务侧：取一个完整帧 */
bool     bridge_uart_get_frame(bridge_frame_t *out);

/* 心跳 */
void     bridge_uart_ping(void);
/* 周期调用：重试因帧间隔/忙而未能发出的 PING（s_ping_pending） */
void     bridge_uart_poll(void);
void     bridge_uart_on_rx_frame(void);   /* 收到任意合法帧时刷新链路活性 */
/* 刷新「最近接收活动」时间戳（唤醒事件或收到合法帧均可调用）。
 * 供 port 层实现「接收活动后保持清醒」窗口（BRIDGE_RX_HOLD_AWAKE_MS）。 */
void     bridge_uart_note_rx_activity(void);

/* 心跳周期开关（CMD_SET_HEARTBEAT）：
 *   关闭后停止一切心跳相关功能——不发送 PING、不做失联判定，且不回 PONG。
 *   默认开启。仅运行期生效（复位后恢复开启）。 */
void     bridge_uart_heartbeat_enable(bool en);
bool     bridge_uart_heartbeat_enabled(void);

/* 链路活性：单向判据——桥只发 PING，对端回 PONG 或任意合法完整帧即保活；
 * 连续 BRIDGE_LINK_LOST_CNT 个周期未收到对端任意合法帧即判失联。 */
bool     bridge_uart_link_check(void);    /* 周期调用：返回 true 表示链路正常 */
bool     bridge_uart_is_link_alive(void);
uint32_t bridge_uart_rx_gap_ms(void);

/* 统计（测试与诊断用） */
uint16_t bridge_uart_crc_error_count(void);
uint16_t bridge_uart_drop_count(void);

/* 把编码后的帧写入 out，返回整帧长度（不发送，供测试与估算使用） */
uint16_t bridge_uart_encode(uint8_t *out, uint16_t out_size, uint8_t cmd, uint8_t seq,
                            uint8_t flags, const uint8_t *payload, uint8_t len);

#endif /* BRIDGE_UART_H */
