/* bridge_port.h 的主机实现：全部用可注入的内存模型，供 PC 上验证 core/ */
#include "bridge_port.h"
#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_uart.h"
#include "bridge_crc16.h"
#include "bridge_proto.h"
#include "host_port.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 时间 ---------------- */
uint32_t g_host_ms;
static uint32_t s_reset_count;
static uint32_t s_boot_count;

void host_advance_ms(uint32_t dt) { g_host_ms += dt; }
void host_time_reset(void) { g_host_ms = 0u; }

uint32_t port_millis(void) { return g_host_ms; }
void port_delay_ms(uint32_t ms) { g_host_ms += ms; }

/* ---------------- UART ---------------- */
uint8_t  g_host_tx[HOST_TX_BUF_SIZE];
uint16_t g_host_tx_len;

static bool     s_uart_busy_forced;
static uint32_t s_tx_busy_until;
static bool     s_uart_inited;
static uint32_t s_uart_baud;

void host_tx_reset(void) { g_host_tx_len = 0u; }

void host_uart_force_busy(bool busy) { s_uart_busy_forced = busy; }

void host_uart_inject(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        bridge_uart_isr_feed(data[i]);
    }
}

void port_uart_init(uint32_t baudrate)
{
    s_uart_inited = true;
    s_uart_baud   = baudrate;
    s_tx_busy_until = 0u;
    s_uart_busy_forced = false;
}

void port_uart_write(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        if (g_host_tx_len < HOST_TX_BUF_SIZE) {
            g_host_tx[g_host_tx_len++] = data[i];
        }
    }
    /* 按当前波特率模拟帧占线时间（向下取整到 ms） */
    if (s_uart_baud != 0u) {
        uint32_t us = (uint32_t)len * 10000000u / s_uart_baud;
        s_tx_busy_until = g_host_ms + (us / 1000u);
    }
}

bool port_uart_tx_busy(void)
{
    if (s_uart_busy_forced) {
        return true;
    }
    return ((int32_t)(s_tx_busy_until - g_host_ms) > 0);
}

void port_uart_irq_enable(bool en) { (void)en; }

/* 主机侧 TX 为即时投递（无软件环/硬件 FIFO），无需排空。 */
void port_uart_tx_flush(void) { }

/* ---------------- 非易失存储 ---------------- */
#define NV_BLOCKS 4
typedef struct { uint16_t id; uint16_t len; uint8_t data[64]; bool used; } nv_blk_t;
static nv_blk_t s_nv[NV_BLOCKS];

void host_nv_clear(void) { memset(s_nv, 0, sizeof(s_nv)); }

int port_nv_read(uint16_t id, void *buf, uint16_t len)
{
    for (uint8_t i = 0u; i < NV_BLOCKS; i++) {
        if (s_nv[i].used && s_nv[i].id == id && s_nv[i].len >= len) {
            memcpy(buf, s_nv[i].data, len);
            return BRIDGE_OK;
        }
    }
    return -1;
}

int port_nv_write(uint16_t id, const void *buf, uint16_t len)
{
    if (len > sizeof(s_nv[0].data)) {
        return -1;
    }
    uint8_t slot = NV_BLOCKS;
    for (uint8_t i = 0u; i < NV_BLOCKS; i++) {
        if (s_nv[i].used && s_nv[i].id == id) {
            slot = i;
            break;
        }
        if (!s_nv[i].used && slot == NV_BLOCKS) {
            slot = i;
        }
    }
    if (slot == NV_BLOCKS) {
        return -1;
    }
    s_nv[slot].id   = id;
    s_nv[slot].len  = len;
    s_nv[slot].used = true;
    memcpy(s_nv[slot].data, buf, len);
    return BRIDGE_OK;
}

/* ---------------- 射频 ---------------- */
host_radio_pkt_t g_host_radio[HOST_RADIO_MAX_PKT];
uint16_t         g_host_radio_count;
static bool      s_radio_conn[3];
static bool      s_radio_up[3];
static bool      s_radio_fail_next;
static uint8_t   s_test_channel;

void host_radio_reset(void)
{
    /* 只清空抓包缓冲与故障注入，**不得**清连接状态：
     * 连接状态由 core 的模式事务驱动，测试里被清掉会与 core 视图不一致 */
    g_host_radio_count = 0u;
    s_radio_fail_next  = false;
}

void host_radio_clear_links(void)
{
    memset(s_radio_conn, 0, sizeof(s_radio_conn));
    memset(s_radio_up, 0, sizeof(s_radio_up));
}

void host_radio_set_connected(uint8_t mode, bool connected)
{
    if (mode < 3u) {
        s_radio_conn[mode] = connected;
    }
}

void host_radio_fail_next(bool fail) { s_radio_fail_next = fail; }

host_radio_pkt_t *host_radio_last(void)
{
    return (g_host_radio_count > 0u) ? &g_host_radio[g_host_radio_count - 1u] : 0;
}

int port_radio_init(uint8_t mode)
{
    if (mode > BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    s_radio_up[mode] = true;
    return BRIDGE_OK;
}

int port_radio_deinit(uint8_t mode)
{
    if (mode > BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    s_radio_up[mode]   = false;
    s_radio_conn[mode] = false;
    return BRIDGE_OK;
}

int port_radio_send(uint8_t mode, const uint8_t *payload, uint8_t len)
{
    if (mode > BRIDGE_RADIO_2G4 || !s_radio_up[mode]) {
        return BRIDGE_ERR_STATE;
    }
    if (s_radio_fail_next) {
        s_radio_fail_next = false;
        return BRIDGE_ERR_IO;
    }
    if (g_host_radio_count < HOST_RADIO_MAX_PKT) {
        host_radio_pkt_t *p = &g_host_radio[g_host_radio_count++];
        p->mode = mode;
        p->len  = (len > HOST_RADIO_PAYLOAD) ? HOST_RADIO_PAYLOAD : len;
        memset(p->data, 0, sizeof(p->data));
        memcpy(p->data, payload, p->len);
    }
    return BRIDGE_OK;
}

bool port_radio_is_connected(uint8_t mode)
{
    return (mode < 3u) && s_radio_conn[mode];
}

uint8_t port_radio_protocol_ver(uint8_t mode) { (void)mode; return 1u; }

int port_radio_pair_start(uint8_t mode) { (void)mode; return BRIDGE_OK; }

static uint32_t s_pair_stop_cnt[3];   /* 下标 = BRIDGE_RADIO_*（0/1/2） */

int port_radio_pair_stop(uint8_t mode)
{
    if (mode < 3u) {
        s_pair_stop_cnt[mode]++;
    }
    return BRIDGE_OK;
}

uint32_t host_radio_pair_stop_count(uint8_t mode)
{
    return (mode < 3u) ? s_pair_stop_cnt[mode] : 0u;
}

void host_radio_pair_stop_reset(void)
{
    s_pair_stop_cnt[0] = 0u;
    s_pair_stop_cnt[1] = 0u;
    s_pair_stop_cnt[2] = 0u;
}

int port_radio_connect_profile(uint8_t mode, uint8_t profile)
{
    (void)profile;
    if (mode < 3u) {
        s_radio_conn[mode] = true;
    }
    return BRIDGE_OK;
}

static uint32_t s_unpair_cnt[3];   /* 下标 = BRIDGE_RADIO_*（0/1/2） */

int port_radio_unpair(uint8_t mode, uint8_t profile)
{
    if (mode < 3u) {
        s_unpair_cnt[mode]++;
    }
    (void)profile;
    return BRIDGE_OK;
}

uint32_t host_radio_unpair_count(uint8_t mode)
{
    return (mode < 3u) ? s_unpair_cnt[mode] : 0u;
}

void host_radio_unpair_reset(void)
{
    s_unpair_cnt[0] = 0u;
    s_unpair_cnt[1] = 0u;
    s_unpair_cnt[2] = 0u;
}

int port_radio_set_report_period_ms(uint8_t mode, uint16_t ms)
{
    (void)mode; (void)ms;
    return BRIDGE_OK;
}

/* 建链取证快照：主机侧桩返回全 0（诊断仅用于真机取证）。
 * 长度须与真机一致 = 42B（24B 基础 + 18B UART 接收腿取证），否则 core 侧
 * handle_get_diag 以 42B 布局转发时字段会错位。 */
int port_radio_diag(uint8_t *out, uint8_t *out_len)
{
    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }
    memset(out, 0, 42u);
    *out_len = 42u;
    return BRIDGE_OK;
}

/* BLE 多主机槽状态：主机侧桩返回"4 槽全空、无活跃"（core 侧仅验证协议封装） */
int port_ble_slots(uint8_t out[5])
{
    if (out == 0) {
        return BRIDGE_ERR_PARAM;
    }
    memset(out, 0, 5u);
    return BRIDGE_OK;
}

/* BLE HID 服务注册取证：主机侧桩返回"未调用(0xFF) + 句柄全 0"（仅真机取证） */
int port_hid_diag(uint8_t *out, uint8_t *out_len)
{
    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }
    memset(out, 0, 12u);
    out[0] = 0xFFu;
    *out_len = 12u;
    return BRIDGE_OK;
}

/* ---------------- 透传通道（借鉴市售模组 0x88/0x89） ---------------- */
uint8_t  g_host_tunnel[HOST_TUNNEL_BUF];
uint16_t g_host_tunnel_len;
uint8_t  g_host_tunnel_kind;

void host_tunnel_reset(void)
{
    g_host_tunnel_len  = 0u;
    g_host_tunnel_kind = 0u;
}

int port_tunnel_send(uint8_t kind, const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    g_host_tunnel_kind = kind;
    g_host_tunnel_len  = (len > HOST_TUNNEL_BUF) ? HOST_TUNNEL_BUF : len;
    memset(g_host_tunnel, 0, sizeof(g_host_tunnel));
    memcpy(g_host_tunnel, payload, g_host_tunnel_len);
    return BRIDGE_OK;
}

/* ---------------- 名称 / USB 标识 / 配置中继（CMD_SET_NAME / CMD_SET_USB_ID） ----------------
 * 主机模型按角色**复刻**目标侧 bridge_radio_ch58x.c 的支持矩阵：
 *   键盘侧：BLE 名本地生效，USB 名 / VID-PID 返回 UNSUPPORTED → core 走 2.4G 中继
 *   Dongle：USB 名 / VID-PID 本地生效，BLE 名返回 UNSUPPORTED
 * 这样 core 的"本地生效 vs 中继"分支才能在主机上被真实区分。 */
#define HOST_NAME_BUF 32u
static uint8_t  s_name_buf[HOST_NAME_BUF];
static uint8_t  s_name_len;
static int      s_name_target = -1;
static int      s_usb_vid = -1;
static int      s_usb_pid = -1;
static uint8_t  s_cfg_buf[BRIDGE_MAX_PAYLOAD + 1u];
static uint8_t  s_cfg_len;
static bool     s_cfg_set;

void host_name_reset(void)
{
    s_name_len    = 0u;
    s_name_target = -1;
    s_usb_vid     = -1;
    s_usb_pid     = -1;
    s_cfg_len     = 0u;
    s_cfg_set     = false;
}

int host_name_target(void) { return s_name_target; }

const uint8_t *host_name(uint8_t *len)
{
    if (s_name_target < 0) {
        if (len != 0) { *len = 0u; }
        return 0;
    }
    if (len != 0) { *len = s_name_len; }
    return s_name_buf;
}

int host_usb_vid(void)        { return s_usb_vid; }
int host_usb_pid(void)        { return s_usb_pid; }
int host_cfg_relay_len(void)  { return s_cfg_set ? (int)s_cfg_len : -1; }

const uint8_t *host_cfg_relay(uint8_t *len)
{
    if (!s_cfg_set) {
        if (len != 0) { *len = 0u; }
        return 0;
    }
    if (len != 0) { *len = s_cfg_len; }
    return s_cfg_buf;
}

bool port_is_dongle(void) { return BRIDGE_BOARD_IS_DONGLE ? true : false; }

int port_set_name(uint8_t target, const uint8_t *name, uint8_t len)
{
    uint8_t n = 0u;

    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
#if BRIDGE_BOARD_IS_DONGLE
    if (target != BRIDGE_NAME_TARGET_USB) {
        return BRIDGE_ERR_UNSUPPORTED;
    }
#else
    if (target != BRIDGE_NAME_TARGET_BLE) {
        return BRIDGE_ERR_UNSUPPORTED;
    }
#endif
    while ((n < len) && (n < HOST_NAME_BUF)) {
        s_name_buf[n] = name[n];
        n++;
    }
    s_name_len    = n;
    s_name_target = (int)target;
    return BRIDGE_OK;
}

int port_set_usb_ids(uint16_t vid, uint16_t pid)
{
#if BRIDGE_BOARD_IS_DONGLE
    s_usb_vid = (int)vid;
    s_usb_pid = (int)pid;
    return BRIDGE_OK;
#else
    (void)vid;
    (void)pid;
    return BRIDGE_ERR_UNSUPPORTED;      /* 键盘侧无 USB：core 会中继给 Dongle */
#endif
}

int port_radio_cfg_send(const uint8_t *frame, uint8_t len)
{
    if (frame == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    s_cfg_len = (len > (uint8_t)sizeof(s_cfg_buf)) ? (uint8_t)sizeof(s_cfg_buf) : len;
    memcpy(s_cfg_buf, frame, s_cfg_len);
    s_cfg_set = true;
    return BRIDGE_OK;
}

/* ---------------- 芯片 / SRRC ---------------- */
int port_chip_get_mac(uint8_t mac[6])
{
    static const uint8_t k_mac[6] = { 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0xCDu };
    memcpy(mac, k_mac, 6u);
    return BRIDGE_OK;
}

int port_radio_test_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
    (void)mode; (void)power;
    s_test_channel = channel;
    return BRIDGE_OK;
}

/* ---------------- 日志 ---------------- */
uint16_t g_host_log_lines;
static char s_logbuf[65536];
static uint32_t s_logbuf_len;

void host_log_reset(void) { g_host_log_lines = 0u; s_logbuf_len = 0u; }

void port_log_write(const char *s, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        if (s_logbuf_len + 1u < sizeof(s_logbuf)) {
            s_logbuf[s_logbuf_len++] = s[i];
        }
        if (s[i] == '\n') {
            g_host_log_lines++;
        }
    }
    if (s_logbuf_len < sizeof(s_logbuf)) {
        s_logbuf[s_logbuf_len] = '\0';
    }
}

/* ---------------- GPIO ---------------- */
static uint8_t  s_mode_sel;
static bool     s_vbus;
static bool     s_wake;
static bool     s_link;
static uint16_t s_bat_mv = 3800u;
static bool     s_charger;

void host_set_vbus(bool present) { s_vbus = present; }
void host_set_mode_sel(uint8_t sel) { s_mode_sel = (uint8_t)(sel & 0x03u); }
void host_set_bat_mv(uint16_t mv) { s_bat_mv = mv; }
void host_set_charger(bool active) { s_charger = active; }
uint32_t host_reset_count(void) { return s_reset_count; }
uint32_t host_bootloader_count(void) { return s_boot_count; }

void port_gpio_init(void) { }
uint8_t port_gpio_read_mode_sel(void) { return s_mode_sel; }
void port_gpio_set_wake(bool active) { s_wake = active; (void)s_wake; }
void port_gpio_set_link(bool connected) { s_link = connected; (void)s_link; }

uint16_t port_bat_read_mv(void) { return s_bat_mv; }
bool     port_vbus_present(void) { return s_vbus; }
bool     port_charger_active(void) { return s_charger; }

/* ---------------- 系统 ---------------- */
void port_enter_bootloader(void) { s_boot_count++; }
void port_reset(void) { s_reset_count++; }
void port_on_leds(uint8_t led_state) { (void)led_state; }

/* ---------------- 固件升级（port_fw_* 抓包） ---------------- */
#define HOST_FW_BUF  1024u

static bool     s_fw_active;
static uint32_t s_fw_total;
static uint32_t s_fw_written;
static uint16_t s_fw_crc;
static uint32_t s_fw_finish_cnt;
static uint32_t s_fw_abort_cnt;
static uint8_t  s_fw_buf[HOST_FW_BUF];
static uint32_t s_fw_buf_len;

void host_fw_reset(void)
{
    s_fw_active = false;
    s_fw_total = 0u;
    s_fw_written = 0u;
    s_fw_crc = 0u;
    s_fw_finish_cnt = 0u;
    s_fw_abort_cnt = 0u;
    s_fw_buf_len = 0u;
}

uint32_t host_fw_total(void) { return s_fw_total; }
uint32_t host_fw_written(void) { return s_fw_written; }
uint16_t host_fw_finish_crc(void) { return s_fw_crc; }
uint32_t host_fw_finish_count(void) { return s_fw_finish_cnt; }
uint32_t host_fw_abort_count(void) { return s_fw_abort_cnt; }

const uint8_t *host_fw_data(uint32_t *len)
{
    if (len) {
        *len = s_fw_buf_len;
    }
    return s_fw_buf;
}

int port_fw_begin(uint32_t total_len)
{
    if (total_len == 0u || total_len > BRIDGE_FW_MAX_LEN) {
        return BRIDGE_ERR_PARAM;
    }
    s_fw_active = true;
    s_fw_total = total_len;
    s_fw_written = 0u;
    s_fw_buf_len = 0u;
    return BRIDGE_OK;
}

int port_fw_write(uint32_t offset, const uint8_t *d, uint16_t len)
{
    if (!s_fw_active) {
        return BRIDGE_ERR_STATE;
    }
    if (offset != s_fw_written || offset + len > s_fw_total) {
        return BRIDGE_ERR_PARAM;
    }
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_fw_buf_len + len <= HOST_FW_BUF) {
        memcpy(&s_fw_buf[s_fw_buf_len], d, len);
        s_fw_buf_len += len;
    }
    s_fw_written += len;
    return BRIDGE_OK;
}

void port_fw_finish(uint16_t crc)
{
    s_fw_crc = crc;
    s_fw_finish_cnt++;
    s_fw_active = false;        /* 目标侧此处复位不复返；主机侧返回以便断言 */
}

void port_fw_abort(void)
{
    s_fw_abort_cnt++;
    s_fw_active = false;
}

/* ---------------- TX 抓包解析 ---------------- */
int host_tx_find_frame(uint8_t cmd, uint8_t *out_flags,
                       uint8_t *out_payload, uint8_t *out_len)
{
    for (uint16_t i = 0u; i + 6u <= g_host_tx_len; i++) {
        if (g_host_tx[i] != BRIDGE_SOF0 || g_host_tx[i + 1u] != BRIDGE_SOF1) {
            continue;
        }
        uint8_t len  = g_host_tx[i + 2u];
        if (i + 2u + BRIDGE_HDR_LEN + len + BRIDGE_CRC_LEN > g_host_tx_len) {
            break;
        }
        uint8_t c    = g_host_tx[i + 3u];
        uint8_t seq  = g_host_tx[i + 4u];
        uint8_t fl   = g_host_tx[i + 5u];
        (void)seq;
        uint16_t crc = bridge_crc16_ccitt(&g_host_tx[i + 2u],
                                          (uint16_t)(BRIDGE_HDR_LEN + len));
        uint16_t rx  = (uint16_t)(g_host_tx[i + 2u + BRIDGE_HDR_LEN + len] |
                                  ((uint16_t)g_host_tx[i + 3u + BRIDGE_HDR_LEN + len] << 8));
        if (crc != rx) {
            continue;
        }
        if (c == cmd) {
            if (out_flags) { *out_flags = fl; }
            if (out_len)   { *out_len = len; }
            if (out_payload && len > 0u) {
                memcpy(out_payload, &g_host_tx[i + 6u], len);
            }
            return 1;
        }
        i = (uint16_t)(i + 2u + BRIDGE_HDR_LEN + len + BRIDGE_CRC_LEN - 1u);
    }
    return 0;
}

uint16_t host_tx_frame_count(void)
{
    uint16_t n = 0u;
    for (uint16_t i = 0u; i + 6u <= g_host_tx_len; i++) {
        if (g_host_tx[i] != BRIDGE_SOF0 || g_host_tx[i + 1u] != BRIDGE_SOF1) {
            continue;
        }
        uint8_t len = g_host_tx[i + 2u];
        if (i + 2u + BRIDGE_HDR_LEN + len + BRIDGE_CRC_LEN > g_host_tx_len) {
            break;
        }
        uint16_t crc = bridge_crc16_ccitt(&g_host_tx[i + 2u],
                                          (uint16_t)(BRIDGE_HDR_LEN + len));
        uint16_t rx  = (uint16_t)(g_host_tx[i + 2u + BRIDGE_HDR_LEN + len] |
                                  ((uint16_t)g_host_tx[i + 3u + BRIDGE_HDR_LEN + len] << 8));
        if (crc == rx) {
            n++;
            i = (uint16_t)(i + 2u + BRIDGE_HDR_LEN + len + BRIDGE_CRC_LEN - 1u);
        }
    }
    return n;
}
