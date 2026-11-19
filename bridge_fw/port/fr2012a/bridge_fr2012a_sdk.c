/* FR2012A（FreqChip FR800x，Cortex-M3）SDK 原语实现
 *
 * 本文件是全工程唯一 #include FreqChip SDK 头的文件（USB/HID 适配文件除外，
 * 它们属于厂商代码的薄封装，见 port.mk 的 SDK_BOUNDARY 说明）。
 * 上层只依赖 bridge_fr2012a_sdk.h。
 */
#include <string.h>

#include "plf.h"
#include "driver_system.h"
#include "driver_pmu.h"
#include "driver_gpio.h"
#include "driver_uart.h"
#include "driver_uart_ex.h"
#include "driver_flash.h"
#include "driver_wdt.h"
#include "os_timer.h"
#include "sys_utils.h"

#include "bridge_fr2012a_sdk.h"
#include "bridge_uart.h"
#include "bridge_log.h"
#include "bridge_board.h"

#define BRIDGE_MOD_FRSDK "frsdk"

/* 与 app_config.h 保持一致：5 = 96MHz（BLE 库要求） */
#ifndef FR_SYSTEM_CLOCK_SEL
#define FR_SYSTEM_CLOCK_SEL 5u
#endif

/* NV / MAC 分区（512KB Flash，避开 BLE 栈的 0x7D000/0x7E000/0x7F000） */
#define FR_NV_BASE      0x7A000u
#define FR_MAC_BASE     0x7B000u
#define FR_SECTOR_SIZE  0x1000u

/* ============================ 系统 / 时间 ============================ */

static enum_GPIOx_t gpio_port(char p);

static volatile uint32_t s_ms;
static os_timer_t        s_ms_timer;
static bool              s_ms_timer_ready;

static void ms_timer_cb(void *arg)
{
    (void)arg;
    s_ms++;
}

void sdk_sys_init(void)
{
    pmu_sub_init();
    system_set_clock(FR_SYSTEM_CLOCK_SEL);

    system_regs->mdm_qspi_cfg.qspi_ref_128m_en = 1;
    system_regs->mdm_qspi_cfg.qspi_ref_clk_sel = 1;
    system_enable_internal_flash_q_read(0x01);
    system_set_internal_flash_clock_div(0);

    __SYSTEM_GPIO_CLK_ENABLE();

    /* v1 不做 MCU 休眠：桥数据面依赖 UART 常开，睡眠唤醒路径待硬件验证 */
    system_sleep_disable();
}

/* 1ms 时基：必须等协议栈 os_timer 环境初始化之后再启动 */
void sdk_tick_init(void)
{
    if (!s_ms_timer_ready) {
        os_timer_init(&s_ms_timer, ms_timer_cb, 0);
        os_timer_start(&s_ms_timer, 1u, true);
        s_ms_timer_ready = true;
    }
}

uint32_t sdk_millis(void)
{
    return s_ms;
}

void sdk_delay_ms(uint32_t ms)
{
    /* os_timer 由 BLE 栈调度推进；纯延时用 SDK 的忙等，避免依赖调度 */
    co_delay_100us(ms * 10u);
}

/* ============================ UART0（桥数据面） ============================
 * 板级引脚见 board/$(TARGET)/bridge_board.h：PA0=UART0_RXD、PA1=UART0_TXD。
 * 注：PA0/PA1 同时是 ROM 串口烧录口（FREQCHIP_Download TX→PA0、RX→PA1）。
 */
#define FR_MUX_UART0     0x04u
#define FR_MUX_UART2     0x0Bu

/* 基版 driver_uart_ex.h 只导出 Uart0/Uart1，UART2 句柄来自 driver_enhance，
 * 这里按 plf.h 的 UART2_BASE 补一个，避免引入两套同名的 driver_system.h。 */
#ifndef Uart2
#define Uart2 ((struct_UART_t *)UART2_BASE)
#endif

static UART_HandleTypeDef s_data_uart;
static bool               s_data_uart_ready;

void sdk_data_uart_mux(char rx_port, uint8_t rx_pin, char tx_port, uint8_t tx_pin)
{
    uint8_t rp = (uint8_t)(rx_port - 'A');
    uint8_t tp = (uint8_t)(tx_port - 'A');

    system_set_port_mux(rp, rx_pin, FR_MUX_UART0);
    system_set_port_mux(tp, tx_pin, FR_MUX_UART0);
    gpio_set_portpull(gpio_port(rx_port), (uint8_t)(1u << rx_pin), GPIO_PULLUP);
}

void sdk_data_uart_init(uint32_t baud)
{
    memset(&s_data_uart, 0, sizeof(s_data_uart));
    s_data_uart.UARTx              = Uart0;
    s_data_uart.Init.BaudRate      = baud;
    s_data_uart.Init.DataLength    = UART_DATA_LENGTH_8BIT;
    s_data_uart.Init.StopBits      = UART_STOPBITS_1;
    s_data_uart.Init.Parity        = UART_PARITY_NONE;
    s_data_uart.Init.FIFO_Mode     = UART_FIFO_ENABLE;

    __SYSTEM_UART0_CLK_ENABLE();
    __SYSTEM_UART0_CLK_SELECT_96M();
    system_regs->rst.uart0_rst = 0;
    uart_init_ex(&s_data_uart);

    __UART_INT_RX_ENABLE(Uart0);
    __UART_INT_LINE_STATUS_ENABLE(Uart0);
    NVIC_ClearPendingIRQ(UART0_IRQn);
    NVIC_SetPriority(UART0_IRQn, 1u);
    NVIC_EnableIRQ(UART0_IRQn);
    s_data_uart_ready = true;
}

bool sdk_data_uart_write_byte(uint8_t b)
{
    uint32_t guard = 200000u;

    if (!s_data_uart_ready) {
        return false;
    }
    while (!Uart0->USR.TFNF) {
        if (--guard == 0u) {
            return false;
        }
    }
    Uart0->DATA_DLL.DATA = b;
    return true;
}

bool sdk_data_uart_tx_empty(void)
{
    return s_data_uart_ready && (Uart0->USR.TFE == 1u);
}

void sdk_data_uart_irq_enable(bool en)
{
    if (!s_data_uart_ready) {
        return;
    }
    if (en) {
        __UART_INT_RX_ENABLE(Uart0);
        NVIC_EnableIRQ(UART0_IRQn);
    } else {
        __UART_INT_RX_DISABLE(Uart0);
        NVIC_DisableIRQ(UART0_IRQn);
    }
}

/* UART0 中断向量（boot_vectors.c 中为 WEAK，本强符号覆盖它） */
void uart0_isr(void)
{
    uint32_t lsr = Uart0->LSR.LSR_DWORD;
    uint8_t  err = 0u;
    uint8_t  guard = 64u;

    (void)Uart0->FCR_IID.IID;    /* 读 IIR 清中断 */

    if (lsr & 0x02u) { err |= BRIDGE_RX_ERR_OVERRUN; }
    if (lsr & 0x08u) { err |= BRIDGE_RX_ERR_FRAME; }
    if (lsr & 0x04u) { err |= BRIDGE_RX_ERR_PARITY; }
    if (err != 0u) {
        bridge_uart_note_rx_error(err);
    }

    while ((guard-- > 0u) && (Uart0->USR.RFNE != 0u)) {
        bridge_uart_isr_feed((uint8_t)Uart0->DATA_DLL.DATA);
    }
}

/* ============================ 调试串口（仅 TX） ============================
 * 键盘：PA5 = UART2_TXD（复用号 0x0B，见 FR201x-U 参考手册 附录 I）
 * 接收器：PA1 = UART0_TXD（复用号 0x04；PA5 已被 USB_DM 占用）
 */
static UART_HandleTypeDef s_dbg_uart;
static bool               s_dbg_uart_ready;

void sdk_dbg_uart_init(uint32_t baud)
{
    if (s_dbg_uart_ready) {
        return;
    }

    memset(&s_dbg_uart, 0, sizeof(s_dbg_uart));
#if BRIDGE_DBG_UART == 2
    system_set_port_mux(BRIDGE_PIN_DBG_TX_PORT - 'A', BRIDGE_PIN_DBG_TX_NUM,
                        FR_MUX_UART2);
    s_dbg_uart.UARTx = Uart2;
    __SYSTEM_UART2_CLK_ENABLE();
#else
    system_set_port_mux(BRIDGE_PIN_DBG_TX_PORT - 'A', BRIDGE_PIN_DBG_TX_NUM,
                        FR_MUX_UART0);
    s_dbg_uart.UARTx = Uart0;
    __SYSTEM_UART0_CLK_ENABLE();
    __SYSTEM_UART0_CLK_SELECT_96M();
    system_regs->rst.uart0_rst = 0;
#endif
    s_dbg_uart.Init.BaudRate   = baud;
    s_dbg_uart.Init.DataLength = UART_DATA_LENGTH_8BIT;
    s_dbg_uart.Init.StopBits   = UART_STOPBITS_1;
    s_dbg_uart.Init.Parity     = UART_PARITY_NONE;
    s_dbg_uart.Init.FIFO_Mode  = UART_FIFO_ENABLE;

    uart_init_ex(&s_dbg_uart);
    s_dbg_uart_ready = true;
}

void sdk_dbg_uart_write(const uint8_t *d, uint16_t len)
{
    if (!s_dbg_uart_ready || d == 0 || len == 0u) {
        return;
    }
    uart_transmit(&s_dbg_uart, (uint8_t *)d, len);
}

/* ============================ GPIO ============================ */

static enum_GPIOx_t gpio_port(char p)
{
    switch (p) {
        case 'B': return GPIO_B;
        case 'C': return GPIO_C;
        case 'D': return GPIO_D;
        case 'E': return GPIO_E;
        default:  return GPIO_A;
    }
}

void sdk_gpio_mux_gpio(char port, uint8_t pin)
{
    (void)pin;
    system_set_port_mux((uint8_t)(port - 'A'), pin, PORT_FUNC_GPIO);
}

void sdk_gpio_out_init(char port, uint8_t pin, bool high)
{
    GPIO_InitTypeDef g;

    sdk_gpio_mux_gpio(port, pin);
    memset(&g, 0, sizeof(g));
    g.Pin  = (uint32_t)(1u << pin);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    gpio_init(gpio_port(port), &g);
    gpio_write_pin(gpio_port(port), (uint8_t)(1u << pin),
                   high ? GPIO_PIN_SET : GPIO_PIN_CLEAR);
}

void sdk_gpio_in_pullup(char port, uint8_t pin)
{
    GPIO_InitTypeDef g;

    sdk_gpio_mux_gpio(port, pin);
    memset(&g, 0, sizeof(g));
    g.Pin  = (uint32_t)(1u << pin);
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    gpio_init(gpio_port(port), &g);
}

void sdk_gpio_write(char port, uint8_t pin, bool high)
{
    gpio_write_pin(gpio_port(port), (uint8_t)(1u << pin),
                   high ? GPIO_PIN_SET : GPIO_PIN_CLEAR);
}

bool sdk_gpio_read(char port, uint8_t pin)
{
    return gpio_read_pin(gpio_port(port), (uint8_t)(1u << pin)) == GPIO_PIN_SET;
}

/* ============================ Flash ============================ */

int sdk_flash_read(uint32_t addr, void *buf, uint16_t len)
{
    if (buf == 0 || len == 0u) {
        return -1;
    }
    return (flash_read(addr, len, (uint8_t *)buf) == 0u) ? 0 : -1;
}

int sdk_flash_write(uint32_t addr, const void *buf, uint16_t len)
{
    if (buf == 0 || len == 0u) {
        return -1;
    }
    return (flash_write(addr, len, (uint8_t *)buf) == 0u) ? 0 : -1;
}

int sdk_flash_erase(uint32_t addr, uint32_t len)
{
    return (flash_erase(addr, len) == 0u) ? 0 : -1;
}

uint32_t sdk_flash_id(void)
{
    return flash_read_id();
}

/* ============================ 芯片信息 / 系统 ============================ */

int sdk_mac_get(uint8_t mac[6])
{
    static const uint8_t erased[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t buf[8];

    if (mac == 0) {
        return -1;
    }
    if (sdk_flash_read(FR_MAC_BASE, buf, sizeof(buf)) != 0) {
        return -1;
    }
    if (memcmp(buf, erased, 6) == 0) {
        uint32_t id = sdk_flash_id();
        uint8_t  i;

        /* 无芯片唯一 MAC：用 Flash ID + 固定前缀生成，写入专用扇区后固定 */
        buf[0] = 0x02; buf[1] = 0x00;
        buf[2] = (uint8_t)(id);
        buf[3] = (uint8_t)(id >> 8);
        buf[4] = (uint8_t)(id >> 16);
        buf[5] = 0x5A;
        (void)sdk_flash_erase(FR_MAC_BASE, FR_SECTOR_SIZE);
        (void)sdk_flash_write(FR_MAC_BASE, buf, 6u);
        for (i = 0u; i < 6u; i++) {
            mac[i] = buf[i];
        }
        return 0;
    }
    memcpy(mac, buf, 6u);
    return 0;
}

void sdk_reset(void)
{
    co_delay_100us(200);
    NVIC_SystemReset();
}

void sdk_enter_bootloader(void)
{
    /* FR2012A 的 ROM ISP 入口由厂商烧录工具/Boot 引脚决定，当前无软件入口；
     * 与 sdk_reset 等价，后续按 EVB 手册补充。 */
    sdk_reset();
}

void sdk_sleep_disable(void)
{
    system_sleep_disable();
}

/* ============================ 射频增强 ============================
 * FR800x-U SDK v2.2 的 patch_rf.o 提供 double rate 相关接口；启用后
 * 最小连接间隔可低于 BLE 默认的 7.5ms（键盘低延迟链路用）。
 * 注意：evt_time_min_slot_enable 所在的 patch.o 依赖未导出的
 * lld_con_evt_time_update_patch，调用会导致链接失败，故不使用。
 */
extern void double_rate_init(void) __attribute__((weak));
extern void double_rate_enable(void) __attribute__((weak));
extern void disenable_conn_param_check(void) __attribute__((weak));
extern void set_start_freq_step(float start_freq, float step) __attribute__((weak));

static bool s_fast_response;

bool sdk_rf_fast_response_enable(void)
{
    if (double_rate_enable == 0) {
        LOG_I(BRIDGE_MOD_FRSDK, "fast response: not in sdk (7.5ms floor)");
        s_fast_response = false;
        return false;
    }

    if (disenable_conn_param_check != 0) {
        disenable_conn_param_check();
    }
    /* 频点/步进：SDK Guide §3 的带外接口（存在则按厂商示例设默认表） */
    if (set_start_freq_step != 0) {
        set_start_freq_step(2404.5f, 2.0f);
    }

    if (double_rate_init != 0) {
        double_rate_init();
    }
    double_rate_enable();      /* 必须在 patch_init() 之后调用 */
    s_fast_response = true;
    LOG_I(BRIDGE_MOD_FRSDK, "fast response: enabled (1.25ms floor)");
    return true;
}

bool sdk_rf_fast_response_supported(void)
{
    return s_fast_response;
}

void sdk_rf_set_tx_power_max(void)
{
    /* SDK Guide §4：本库只有兼容版接口，按最大档设置 */
    system_set_tx_power(RF_TX_POWER_POS_4dBm);
}
