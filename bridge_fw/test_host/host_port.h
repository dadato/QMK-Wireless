/* 主机端验证用 port 层辅助接口（仅测试使用） */
#ifndef HOST_PORT_H
#define HOST_PORT_H

#include <stdint.h>
#include <stdbool.h>

#define HOST_RADIO_MAX_PKT   512
#define HOST_TX_BUF_SIZE     8192
#define HOST_RADIO_PAYLOAD   64
#define HOST_TUNNEL_BUF      64

typedef struct {
    uint8_t mode;                    /* BRIDGE_RADIO_* */
    uint8_t len;
    uint8_t data[HOST_RADIO_PAYLOAD];
} host_radio_pkt_t;

/* ---- 时间 ---- */
extern uint32_t g_host_ms;
void     host_advance_ms(uint32_t dt);
void     host_time_reset(void);

/* ---- UART TX 抓包 ---- */
extern uint8_t  g_host_tx[HOST_TX_BUF_SIZE];
extern uint16_t g_host_tx_len;
void     host_tx_reset(void);
/* 在 TX 缓冲区中查找指定 CMD 的合法帧；找到返回 1 */
int      host_tx_find_frame(uint8_t cmd, uint8_t *out_flags,
                            uint8_t *out_payload, uint8_t *out_len);
/* 统计 TX 缓冲区中的合法帧总数 */
uint16_t host_tx_frame_count(void);

/* 注入 RX 字节（模拟 SK32 → 桥） */
void     host_uart_inject(const uint8_t *data, uint16_t len);
/* 强制 UART TX 忙（模拟硬件 FIFO 塞满） */
void     host_uart_force_busy(bool busy);

/* ---- 射频 ---- */
extern host_radio_pkt_t g_host_radio[HOST_RADIO_MAX_PKT];
extern uint16_t         g_host_radio_count;
void     host_radio_reset(void);
void     host_radio_clear_links(void);
void     host_radio_set_connected(uint8_t mode, bool connected);
void     host_radio_fail_next(bool fail);
host_radio_pkt_t *host_radio_last(void);
uint32_t host_radio_unpair_count(uint8_t mode);   /* mode = BRIDGE_RADIO_* */
void     host_radio_unpair_reset(void);
uint32_t host_radio_pair_start_count(uint8_t mode); /* mode = BRIDGE_RADIO_* */
void     host_radio_pair_start_reset(void);
uint32_t host_radio_pair_stop_count(uint8_t mode); /* mode = BRIDGE_RADIO_* */
void     host_radio_pair_stop_reset(void);
uint32_t host_radio_adv_resume_count(uint8_t mode); /* mode = BRIDGE_RADIO_* */
void     host_radio_adv_resume_reset(void);

/* ---- 透传通道（port_tunnel_send 抓包） ---- */
extern uint8_t  g_host_tunnel[HOST_TUNNEL_BUF];
extern uint16_t g_host_tunnel_len;
extern uint8_t  g_host_tunnel_kind;    /* BRIDGE_TUNNEL_USB / BRIDGE_TUNNEL_BLE */
void     host_tunnel_reset(void);

/* ---- 名称 / USB 标识 / 配置中继（CMD_SET_NAME / CMD_SET_USB_ID 抓包） ---- */
void     host_name_reset(void);
int      host_name_target(void);            /* -1 = 未调用 */
const uint8_t *host_name(uint8_t *len);     /* NULL = 未调用 */
int      host_usb_vid(void);                /* -1 = 未调用 */
int      host_usb_pid(void);                /* -1 = 未调用 */
int      host_cfg_relay_len(void);          /* -1 = 未调用 */
const uint8_t *host_cfg_relay(uint8_t *len);/* NULL = 未调用 */

/* ---- 非易失存储 ---- */
void     host_nv_clear(void);

/* ---- 固件升级（port_fw_* 抓包：镜像内容/完成 CRC/调用计数） ---- */
void     host_fw_reset(void);
uint32_t host_fw_total(void);
uint32_t host_fw_written(void);
uint16_t host_fw_finish_crc(void);
uint32_t host_fw_finish_count(void);
uint32_t host_fw_abort_count(void);
const uint8_t *host_fw_data(uint32_t *len);

/* ---- GPIO / 电源 ---- */
void     host_set_vbus(bool present);
uint32_t host_reset_count(void);
uint32_t host_bootloader_count(void);

/* ---- 日志 ---- */
extern uint16_t g_host_log_lines;
void     host_log_reset(void);

#endif /* HOST_PORT_H */
