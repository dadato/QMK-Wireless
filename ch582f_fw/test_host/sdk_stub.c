/* 主机端 SDK 桩实现：bridge_ch58x_sdk.h 全量原语的内存模型
 *
 * 详见 sdk_stub.h 顶部的保真约定。本文件不参与目标侧构建。
 */
#include <string.h>

#include "bridge_ch58x_sdk.h"
#include "bridge_config.h"
#include "bridge_port.h"
#include "bridge_types.h"
#include "sdk_stub.h"

/* 端口索引：'A' → 0，其余（'B'）→ 1 */
static uint8_t pidx(uint8_t port)
{
    return (port == 'B') ? 1u : 0u;
}

/* 抓包：写入时先清零长度 */
static void record_tx(uint8_t *buf, uint16_t *len, const uint8_t *d, uint8_t n)
{
    *len = 0u;
    for (uint8_t i = 0u; (i < n) && (*len < STUB_RADIO_BUF); i++) {
        buf[(*len)++] = d[i];
    }
}

/* ============================ 时间 ============================ */

static uint32_t s_ms;

void     stub_advance_ms(uint32_t dt) { s_ms += dt; }
uint32_t stub_now_ms(void)            { return s_ms; }

/* ============================ UART1 ============================ */

static uint8_t  s_u1_tx[STUB_FIFO_DEPTH];
static uint8_t  s_u1_tx_len;
static uint8_t  s_u1_rx[STUB_FIFO_DEPTH];
static uint8_t  s_u1_rx_len;
static bool     s_u1_inited;
static uint32_t s_u1_baud;
static bool     s_u1_rx_irq;
static uint32_t s_u1_rx_drop;
static uint32_t s_u1_rx_reads;
static uint32_t s_u1_iir_reads;

uint8_t  g_stub_u1_tx[STUB_TX_BUF_SIZE];
uint16_t g_stub_u1_tx_len;
uint32_t g_stub_u1_tx_drop;

void stub_u1_tx_reset(void)
{
    g_stub_u1_tx_len  = 0u;
    g_stub_u1_tx_drop = 0u;
}

void stub_u1_tx_drain(void)
{
    for (uint8_t i = 0u; i < s_u1_tx_len; i++) {
        if (g_stub_u1_tx_len < STUB_TX_BUF_SIZE) {
            g_stub_u1_tx[g_stub_u1_tx_len++] = s_u1_tx[i];
        }
    }
    s_u1_tx_len = 0u;
}

void stub_u1_rx_push(const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        if (s_u1_rx_len < (uint8_t)STUB_FIFO_DEPTH) {
            s_u1_rx[s_u1_rx_len++] = d[i];
        } else {
            s_u1_rx_drop++;     /* 硬件 FIFO 满：后续字节丢失 */
        }
    }
}

uint32_t stub_u1_rx_drop(void)   { return s_u1_rx_drop; }
uint32_t stub_u1_rx_reads(void)  { return s_u1_rx_reads; }
uint32_t stub_u1_iir_reads(void) { return s_u1_iir_reads; }
bool     stub_u1_inited(void)    { return s_u1_inited; }
uint32_t stub_u1_baud(void)      { return s_u1_baud; }
bool     stub_u1_rx_irq(void)    { return s_u1_rx_irq; }
uint8_t  sdk_uart1_lsr(void)     { return 0x60u; }   /* TX 全空、无错误位 */

void sdk_uart1_init(uint32_t baud)
{
    s_u1_inited = true;
    s_u1_baud   = baud;
    s_u1_tx_len = 0u;
    s_u1_rx_len = 0u;
}

void sdk_uart1_rx_irq_enable(bool en)
{
    s_u1_rx_irq = en;
}

void sdk_uart1_write(const uint8_t *d, uint16_t len)
{
    /* 真实 UART1_SendString 满则等待；port 层已按 FIFO 余量分块，
     * 故此处溢出一律视为 port 层缺陷，计入 drop 由测试断言 */
    for (uint16_t i = 0u; i < len; i++) {
        if (s_u1_tx_len < (uint8_t)STUB_FIFO_DEPTH) {
            s_u1_tx[s_u1_tx_len++] = d[i];
        } else {
            g_stub_u1_tx_drop++;
        }
    }
}

uint8_t sdk_uart1_tx_bytes(void) { return s_u1_tx_len; }
uint8_t sdk_uart1_rx_bytes(void) { return s_u1_rx_len; }

uint8_t sdk_uart1_rx_read(void)
{
    uint8_t b = 0u;

    if (s_u1_rx_len > 0u) {
        b = s_u1_rx[0];
        s_u1_rx_len--;
        memmove(s_u1_rx, &s_u1_rx[1], s_u1_rx_len);
        s_u1_rx_reads++;
    }
    return b;
}

uint8_t sdk_uart1_iir(void)
{
    s_u1_iir_reads++;
    /* 只区分"有待读数据 / 无"，port 层不依赖具体中断源 */
    return (s_u1_rx_len > 0u) ? 0x04u : 0x01u;
}

/* ============================ UART0 ============================ */

static uint32_t s_u0_init_count;
static uint32_t s_u0_baud;

uint8_t  g_stub_u0_tx[STUB_LOG_BUF_SIZE];
uint16_t g_stub_u0_tx_len;

uint32_t stub_u0_init_count(void) { return s_u0_init_count; }
uint32_t stub_u0_baud(void)       { return s_u0_baud; }

void stub_u0_tx_reset(void)
{
    g_stub_u0_tx_len = 0u;
    g_stub_u0_tx[0]  = '\0';
}

void sdk_uart0_init(uint32_t baud)
{
    s_u0_init_count++;
    s_u0_baud = baud;
}

void sdk_uart0_write(const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        if (g_stub_u0_tx_len < (uint16_t)(STUB_LOG_BUF_SIZE - 1u)) {
            g_stub_u0_tx[g_stub_u0_tx_len++] = d[i];
        }
    }
    g_stub_u0_tx[g_stub_u0_tx_len] = '\0';
}

/* ============================ Flash / DataFlash ============================ */

static uint8_t  s_flash[STUB_FLASH_SIZE];
static bool     s_flash_fail_w;
static bool     s_flash_fail_e;
static uint32_t s_flash_erase_cnt;
static uint32_t s_flash_write_cnt;

static bool flash_addr_ok(uint32_t addr, uint16_t len)
{
    if (addr < SDK_DATAFLASH_BASE) {
        return false;
    }
    uint32_t off = addr - SDK_DATAFLASH_BASE;
    return ((uint32_t)len <= (uint32_t)STUB_FLASH_SIZE) &&
           (off <= (uint32_t)(STUB_FLASH_SIZE - len));
}

void stub_flash_clear(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_flash_fail_w   = false;
    s_flash_fail_e   = false;
    s_flash_erase_cnt = 0u;
    s_flash_write_cnt = 0u;
}

void stub_flash_fail_write(bool fail) { s_flash_fail_w = fail; }
void stub_flash_fail_erase(bool fail) { s_flash_fail_e = fail; }
uint32_t stub_flash_erase_count(void) { return s_flash_erase_cnt; }
uint32_t stub_flash_write_count(void) { return s_flash_write_cnt; }

int stub_flash_peek(uint32_t addr, void *buf, uint16_t len)
{
    if (buf == 0 || !flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_PARAM;
    }
    memcpy(buf, &s_flash[addr - SDK_DATAFLASH_BASE], len);
    return BRIDGE_OK;
}

void stub_flash_poke(uint32_t addr, const uint8_t *d, uint16_t len)
{
    if (d == 0 || !flash_addr_ok(addr, len)) {
        return;
    }
    memcpy(&s_flash[addr - SDK_DATAFLASH_BASE], d, len);
}

int sdk_flash_read(uint32_t addr, void *buf, uint16_t len)
{
    if (buf == 0 || !flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_IO;
    }
    memcpy(buf, &s_flash[addr - SDK_DATAFLASH_BASE], len);
    return BRIDGE_OK;
}

int sdk_flash_write(uint32_t addr, const void *buf, uint16_t len)
{
    if (buf == 0 || !flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_IO;
    }
    if (s_flash_fail_w) {
        return BRIDGE_ERR_IO;
    }
    s_flash_write_cnt++;

    const uint8_t *d = (const uint8_t *)buf;
    uint32_t       o = addr - SDK_DATAFLASH_BASE;
    /* NOR 语义：写只清位（未擦除即写会得到 AND 结果，无法"恢复"位） */
    for (uint16_t i = 0u; i < len; i++) {
        s_flash[o + i] &= d[i];
    }
    return BRIDGE_OK;
}

int sdk_flash_erase(uint32_t addr, uint16_t len)
{
    if (!flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_IO;
    }
    if ((addr % SDK_FLASH_ERASE_UNIT) != 0u || (len % SDK_FLASH_ERASE_UNIT) != 0u) {
        return BRIDGE_ERR_IO;       /* 擦除必须 256B 对齐 */
    }
    if (s_flash_fail_e) {
        return BRIDGE_ERR_IO;
    }
    s_flash_erase_cnt++;
    memset(&s_flash[addr - SDK_DATAFLASH_BASE], 0xFF, len);
    return BRIDGE_OK;
}

/* ============================ GPIO ============================ */

static bool    s_gpio_in[2][32];
static bool    s_gpio_out[2][32];
static uint8_t s_gpio_mode[2][32];

void stub_gpio_set_all_in(bool level)
{
    for (uint8_t p = 0u; p < 2u; p++) {
        for (uint8_t i = 0u; i < 32u; i++) {
            s_gpio_in[p][i] = level;
        }
    }
}

void stub_gpio_set_in(uint8_t port, uint8_t pin, bool level)
{
    if (pin < 32u) {
        s_gpio_in[pidx(port)][pin] = level;
    }
}

bool stub_gpio_get_out(uint8_t port, uint8_t pin)
{
    return (pin < 32u) && s_gpio_out[pidx(port)][pin];
}

uint8_t stub_gpio_mode(uint8_t port, uint8_t pin)
{
    return (pin < 32u) ? s_gpio_mode[pidx(port)][pin] : 0xFFu;
}

void sdk_gpio_out_init(uint8_t port, uint8_t pin, bool high)
{
    if (pin >= 32u) {
        return;
    }
    s_gpio_mode[pidx(port)][pin] = STUB_GPIO_OUT;
    s_gpio_out[pidx(port)][pin]  = high;
}

void sdk_gpio_in_pullup(uint8_t port, uint8_t pin)
{
    if (pin < 32u) {
        s_gpio_mode[pidx(port)][pin] = STUB_GPIO_IN_PU;
    }
}

void sdk_gpio_in_pulldown(uint8_t port, uint8_t pin)
{
    if (pin < 32u) {
        s_gpio_mode[pidx(port)][pin] = STUB_GPIO_IN_PD;
    }
}

void sdk_gpio_adc_in(uint8_t port, uint8_t pin)
{
    if (pin < 32u) {
        s_gpio_mode[pidx(port)][pin] = STUB_GPIO_IN_FLOAT;
    }
}

void sdk_gpio_write(uint8_t port, uint8_t pin, bool high)
{
    if (pin < 32u) {
        s_gpio_out[pidx(port)][pin] = high;
    }
}

bool sdk_gpio_read(uint8_t port, uint8_t pin)
{
    if (pin >= 32u) {
        return false;
    }
    uint8_t p = pidx(port);
    return (s_gpio_mode[p][pin] == STUB_GPIO_OUT) ? s_gpio_out[p][pin]
                                                 : s_gpio_in[p][pin];
}

/* ============================ ADC ============================ */

static uint16_t s_adc_mv[16];
static uint32_t s_adc_reads;

void stub_adc_set_mv(uint8_t ch, uint16_t mv)
{
    if (ch < 16u) {
        s_adc_mv[ch] = mv;
    }
}

uint32_t stub_adc_reads(void) { return s_adc_reads; }

uint16_t sdk_adc_read_mv(uint8_t ch)
{
    s_adc_reads++;
    return (ch < 16u) ? s_adc_mv[ch] : 0u;
}

/* ============================ 射频 ============================ */

static bool     s_ble_up;
static bool     s_ble_conn;
static uint32_t s_ble_init_cnt;
static uint32_t s_ble_deinit_cnt;
static uint32_t s_ble_pair_cnt;
static uint32_t s_ble_pair_stop_cnt;
static int      s_ble_profile;
static int      s_ble_adv;
static int      s_ble_units;
static int      s_ble_test_ch;
static int      s_ble_unpair;

static bool     s_rf24_up;
static bool     s_rf24_conn;
static bool     s_rf24_fail_init;
static bool     s_rf24_fail_send;
static uint32_t s_rf24_init_cnt;
static uint32_t s_rf24_deinit_cnt;
static uint32_t s_rf24_pair_cnt;
static uint32_t s_rf24_pair_stop_cnt;
static uint32_t s_rf24_unpair_cnt;
static int      s_rf24_period;

static bool     s_usb_up;
static bool     s_usb_configured;
static bool     s_usb_fail_send;
static uint32_t s_usb_init_cnt;

/* 名称 / USB 标识（CMD_SET_NAME / CMD_SET_USB_ID）：仅记录，供测试断言 */
#define STUB_NAME_MAX 32u
static uint8_t  s_ble_adv_name[STUB_NAME_MAX];
static uint8_t  s_ble_adv_name_len;
static uint8_t  s_usb_prod_name[STUB_NAME_MAX];
static uint8_t  s_usb_prod_name_len;
static int      s_usb_vid = -1;
static int      s_usb_pid = -1;
static int      s_rf24_cfg_len = -1;

uint8_t  g_stub_ble_tx[STUB_RADIO_BUF];
uint16_t g_stub_ble_tx_len;
uint8_t  g_stub_ble_tx_rid;
uint8_t  g_stub_rf24_tx[STUB_RADIO_BUF];
uint16_t g_stub_rf24_tx_len;
uint8_t  g_stub_usb_tx[STUB_RADIO_BUF];
uint16_t g_stub_usb_tx_len;

void stub_radio_obs_reset(void)
{
    g_stub_ble_tx_len  = 0u;
    g_stub_ble_tx_rid  = 0u;
    g_stub_rf24_tx_len = 0u;
    g_stub_usb_tx_len  = 0u;
    s_ble_init_cnt     = 0u;
    s_ble_deinit_cnt   = 0u;
    s_ble_pair_cnt     = 0u;
    s_ble_pair_stop_cnt = 0u;
    s_ble_profile      = -1;
    s_ble_adv          = -1;
    s_ble_units        = -1;
    s_ble_test_ch      = -1;
    s_ble_unpair       = -1;
    s_rf24_init_cnt    = 0u;
    s_rf24_deinit_cnt  = 0u;
    s_rf24_pair_cnt    = 0u;
    s_rf24_pair_stop_cnt = 0u;
    s_rf24_unpair_cnt  = 0u;
    s_rf24_period      = -1;
    s_usb_init_cnt     = 0u;
    s_rf24_fail_init   = false;
    s_rf24_fail_send   = false;
    s_usb_fail_send    = false;
    s_ble_adv_name_len  = 0u;
    s_usb_prod_name_len = 0u;
    s_usb_vid           = -1;
    s_usb_pid           = -1;
    s_rf24_cfg_len      = -1;
}

void stub_radio_clear_links(void)
{
    s_ble_up     = false;
    s_ble_conn   = false;
    s_rf24_up    = false;
    s_rf24_conn  = false;
    s_usb_up     = false;
    s_usb_configured = false;
}

void stub_ble_set_connected(bool c)  { s_ble_conn = c; }
uint32_t stub_ble_init_count(void)   { return s_ble_init_cnt; }
uint32_t stub_ble_deinit_count(void) { return s_ble_deinit_cnt; }
uint32_t stub_ble_pair_count(void)   { return s_ble_pair_cnt; }
uint32_t stub_ble_pair_stop_count(void) { return s_ble_pair_stop_cnt; }
int  stub_ble_profile(void)          { return s_ble_profile; }
int  stub_ble_adv_name_mac(void)     { return s_ble_adv; }
int  stub_ble_interval_units(void)   { return s_ble_units; }
int  stub_ble_test_ch(void)          { return s_ble_test_ch; }
int  stub_ble_unpair_profile(void)   { return s_ble_unpair; }

const uint8_t *stub_ble_adv_name(uint8_t *len)
{
    if (s_ble_adv_name_len == 0u) {
        if (len != 0) { *len = 0u; }
        return 0;
    }
    if (len != 0) { *len = s_ble_adv_name_len; }
    return s_ble_adv_name;
}

const uint8_t *stub_usb_prod_name(uint8_t *len)
{
    if (s_usb_prod_name_len == 0u) {
        if (len != 0) { *len = 0u; }
        return 0;
    }
    if (len != 0) { *len = s_usb_prod_name_len; }
    return s_usb_prod_name;
}

int stub_usb_vid(void)        { return s_usb_vid; }
int stub_usb_pid(void)        { return s_usb_pid; }
int stub_rf24_cfg_len(void)   { return s_rf24_cfg_len; }

void stub_rf24_fail_init(bool fail)  { s_rf24_fail_init = fail; }
void stub_rf24_fail_send(bool fail)  { s_rf24_fail_send = fail; }
void stub_rf24_set_connected(bool c) { s_rf24_conn = c; }
uint32_t stub_rf24_init_count(void)  { return s_rf24_init_cnt; }
uint32_t stub_rf24_deinit_count(void){ return s_rf24_deinit_cnt; }
uint32_t stub_rf24_pair_count(void)  { return s_rf24_pair_cnt; }
uint32_t stub_rf24_pair_stop_count(void) { return s_rf24_pair_stop_cnt; }
uint32_t stub_rf24_unpair_count(void){ return s_rf24_unpair_cnt; }
int  stub_rf24_period_ms(void)       { return s_rf24_period; }

void stub_usb_set_configured(bool c) { s_usb_configured = c; }
void stub_usb_fail_send(bool fail)   { s_usb_fail_send = fail; }
uint32_t stub_usb_init_count(void)   { return s_usb_init_cnt; }

int sdk_ble_stack_init(void)
{
    s_ble_init_cnt++;
    s_ble_up = true;
    return BRIDGE_OK;
}

int sdk_ble_deinit(void)
{
    s_ble_deinit_cnt++;
    s_ble_conn = false;
    return BRIDGE_OK;
}

int sdk_ble_hid_send(uint8_t rid, const uint8_t *d, uint8_t len)
{
    if (!s_ble_up || !s_ble_conn) {
        return BRIDGE_ERR_STATE;
    }
    g_stub_ble_tx_rid = rid;
    record_tx(g_stub_ble_tx, &g_stub_ble_tx_len, d, len);
    return BRIDGE_OK;
}

bool sdk_ble_is_connected(void) { return s_ble_up && s_ble_conn; }

int sdk_ble_pair_start(uint16_t timeout_s)
{
    (void)timeout_s;
    s_ble_pair_cnt++;
    return BRIDGE_OK;
}

int sdk_ble_pair_stop(void)
{
    s_ble_pair_stop_cnt++;
    return BRIDGE_OK;
}

int sdk_ble_connect_profile(uint8_t profile)
{
    if (profile < 1u || profile > 4u) {
        return BRIDGE_ERR_PARAM;
    }
    s_ble_profile = (int)profile;
    return BRIDGE_OK;
}

int sdk_ble_unpair(uint8_t profile)
{
    s_ble_unpair = (int)profile;
    return BRIDGE_OK;
}

int sdk_ble_set_conn_interval(uint8_t units_1p25ms)
{
    s_ble_units = (int)units_1p25ms;
    return BRIDGE_OK;
}

int sdk_ble_set_adv_name_with_mac(bool en)
{
    s_ble_adv = en ? 1 : 0;
    return BRIDGE_OK;
}

int sdk_ble_set_adv_name(const uint8_t *name, uint8_t len)
{
    uint8_t n = 0u;

    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    while ((n < len) && (n < STUB_NAME_MAX)) {
        s_ble_adv_name[n] = name[n];
        n++;
    }
    s_ble_adv_name_len = n;
    return BRIDGE_OK;
}

int sdk_ble_test_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
    (void)mode;
    (void)power;
    s_ble_test_ch = (int)channel;
    return BRIDGE_OK;
}

uint8_t sdk_ble_protocol_ver(void) { return 1u; }

int sdk_rf24_init(void)
{
    if (s_rf24_fail_init) {
        return BRIDGE_ERR_IO;
    }
    s_rf24_init_cnt++;
    s_rf24_up   = true;
    s_rf24_conn = false;
    return BRIDGE_OK;
}

int sdk_rf24_deinit(void)
{
    s_rf24_deinit_cnt++;
    s_rf24_up   = false;
    s_rf24_conn = false;
    return BRIDGE_OK;
}

int sdk_rf24_send(const uint8_t *d, uint8_t len)
{
    if (!s_rf24_up || !s_rf24_conn) {
        return BRIDGE_ERR_STATE;
    }
    if (s_rf24_fail_send) {
        return BRIDGE_ERR_IO;
    }
    record_tx(g_stub_rf24_tx, &g_stub_rf24_tx_len, d, len);
    return BRIDGE_OK;
}

bool sdk_rf24_is_connected(void) { return s_rf24_up && s_rf24_conn; }

int sdk_rf24_pair_start(void)
{
    s_rf24_pair_cnt++;
    return BRIDGE_OK;
}

int sdk_rf24_pair_stop(void)
{
    s_rf24_pair_stop_cnt++;
    return BRIDGE_OK;
}

int sdk_rf24_unpair(void)
{
    s_rf24_unpair_cnt++;
    return BRIDGE_OK;
}

int sdk_rf24_send_pair(const uint8_t *d, uint8_t len)
{
    /* 配对应答帧（Dongle → 键盘）：与普通发送同记录，供用例断言载荷 */
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    record_tx(g_stub_rf24_tx, &g_stub_rf24_tx_len, d, len);
    return BRIDGE_OK;
}

int sdk_rf24_send_host(uint8_t state)
{
    /* 主机电源状态（Dongle → 键盘）：记录 [state]（不含线上类型前缀），
     * 与 sdk_rf24_send / send_pair 的记录约定一致。 */
    if (state != BRIDGE_HOST_AWAKE && state != BRIDGE_HOST_SUSPEND) {
        return BRIDGE_ERR_PARAM;
    }
    record_tx(g_stub_rf24_tx, &g_stub_rf24_tx_len, &state, 1u);
    return BRIDGE_OK;
}

int sdk_rf24_set_report_period_ms(uint16_t ms)
{
    s_rf24_period = (int)ms;
    return BRIDGE_OK;
}

uint8_t sdk_rf24_protocol_ver(void) { return 2u; }

int sdk_rf24_diag(uint8_t *out, uint8_t *out_len)
{
    /* 建链取证快照（24B，布局见 bridge_ch58x_sdk.h）。
     * 主机端未建模 diag 计数器，按 port_host.c 的同名约定返回全 0，
     * 仅置长度；诊断数据仅供真机取证。 */
    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }
    memset(out, 0, 24u);
    *out_len = 24u;
    return BRIDGE_OK;
}

int sdk_usb_init(void)
{
    s_usb_init_cnt++;
    s_usb_up = true;
    return BRIDGE_OK;
}

int sdk_usb_hid_send(const uint8_t *d, uint8_t len)
{
    if (!s_usb_up || !s_usb_configured) {
        return BRIDGE_ERR_STATE;
    }
    if (s_usb_fail_send) {
        return BRIDGE_ERR_IO;
    }
    record_tx(g_stub_usb_tx, &g_stub_usb_tx_len, d, len);
    return BRIDGE_OK;
}

bool sdk_usb_is_configured(void) { return s_usb_configured; }

int sdk_usb_tunnel_send(const uint8_t *d, uint8_t len)
{
    /* Dongle 侧：透传经 USB Raw HID 送主机。与 HID 发送同门槛（已配置）。 */
    if (!s_usb_up || !s_usb_configured) {
        return BRIDGE_ERR_STATE;
    }
    if (s_usb_fail_send) {
        return BRIDGE_ERR_IO;
    }
    record_tx(g_stub_usb_tx, &g_stub_usb_tx_len, d, len);
    return BRIDGE_OK;
}

int sdk_usb_set_product_name(const uint8_t *name, uint8_t len)
{
    uint8_t n = 0u;

    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    while ((n < len) && (n < STUB_NAME_MAX)) {
        s_usb_prod_name[n] = name[n];
        n++;
    }
    s_usb_prod_name_len = n;
    return BRIDGE_OK;
}

int sdk_usb_set_ids(uint16_t vid, uint16_t pid)
{
    s_usb_vid = (int)vid;
    s_usb_pid = (int)pid;
    return BRIDGE_OK;
}

int sdk_rf24_send_tunnel(const uint8_t *d, uint8_t len)
{
    /* 键盘侧：透传经 2.4G 送 Dongle。载荷前置类型字节在真机 RF 层补齐，
     * 本桩只记录原始载荷供测试断言。 */
    if (!s_rf24_up || !s_rf24_conn) {
        return BRIDGE_ERR_STATE;
    }
    if (s_rf24_fail_send) {
        return BRIDGE_ERR_IO;
    }
    record_tx(g_stub_rf24_tx, &g_stub_rf24_tx_len, d, len);
    return BRIDGE_OK;
}

int sdk_rf24_send_cfg(const uint8_t *d, uint8_t len)
{
    /* 键盘侧：配置中继经 2.4G 送 Dongle。与透传同门槛（链路已连接）。 */
    if (!s_rf24_up || !s_rf24_conn) {
        return BRIDGE_ERR_STATE;
    }
    if (s_rf24_fail_send) {
        return BRIDGE_ERR_IO;
    }
    record_tx(g_stub_rf24_tx, &g_stub_rf24_tx_len, d, len);
    s_rf24_cfg_len = (int)len;
    return BRIDGE_OK;
}

int sdk_ble_tunnel_send(const uint8_t *d, uint8_t len)
{
    /* 键盘侧：透传经 BLE GATT 送手机 APP。需已连接。 */
    if (!s_ble_up || !s_ble_conn) {
        return BRIDGE_ERR_STATE;
    }
    record_tx(g_stub_ble_tx, &g_stub_ble_tx_len, d, len);
    return BRIDGE_OK;
}

/* BLE HID 服务注册取证（真机实现在 bridge_ble_hidsvc.c，此处为该边界文件的桩）：
 * 主机侧返回"未调用(0xFF) + 句柄全 0"，仅真机可取证。 */
int port_hid_diag(uint8_t *out, uint8_t *out_len)
{
    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }
    memset(out, 0, 14u);
    out[0] = 0xFFu;
    *out_len = 14u;
    return BRIDGE_OK;
}

/* ============================ 系统 ============================ */

static uint32_t s_sys_init_cnt;
static uint32_t s_reset_cnt;
static uint32_t s_boot_cnt;
static uint32_t s_tmos_cnt;
static uint32_t s_sleep_cnt;
static uint32_t s_sleep_last;
static uint8_t  s_ota_flag;
static uint32_t s_ota_flag_cnt;
static uint8_t  s_mac[6] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u };

uint32_t stub_sys_init_count(void)  { return s_sys_init_cnt; }
uint32_t stub_reset_count(void)     { return s_reset_cnt; }
uint32_t stub_boot_count(void)      { return s_boot_cnt; }
uint32_t stub_ota_flag_count(void)  { return s_ota_flag_cnt; }
uint8_t  stub_ota_flag(void)        { return s_ota_flag; }
uint32_t stub_tmos_count(void)      { return s_tmos_cnt; }
uint32_t stub_sleep_count(void)     { return s_sleep_cnt; }
uint32_t stub_sleep_last_ms(void)   { return s_sleep_last; }

void stub_ble_mac_set(const uint8_t mac[6])
{
    memcpy(s_mac, mac, 6u);
}

void sdk_sys_init(void)          { s_sys_init_cnt++; }
uint32_t sdk_sysclk_hz(void)     { return 60000000u; }
uint32_t sdk_millis(void)        { return s_ms; }
void sdk_delay_ms(uint32_t ms)   { s_ms += ms; }
void sdk_reset(void)             { s_reset_cnt++; }
void sdk_enter_bootloader(void)  { s_boot_cnt++; }

/* 复位源取证：主机侧桩固定返回 0（软件复位），仅真机取证。 */
static uint8_t s_last_reset_cause;
void    sdk_capture_reset_cause(void) { s_last_reset_cause = 0u; }
uint8_t sdk_last_reset_cause(void)    { return s_last_reset_cause; }

void sdk_ota_flag_set_reset(uint8_t flag)
{
    /* 真机：写 IAP 标志记录后 SYS_ResetExecute（不复返）。桩按同样副作用记账。 */
    s_ota_flag     = flag;
    s_ota_flag_cnt++;
    s_reset_cnt++;
}

void sdk_ble_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6u);
}

void sdk_sleep(uint32_t max_ms)
{
    s_sleep_cnt++;
    s_sleep_last = max_ms;
    s_ms += max_ms;         /* 低功耗期间时间照常流逝 */
}

/* ---- 看门狗桩：记录调用状态，不断言时序 ---- */
static uint32_t s_wdog_init_cnt;
static uint16_t s_wdog_ms;
static bool     s_wdog_susp;

void sdk_wdog_init(uint16_t timeout_ms)
{
    s_wdog_ms = timeout_ms;
    if (timeout_ms != 0u) {
        s_wdog_init_cnt++;
    }
    s_wdog_susp = false;
}

void sdk_wdog_feed(void)     { /* no-op */ }
void sdk_wdog_suspend(void)  { s_wdog_susp = true; }
void sdk_wdog_resume(void)   { s_wdog_susp = false; }

uint32_t stub_wdog_init_count(void) { return s_wdog_init_cnt; }
uint16_t stub_wdog_timeout_ms(void) { return s_wdog_ms; }
bool     stub_wdog_suspended(void)  { return s_wdog_susp; }

/* ---- RXD1 唤醒源开关桩（每次调用计数：睡前开 + 醒后关 = 2 次） ---- */
static uint32_t s_u1_wake_cnt;
static bool     s_u1_wake_on;

void sdk_uart1_rx_wake_enable(bool en)
{
    s_u1_wake_cnt++;
    s_u1_wake_on = en;
}

uint32_t stub_uart1_rx_wake_count(void) { return s_u1_wake_cnt; }
bool     stub_uart1_rx_wake_on(void)    { return s_u1_wake_on; }

/* ---- RXD1 唤醒痕迹（读清）：模拟睡眠窗口内有下行数据 ---- */
static bool s_u1_rx_woke;

bool sdk_uart1_rx_woke(void)
{
    bool woke = s_u1_rx_woke;
    s_u1_rx_woke = false;
    return woke;
}

void stub_uart1_rx_woke_set(bool v) { s_u1_rx_woke = v; }

/* ---- 固件升级桩（只记状态，不做擦写） ---- */
static bool     s_fw_active;
static uint32_t s_fw_total;
static uint32_t s_fw_written;
static uint32_t s_fw_finish_cnt;
static uint16_t s_fw_crc;

int sdk_fw_begin(uint32_t total_len)
{
    if (total_len == 0u || total_len > BRIDGE_FW_MAX_LEN) {
        return BRIDGE_ERR_PARAM;
    }
    s_fw_active = true;
    s_fw_total = total_len;
    s_fw_written = 0u;
    return BRIDGE_OK;
}

int sdk_fw_write(uint32_t offset, const uint8_t *d, uint16_t len)
{
    if (!s_fw_active) {
        return BRIDGE_ERR_STATE;
    }
    if (d == 0 || len == 0u || offset != s_fw_written || offset + len > s_fw_total) {
        return BRIDGE_ERR_PARAM;
    }
    s_fw_written += len;
    return BRIDGE_OK;
}

void sdk_fw_finish(uint16_t crc)
{
    s_fw_crc = crc;
    s_fw_finish_cnt++;
    s_fw_active = false;        /* 目标侧不复返；桩返回以便断言 */
}

void sdk_fw_abort(void)
{
    s_fw_active = false;
}

uint32_t stub_fw_finish_count(void) { return s_fw_finish_cnt; }
uint16_t stub_fw_finish_crc(void)   { return s_fw_crc; }

void sdk_tmos_process(void) { s_tmos_cnt++; }

/* ============================ 全量复位 ============================ */

void stub_reset(void)
{
    s_ms = 0u;

    s_u1_tx_len  = 0u;
    s_u1_rx_len  = 0u;
    s_u1_inited  = false;
    s_u1_baud    = 0u;
    s_u1_rx_irq  = false;
    s_u1_rx_drop = 0u;
    s_u1_rx_reads = 0u;
    s_u1_iir_reads = 0u;
    stub_u1_tx_reset();

    s_u0_init_count = 0u;
    s_u0_baud       = 0u;
    stub_u0_tx_reset();

    stub_flash_clear();

    stub_gpio_set_all_in(true);         /* 内部上拉：未接按键视为高 */
    for (uint8_t p = 0u; p < 2u; p++) {
        for (uint8_t i = 0u; i < 32u; i++) {
            s_gpio_mode[p][i] = STUB_GPIO_IN_PU;
            s_gpio_out[p][i]  = false;
        }
    }

    memset(s_adc_mv, 0, sizeof(s_adc_mv));
    s_adc_reads = 0u;

    stub_radio_obs_reset();
    stub_radio_clear_links();

    s_sys_init_cnt = 0u;
    s_reset_cnt    = 0u;
    s_boot_cnt     = 0u;
    s_ota_flag     = 0u;
    s_ota_flag_cnt = 0u;
    s_tmos_cnt     = 0u;
    s_sleep_cnt    = 0u;
    s_sleep_last   = 0u;
    s_wdog_init_cnt = 0u;
    s_wdog_ms       = 0u;
    s_wdog_susp     = false;
    s_u1_wake_cnt   = 0u;
    s_u1_wake_on    = false;
    s_u1_rx_woke    = false;
    s_fw_active     = false;
    s_fw_total      = 0u;
    s_fw_written    = 0u;
    s_fw_finish_cnt = 0u;
    s_fw_crc        = 0u;

    static const uint8_t k_mac[6] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u };
    memcpy(s_mac, k_mac, 6u);
}
