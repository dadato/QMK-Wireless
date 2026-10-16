/* 主机端功能验证（二）：port/ch58x 层
 *
 * 与 main_test.c 的分工：
 *   main_test.c  —— port_host.c 顶替整个 port 层，验证 core/（协议/状态机/时序）
 *   port_test.c  —— sdk_stub.c 顶替 WCH SDK，验证 port 层本身
 *                   （串口 FIFO 背压、NV 布局与读-改-写、ADC 分压、
 *                    GPIO 模式与极性、射频参数翻译、回调翻译、休眠判定）
 *
 * 编译运行见 test_host/Makefile 的 `make test`（键盘/Dongle 两角色各跑一遍）。
 * 覆盖不到的：含 WCH SDK 头的 bridge_ch58x_sdk.c（见 sdk_stub.h 保真约定第 4/5 条）。
 */
#include <stdio.h>
#include <string.h>

#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_port.h"
#include "bridge_ch58x.h"
#include "bridge_ch58x_sdk.h"
#include "bridge_proto.h"
#include "bridge_uart.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"
#include "bridge_hid.h"
#include "bridge_test.h"
#include "bridge_log.h"
#include "bridge_crc16.h"
#include "bridge_rf24pair.h"
#include "bridge_ble_multihost.h"
#include "bridge_ota.h"
#include "bridge_ota_layout.h"      /* BRIDGE_OTA_FLAG_UART_IAP */
#include "bridge_ble_hidsvc.h"      /* BRIDGE_HID_RID_RAW / BRIDGE_HID_RAW_REPORT_LEN */
#include "sdk_stub.h"

/* ---------------- 迷你测试框架（与 main_test.c 一致） ---------------- */
static int s_pass;
static int s_fail;
static const char *s_case;

#define CASE(name) do { s_case = (name); } while (0)

#define CHECK(cond) do {                                                        \
    if (cond) { s_pass++; } else {                                              \
        s_fail++;                                                               \
        printf("  FAIL %s:%d  [%s]  %s\n", __FILE__, __LINE__, s_case, #cond);   \
    }                                                                           \
} while (0)

#define CHECK_EQ(a, b) do {                                                     \
    long _a = (long)(a), _b = (long)(b);                                        \
    if (_a == _b) { s_pass++; } else {                                          \
        s_fail++;                                                               \
        printf("  FAIL %s:%d  [%s]  %s=%ld expected %ld\n",                     \
               __FILE__, __LINE__, s_case, #a, _a, _b);                          \
    }                                                                           \
} while (0)

/* ---------------- 公共工具 ---------------- */

/* NV 配置区基址：DataFlash(0x70000) + 0x7000（与 bridge_port_ch58x.c 一致） */
#define NV_TEST_BASE  0x77000u

#if !BRIDGE_BOARD_IS_DONGLE
/* 键盘侧专属工具：只有 test_callback_translation 的键盘分支用 UART1 抓包比对 */
/* 推进时间并跑若干轮主循环，每轮把 UART1 FIFO 排到抓包缓冲 */
static void pump(uint32_t ms)
{
    stub_advance_ms(ms);
    for (uint8_t i = 0u; i < 8u; i++) {
        bridge_ch58x_loop();
        stub_u1_tx_drain();
    }
}

/* 在 UART1 抓包缓冲里查找指定 CMD 的合法帧（CRC 校验） */
static int tx_find(uint8_t cmd, uint8_t *out_payload, uint8_t *out_len)
{
    for (uint16_t i = 0u; (uint16_t)(i + BRIDGE_FRAME_OVERHEAD) <= g_stub_u1_tx_len; i++) {
        if (g_stub_u1_tx[i] != BRIDGE_SOF0 || g_stub_u1_tx[i + 1u] != BRIDGE_SOF1) {
            continue;
        }
        uint8_t len = g_stub_u1_tx[i + 2u];
        if ((uint16_t)(i + BRIDGE_FRAME_OVERHEAD + len) > g_stub_u1_tx_len) {
            break;
        }
        uint16_t crc = bridge_crc16_ccitt(&g_stub_u1_tx[i + 2u],
                                          (uint16_t)(BRIDGE_HDR_LEN + len));
        uint16_t rx  = (uint16_t)(g_stub_u1_tx[i + 2u + BRIDGE_HDR_LEN + len] |
                                  ((uint16_t)g_stub_u1_tx[i + 3u + BRIDGE_HDR_LEN + len] << 8));
        if (crc != rx) {
            continue;
        }
        if (g_stub_u1_tx[i + 3u] == cmd) {
            if (out_len != 0) {
                *out_len = len;
            }
            if ((out_payload != 0) && (len > 0u)) {
                memcpy(out_payload, &g_stub_u1_tx[i + 6u], len);
            }
            return 1;
        }
        i = (uint16_t)(i + BRIDGE_FRAME_OVERHEAD + len - 1u);
    }
    return 0;
}
#endif /* !BRIDGE_BOARD_IS_DONGLE */

#if BRIDGE_BOARD_HAS_MODE_SEL
static void reset_mode_sel(uint8_t sel)
{
    stub_gpio_set_in(BRIDGE_PIN_MODE_SEL0_PORT, BRIDGE_PIN_MODE_SEL0_NUM,
                     (sel & 0x01u) != 0u);
    stub_gpio_set_in(BRIDGE_PIN_MODE_SEL1_PORT, BRIDGE_PIN_MODE_SEL1_NUM,
                     (sel & 0x02u) != 0u);
}
#endif

/* ---------------- 用例 ---------------- */

/* 必须是本进程第一次触发 UART0 写入（port 层静态 s_log_ready 只在首次调用时置位） */
static void test_log_lazy_init(void)
{
    CASE("日志 UART0 惰性初始化（release 不白占 UART0）");

    CHECK_EQ(stub_u0_init_count(), 0u);
    CHECK_EQ(stub_u1_baud(), 0u);           /* 桥数据面此时尚未初始化 */

    port_log_write("A", 1u);
    CHECK_EQ(stub_u0_init_count(), 1u);
    CHECK_EQ(stub_u0_baud(), BRIDGE_LOG_BAUD);
    CHECK_EQ(g_stub_u0_tx_len, 1u);
    CHECK_EQ(g_stub_u0_tx[0], 'A');

    port_log_write("B", 1u);
    CHECK_EQ(stub_u0_init_count(), 1u);     /* 只初始化一次 */
    CHECK_EQ(g_stub_u0_tx_len, 2u);
    CHECK_EQ(g_stub_u0_tx[1], 'B');

    port_log_write(0, 3u);                  /* 空指针：不得崩 */
    port_log_write("C", 0u);                /* 零长度：不得崩 */
    CHECK_EQ(g_stub_u0_tx_len, 2u);
}

static void test_uart_init(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    CASE("串口初始化：Dongle 不使用 UART1（保持未初始化）");

    bridge_uart_init(BRIDGE_UART_BAUD);
    CHECK(!stub_u1_inited());               /* Dongle 走 USB，不占 PA8/PA9 */
    CHECK_EQ(stub_u1_baud(), 0u);
    CHECK(!stub_u1_rx_irq());

    port_uart_irq_enable(true);             /* 不得初始化/使能 UART1 */
    CHECK(!stub_u1_inited());
    CHECK(!stub_u1_rx_irq());
#else
    CASE("串口初始化：波特率 921600 / RX 中断开关");

    bridge_uart_init(BRIDGE_UART_BAUD);
    CHECK(stub_u1_inited());
    CHECK_EQ(stub_u1_baud(), 921600u);
    CHECK_EQ(stub_u1_baud(), BRIDGE_UART_BAUD);
    CHECK(stub_u1_rx_irq());

    port_uart_irq_enable(false);
    CHECK(!stub_u1_rx_irq());
    port_uart_irq_enable(true);
    CHECK(stub_u1_rx_irq());
#endif
}

#if !BRIDGE_BOARD_IS_DONGLE
static void test_uart_tx_backpressure(void)
{
    uint8_t d[17];

    CASE("UART1 TX：按 FIFO 余量分块 + 环满丢弃");

    bridge_uart_init(BRIDGE_UART_BAUD);
    stub_u1_tx_reset();
    for (uint8_t i = 0u; i < 17u; i++) {
        d[i] = (uint8_t)(i + 1u);
    }

    port_uart_write(d, 17u);
    CHECK(port_uart_tx_busy());                 /* 软件环仍有残留 → 线路忙 */

    bridge_ch58x_loop();
    CHECK_EQ(sdk_uart1_tx_bytes(), 8u);         /* 一次最多灌满 FIFO，不得溢出 */
    CHECK_EQ(g_stub_u1_tx_drop, 0u);
    stub_u1_tx_drain();

    bridge_ch58x_loop();
    CHECK_EQ(sdk_uart1_tx_bytes(), 8u);
    stub_u1_tx_drain();

    bridge_ch58x_loop();
    CHECK_EQ(sdk_uart1_tx_bytes(), 1u);
    stub_u1_tx_drain();

    CHECK_EQ(g_stub_u1_tx_len, 17u);
    CHECK(memcmp(g_stub_u1_tx, d, 17u) == 0);   /* 顺序与内容一致 */
    CHECK(!port_uart_tx_busy());
    CHECK_EQ(g_stub_u1_tx_drop, 0u);

    /* 环满：128B 写入 96B 的 TX 环 → 只保留前 95B（环形缓冲留 1B 区分满/空） */
    stub_reset();
#if BRIDGE_BOARD_HAS_MODE_SEL
    reset_mode_sel(1u);                 /* 01 = BLE：与 bridge_mode_init 采样值一致 */
#endif
    bridge_ch58x_init();                /* 完整上电：core 模式状态机就位，MODE_SEL 不再变化 */
    for (uint8_t i = 0u; i < 8u; i++) {
        bridge_ch58x_loop();
        stub_u1_tx_drain();
    }
    stub_u1_tx_reset();                 /* 丢弃上电 EVT_STATE，从干净抓包开始 */
    {
        uint8_t big[128];
        memset(big, 0x5Au, sizeof(big));
        port_uart_write(big, (uint16_t)sizeof(big));
        for (uint8_t i = 0u; i < 20u; i++) {
            bridge_ch58x_loop();
            stub_u1_tx_drain();
        }
    }
    CHECK_EQ(BRIDGE_TX_RING_SIZE, 96u);
    CHECK_EQ(g_stub_u1_tx_len, (uint16_t)(BRIDGE_TX_RING_SIZE - 1u));
    CHECK_EQ(g_stub_u1_tx_drop, 0u);            /* 丢弃发生在软件环，非硬件 FIFO */
    CHECK(!port_uart_tx_busy());
}

static void test_uart_isr_rx(void)
{
    uint8_t        f[32];
    uint8_t        pay[4] = { 0x11u, 0x22u, 0x33u, 0x44u };
    bridge_frame_t fr;

    CASE("UART1 ISR：读空 RX FIFO → core 组帧");

    bridge_uart_init(BRIDGE_UART_BAUD);
    uint16_t n = bridge_uart_encode(f, (uint16_t)sizeof(f), CMD_GET_VERSION,
                                    7u, 0u, pay, 4u);
    CHECK_EQ(n, 12u);                           /* 2 SOF + 4 HDR + 4 payload + 2 CRC */

    uint32_t iir0 = stub_u1_iir_reads();
    uint32_t rd0  = stub_u1_rx_reads();

    /* 硬件 FIFO 深 8：分两次注入，模拟真实中断时机 */
    stub_u1_rx_push(f, 8u);
    bridge_ch58x_uart_isr();
    CHECK_EQ(sdk_uart1_rx_bytes(), 0u);         /* ISR 必须读空 */
    CHECK_EQ(stub_u1_rx_reads() - rd0, 8u);

    stub_u1_rx_push(&f[8], (uint16_t)(n - 8u));
    bridge_ch58x_uart_isr();
    CHECK_EQ(sdk_uart1_rx_bytes(), 0u);
    CHECK_EQ(stub_u1_rx_reads() - rd0, (uint32_t)n);
    CHECK_EQ(stub_u1_iir_reads() - iir0, 2u);   /* 每次进 ISR 先读 IIR 清中断 */

    CHECK(bridge_uart_get_frame(&fr));
    CHECK_EQ(fr.cmd, CMD_GET_VERSION);
    CHECK_EQ(fr.seq, 7u);
    CHECK_EQ(fr.len, 4u);
    CHECK(memcmp(fr.payload, pay, 4u) == 0);

    /* FIFO 溢出语义：注入 10B 只留 8B */
    {
        uint8_t over[10];
        memset(over, 0xAAu, sizeof(over));
        stub_u1_rx_push(over, (uint16_t)sizeof(over));
    }
    CHECK_EQ(sdk_uart1_rx_bytes(), (uint8_t)STUB_FIFO_DEPTH);
    CHECK_EQ(stub_u1_rx_drop(), 2u);
    bridge_ch58x_uart_isr();
    CHECK_EQ(sdk_uart1_rx_bytes(), 0u);
}
#endif /* !BRIDGE_BOARD_IS_DONGLE */

static void test_nv_rw(void)
{
    uint8_t w[64], r[64], n2[64], n3[64], n4[64];

    CASE("NV：槽地址 / 256B 读-改-写 / 邻槽保护");

    stub_flash_clear();
    memset(w, 0xA5u, sizeof(w));
    memset(r, 0, sizeof(r));

    CHECK_EQ(port_nv_write(3u, w, 64u), BRIDGE_OK);
    CHECK_EQ(stub_flash_erase_count(), 1u);
    CHECK_EQ(stub_flash_write_count(), 1u);

    /* 槽地址 = DataFlash 0x70000 + 0x7000 + id*64 */
    CHECK_EQ(stub_flash_peek(NV_TEST_BASE + (3u * 64u), r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, w, 64u) == 0);

    memset(r, 0, sizeof(r));
    CHECK_EQ(port_nv_read(3u, r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, w, 64u) == 0);

    /* 同一 256B 块内（槽 2/3）与跨块（槽 4）：邻槽必须被保留 */
    memset(n2, 0x11u, sizeof(n2));
    memset(n3, 0x22u, sizeof(n3));
    memset(n4, 0x33u, sizeof(n4));

    CHECK_EQ(port_nv_write(2u, n2, 64u), BRIDGE_OK);
    memset(r, 0, sizeof(r));
    CHECK_EQ(port_nv_read(3u, r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, w, 64u) == 0);              /* 槽 3 未被槽 2 的擦除破坏 */

    CHECK_EQ(port_nv_write(3u, n3, 64u), BRIDGE_OK);
    CHECK_EQ(port_nv_write(4u, n4, 64u), BRIDGE_OK);
    memset(r, 0, sizeof(r));
    CHECK_EQ(port_nv_read(2u, r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, n2, 64u) == 0);
    memset(r, 0, sizeof(r));
    CHECK_EQ(port_nv_read(3u, r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, n3, 64u) == 0);
    memset(r, 0, sizeof(r));
    CHECK_EQ(port_nv_read(4u, r, 64u), BRIDGE_OK);
    CHECK(memcmp(r, n4, 64u) == 0);

    /* 每次写 = 1 次 256B 擦 + 1 次 256B 写 */
    CHECK_EQ(stub_flash_erase_count(), 4u);
    CHECK_EQ(stub_flash_write_count(), 4u);
}

static void test_nv_param_and_errors(void)
{
    uint8_t w[64], r[64];

    CASE("NV：参数校验与 IO 故障传播");

    memset(w, 0x77u, sizeof(w));
    memset(r, 0, sizeof(r));

    CHECK_EQ(port_nv_read(0u, r, 8u), BRIDGE_ERR_PARAM);    /* id 0 保留 */
    CHECK_EQ(port_nv_write(0u, w, 8u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_nv_read(64u, r, 8u), BRIDGE_ERR_PARAM);   /* 64 槽 → id 最大 63 */
    CHECK_EQ(port_nv_write(64u, w, 8u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_nv_read(1u, r, 65u), BRIDGE_ERR_PARAM);   /* 超过槽大小 64B */
    CHECK_EQ(port_nv_read(1u, 0, 8u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_nv_write(1u, 0, 8u), BRIDGE_ERR_PARAM);

    CHECK_EQ(port_nv_read(63u, r, 64u), BRIDGE_OK);         /* 末槽不越界 */
    CHECK_EQ(port_nv_write(63u, w, 64u), BRIDGE_OK);

    stub_flash_fail_erase(true);
    CHECK_EQ(port_nv_write(5u, w, 64u), BRIDGE_ERR_IO);
    stub_flash_fail_erase(false);

    stub_flash_fail_write(true);
    CHECK_EQ(port_nv_write(5u, w, 64u), BRIDGE_ERR_IO);
    stub_flash_fail_write(false);
}

static void test_battery(void)
{
    CASE("电池 ADC 分压还原与充电检测");

#if BRIDGE_BOARD_IS_DONGLE
    CHECK_EQ(port_bat_read_mv(), 0u);           /* Dongle 无电池 */
    CHECK_EQ(port_charger_active(), false);
#else
    {
        uint32_t a0 = stub_adc_reads();
        stub_adc_set_mv(BRIDGE_ADC_CH_BAT, 2100u);
        CHECK_EQ(port_bat_read_mv(), 4200u);    /* 分压 DEN/NUM = 2/1 → ×2 */
        CHECK_EQ(stub_adc_reads(), a0 + 1u);
        stub_adc_set_mv(BRIDGE_ADC_CH_BAT, 1800u);
        CHECK_EQ(port_bat_read_mv(), 3600u);
    }
    /* CHRG_STAT 低有效 */
    stub_gpio_set_in(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM, true);
    CHECK(!port_charger_active());
    stub_gpio_set_in(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM, false);
    CHECK(port_charger_active());
    stub_gpio_set_in(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM, true);
#endif

    /* pinmap 第 5 章：VBUS 由 SK32 检测，桥不参与（否则无线模式会被永久拒绝） */
    CHECK(!port_vbus_present());
}

static void test_gpio(void)
{
    CASE("GPIO 初始化模式与输出极性");

    stub_reset();
    port_gpio_init();

    /* ISP 配置脚量产必须为高：外部 10k 上拉 + 内部上拉兜底 */
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_ISP_CFG_PORT, BRIDGE_PIN_ISP_CFG_NUM),
             STUB_GPIO_IN_PU);

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：无 MODE_SEL 拨码，port 层不得引用该引脚（BRIDGE_BOARD_HAS_MODE_SEL=0） */
    CHECK_EQ(port_gpio_read_mode_sel(), 0u);

    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM),
             STUB_GPIO_OUT);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM));
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM),
             STUB_GPIO_IN_PU);
#else
    /* MODE_SEL0/1：内部下拉输入（悬空 = 00 = USB 默认模式） */
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_MODE_SEL0_PORT, BRIDGE_PIN_MODE_SEL0_NUM),
             STUB_GPIO_IN_PD);
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_MODE_SEL1_PORT, BRIDGE_PIN_MODE_SEL1_NUM),
             STUB_GPIO_IN_PD);

    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM), STUB_GPIO_IN_PU);
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM), STUB_GPIO_OUT);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM));
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM), STUB_GPIO_OUT);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM));
    CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_BAT_ADC_PORT, BRIDGE_PIN_BAT_ADC_NUM),
             STUB_GPIO_IN_FLOAT);

    /* MODE_SEL 编码：bit0 = SEL0，bit1 = SEL1 */
    reset_mode_sel(0u);
    CHECK_EQ(port_gpio_read_mode_sel(), 0u);
    reset_mode_sel(1u);
    CHECK_EQ(port_gpio_read_mode_sel(), 1u);
    reset_mode_sel(2u);
    CHECK_EQ(port_gpio_read_mode_sel(), 2u);
    reset_mode_sel(3u);
    CHECK_EQ(port_gpio_read_mode_sel(), 3u);
    reset_mode_sel(0u);
#endif

    /* 未使用引脚：推挽输出低。SDI（PB14/PB15）与 RST#（PB23）严禁改动 */
    CHECK_EQ(stub_gpio_mode('B', 14), STUB_GPIO_IN_PU);   /* SDI TIO 未被改动 */
    CHECK_EQ(stub_gpio_mode('B', 15), STUB_GPIO_IN_PU);   /* SDI TCK 未被改动 */
    CHECK_EQ(stub_gpio_mode('B', 23), STUB_GPIO_IN_PU);   /* RST# 未被改动 */
#if BRIDGE_BOARD_IS_DONGLE
    CHECK_EQ(stub_gpio_mode('A', 8), STUB_GPIO_OUT);      /* PA8 未用 → 输出低 */
    CHECK(!stub_gpio_get_out('A', 8));
    CHECK_EQ(stub_gpio_mode('B', 12), STUB_GPIO_OUT);     /* PB12 未用 → 输出低 */
    CHECK(!stub_gpio_get_out('B', 12));
#else
    CHECK_EQ(stub_gpio_mode('A', 12), STUB_GPIO_OUT);     /* PA12 未用 → 输出低 */
    CHECK(!stub_gpio_get_out('A', 12));
    CHECK_EQ(stub_gpio_mode('B', 10), STUB_GPIO_OUT);     /* PB10（USB1 未用）→ 低 */
    CHECK(!stub_gpio_get_out('B', 10));
#endif

#if BRIDGE_BOARD_IS_DONGLE
    port_gpio_set_link(true);
    CHECK(stub_gpio_get_out(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM));
    port_gpio_set_link(false);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM));
    port_gpio_set_wake(true);           /* Dongle 无 WAKE：应为空操作 */
#else
    port_gpio_set_wake(true);
    CHECK(stub_gpio_get_out(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM));
    port_gpio_set_wake(false);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM));

    port_gpio_set_link(true);
    CHECK(stub_gpio_get_out(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM));
    port_gpio_set_link(false);
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM));
#endif
}

static void test_chip_and_system(void)
{
    uint8_t mac[6];
    uint8_t exp[6] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu };

    CASE("芯片 MAC / 软复位 / 进 ISP");

    stub_ble_mac_set(exp);
    memset(mac, 0, sizeof(mac));
    CHECK_EQ(port_chip_get_mac(mac), BRIDGE_OK);
    CHECK(memcmp(mac, exp, 6u) == 0);
    CHECK_EQ(port_chip_get_mac(0), BRIDGE_ERR_PARAM);

    {
        uint32_t r0 = stub_reset_count();
        port_reset();
        CHECK_EQ(stub_reset_count(), r0 + 1u);
    }

#if BRIDGE_BOARD_IS_DONGLE
    {
        uint32_t b0  = stub_boot_count();
        uint32_t ms0 = stub_now_ms();
        port_enter_bootloader();
        CHECK_EQ(stub_boot_count(), b0 + 1u);
        CHECK_EQ(stub_gpio_mode(BRIDGE_PIN_ISP_CFG_PORT, BRIDGE_PIN_ISP_CFG_NUM),
                 STUB_GPIO_OUT);
        CHECK(!stub_gpio_get_out(BRIDGE_PIN_ISP_CFG_PORT, BRIDGE_PIN_ISP_CFG_NUM));
        CHECK_EQ(stub_now_ms(), ms0 + 2u);      /* 拉低 ISP 脚后延时 2ms 再复位 */
    }
#else
    {
        uint32_t c0 = stub_ota_flag_count();
        uint32_t r0 = stub_reset_count();
        port_enter_bootloader();
        CHECK_EQ(stub_ota_flag_count(), c0 + 1u);
        CHECK_EQ(stub_ota_flag(), BRIDGE_OTA_FLAG_UART_IAP);
        CHECK_EQ(stub_reset_count(), r0 + 1u);  /* 写 IAP 标志后复位（不复返） */
    }
#endif
}

/* ---------------- 射频用例公共基线 ---------------- */

/* 干净的射频基线：core 全量初始化后，把两条射频都置为「未初始化」。
 * 注意 port 层的 s_up[]/s_active_radio 是文件级静态，stub_reset() 不会清它们，
 * 故每个射频用例都必须经本函数重新建立确定状态。 */
static void radio_baseline(void)
{
    stub_reset();
#if BRIDGE_BOARD_HAS_MODE_SEL
    reset_mode_sel(1u);                 /* 01 = BLE：两角色上电模式统一，便于断言 */
#endif
    bridge_ch58x_init();                /* 内置 bridge_mode_init → 会按 cfg 拉起 BLE */
    (void)port_radio_deinit(BRIDGE_RADIO_BLE);
    (void)port_radio_deinit(BRIDGE_RADIO_2G4);
    stub_radio_obs_reset();             /* 只清抓包与计数，不动已初始化/链路状态 */
}

/* ---------------- 射频用例 ---------------- */

static void test_radio_init(void)
{
    CASE("射频初始化/反初始化：参数、幂等、IO 故障传播");

    radio_baseline();

    CHECK_EQ(port_radio_init(BRIDGE_RADIO_NONE), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_init(3u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_deinit(BRIDGE_RADIO_NONE), BRIDGE_ERR_PARAM);

    /* BLE：按 cfg 应用广播名与连接间隔 */
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_init_count(), 1u);
    CHECK_EQ(stub_ble_adv_name_mac(), 1);      /* cfg 默认 adv_name_with_mac=1 */
    CHECK_EQ(stub_ble_interval_units(), 6);    /* cfg 默认 conn_interval=6 → 7.5ms */
    CHECK(!port_radio_is_connected(BRIDGE_RADIO_BLE));

    /* 幂等：core 的模式事务可能重复调用 */
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_init_count(), 1u);

    CHECK_EQ(port_radio_deinit(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_deinit_count(), 1u);
    CHECK_EQ(port_radio_deinit(BRIDGE_RADIO_BLE), BRIDGE_OK);   /* 幂等 */
    CHECK_EQ(stub_ble_deinit_count(), 1u);

    /* 2.4G：厂商初始化失败必须向上传播（core 据此判定模式切换失败） */
    stub_rf24_fail_init(true);
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_ERR_IO);
    CHECK_EQ(stub_rf24_init_count(), 0u);
    stub_rf24_fail_init(false);
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_OK);
    CHECK_EQ(stub_rf24_init_count(), 1u);
    CHECK_EQ(port_radio_deinit(BRIDGE_RADIO_2G4), BRIDGE_OK);
    CHECK_EQ(stub_rf24_deinit_count(), 1u);

    /* 协议版本查询：未知类型返回 0 */
    CHECK_EQ(port_radio_protocol_ver(BRIDGE_RADIO_NONE), 0u);
    CHECK_EQ(port_radio_protocol_ver(BRIDGE_RADIO_BLE), 1u);
    CHECK_EQ(port_radio_protocol_ver(BRIDGE_RADIO_2G4), 2u);
}

static void test_radio_send(void)
{
    uint8_t p[2] = { 0x11u, 0x22u };
    uint8_t kbd8[BRIDGE_REPORT_KEYBOARD_LEN] = { 0 };
    uint8_t mouse4[BRIDGE_REPORT_MOUSE_LEN]  = { 0 };

    CASE("射频发送：参数校验与链路状态门");

    radio_baseline();

    CHECK_EQ(port_radio_send(BRIDGE_RADIO_NONE, p, 2u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_send(3u, p, 2u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, 0, 2u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, p, 0u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, p, 2u), BRIDGE_ERR_STATE);  /* 未初始化 */
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_2G4, p, 2u), BRIDGE_ERR_STATE);

    CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, p, 2u), BRIDGE_ERR_STATE);  /* 已初始化未连接 */

    stub_ble_set_connected(true);
    CHECK(port_radio_is_connected(BRIDGE_RADIO_BLE));
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, p, 2u), BRIDGE_OK);
    CHECK_EQ(g_stub_ble_tx_len, 2u);
    CHECK_EQ(g_stub_ble_tx[0], 0x11u);
    CHECK_EQ(g_stub_ble_tx[1], 0x22u);
    CHECK_EQ(g_stub_ble_tx_rid, 2u);   /* 2B → 多媒体 Report ID */

    /* BLE：按报文长度选 HOGP Report ID（bridge_ble_hidsvc.h） */
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, kbd8, BRIDGE_REPORT_KEYBOARD_LEN), BRIDGE_OK);
    CHECK_EQ(g_stub_ble_tx_rid, 1u);   /* 8B → 键盘 Report ID */
    /* 4B 鼠标在 BLE 侧无对应报表 */
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_BLE, mouse4, BRIDGE_REPORT_MOUSE_LEN),
             BRIDGE_ERR_UNSUPPORTED);

    CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_OK);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_2G4, p, 2u), BRIDGE_ERR_STATE);
    stub_rf24_set_connected(true);
    CHECK(port_radio_is_connected(BRIDGE_RADIO_2G4));
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_2G4, p, 2u), BRIDGE_OK);
    CHECK_EQ(g_stub_rf24_tx_len, 2u);

    /* 厂商发送失败 → 向上报 IO 错误（core 会转成 HID 转发失败） */
    stub_rf24_fail_send(true);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_2G4, p, 2u), BRIDGE_ERR_IO);
    stub_rf24_fail_send(false);
    CHECK_EQ(port_radio_send(BRIDGE_RADIO_2G4, p, 2u), BRIDGE_OK);
}

static void test_radio_pair_profile(void)
{
    CASE("射频配对 / 多主机 profile / 解除绑定");

    radio_baseline();

    CHECK_EQ(port_radio_pair_start(BRIDGE_RADIO_NONE), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_pair_start(BRIDGE_RADIO_BLE), BRIDGE_ERR_STATE);
    CHECK_EQ(port_radio_pair_start(BRIDGE_RADIO_2G4), BRIDGE_ERR_STATE);

    /* 停配对窗口：同样按参数/状态校验 */
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_NONE), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_BLE), BRIDGE_ERR_STATE);
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_2G4), BRIDGE_ERR_STATE);

    /* profile 是 BLE 独有概念 */
    CHECK_EQ(port_radio_connect_profile(BRIDGE_RADIO_2G4, 1u), BRIDGE_ERR_UNSUPPORTED);
    CHECK_EQ(port_radio_connect_profile(BRIDGE_RADIO_BLE, 1u), BRIDGE_ERR_STATE);

    CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_OK);

    CHECK_EQ(port_radio_pair_start(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_pair_count(), 1u);
    CHECK_EQ(port_radio_pair_start(BRIDGE_RADIO_2G4), BRIDGE_OK);
    CHECK_EQ(stub_rf24_pair_count(), 1u);

    /* 停配对窗口：BLE 关广播 / 2.4G 停重发，且可重复调用 */
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_pair_stop_count(), 1u);
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_BLE), BRIDGE_OK);
    CHECK_EQ(stub_ble_pair_stop_count(), 2u);
    CHECK_EQ(port_radio_pair_stop(BRIDGE_RADIO_2G4), BRIDGE_OK);
    CHECK_EQ(stub_rf24_pair_stop_count(), 1u);

    CHECK_EQ(port_radio_connect_profile(BRIDGE_RADIO_BLE, 0u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_connect_profile(BRIDGE_RADIO_BLE,
                                        (uint8_t)(BRIDGE_MAX_BLE_PROFILE + 1u)),
             BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_connect_profile(BRIDGE_RADIO_BLE, 3u), BRIDGE_OK);
    CHECK_EQ(stub_ble_profile(), 3);

    /* 解绑：BLE 透传 profile（0xFF = 全部）；2.4G 无 profile 概念 */
    CHECK_EQ(port_radio_unpair(BRIDGE_RADIO_BLE, 2u), BRIDGE_OK);
    CHECK_EQ(stub_ble_unpair_profile(), 2);
    CHECK_EQ(port_radio_unpair(BRIDGE_RADIO_BLE, 0xFFu), BRIDGE_OK);
    CHECK_EQ(stub_ble_unpair_profile(), 0xFF);

    {
        uint32_t u0 = stub_rf24_unpair_count();
        CHECK_EQ(port_radio_unpair(BRIDGE_RADIO_2G4, 0u), BRIDGE_OK);
        CHECK_EQ(stub_rf24_unpair_count(), u0 + 1u);
    }
}

static void test_radio_report_period(void)
{
    CASE("上报周期翻译（BLE 1.25ms 单位）/ SRRC 定频路由");

    radio_baseline();

    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_NONE, 10u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_BLE, 0u), BRIDGE_ERR_PARAM);
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_2G4, 0u), BRIDGE_ERR_PARAM);

    /* BLE：连接间隔单位 1.25ms → units = ms*4/5 */
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_BLE, 10u), BRIDGE_OK);
    CHECK_EQ(stub_ble_interval_units(), 8);
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_BLE, 8u), BRIDGE_OK);
    CHECK_EQ(stub_ble_interval_units(), 6);                 /* 8ms → 6 单位 = 7.5ms */

    /* 非法毫秒值不得下发：1ms → 0 单位；400ms → 320 单位 > 255 */
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_BLE, 1u), BRIDGE_ERR_PARAM);
    CHECK_EQ(stub_ble_interval_units(), 6);
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_BLE, 400u), BRIDGE_ERR_PARAM);
    CHECK_EQ(stub_ble_interval_units(), 6);

    /* 2.4G：直传毫秒，目标 1ms（D5），不做单位换算 */
    CHECK_EQ(port_radio_set_report_period_ms(BRIDGE_RADIO_2G4, 1u), BRIDGE_OK);
    CHECK_EQ(stub_rf24_period_ms(), 1);

    /* SRRC 定频（D9）：当前只有 BLE 侧可实现 */
    CHECK_EQ(port_radio_test_mode(BRIDGE_RADIO_2G4, 19u, 2u), BRIDGE_ERR_UNSUPPORTED);
    CHECK_EQ(port_radio_test_mode(BRIDGE_RADIO_BLE, 19u, 2u), BRIDGE_OK);
    CHECK_EQ(stub_ble_test_ch(), 19);
}

static void test_radio_event(void)
{
    CASE("射频链路事件 → core 模式/GPIO 联动");

    radio_baseline();
    CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);

    CHECK(!bridge_mode_is_connected());
    bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_CONNECTED);
    CHECK(bridge_mode_is_connected());
    /* 连接状态以协议栈为准，回调只做事件通知 */
    CHECK(!port_radio_is_connected(BRIDGE_RADIO_BLE));

#if BRIDGE_BOARD_IS_DONGLE
    CHECK(stub_gpio_get_out(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM));
#else
    CHECK(stub_gpio_get_out(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM));
#endif

    /* 未定义事件不得改变状态 */
    bridge_ch58x_radio_evt(0x7Fu);
    CHECK(bridge_mode_is_connected());

    bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_DISCONNECTED);
    CHECK(!bridge_mode_is_connected());

#if BRIDGE_BOARD_IS_DONGLE
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM));
#else
    CHECK(!stub_gpio_get_out(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM));
#endif
}

/* ---------------- 厂商回调翻译用例 ---------------- */

static void test_callback_translation(void)
{
    CASE("厂商回调 → 桥内部翻译（bridge_ch58x_callbacks.c）");

    radio_baseline();

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：键盘经 2.4G 上行的 HID 报文 → USB HID 转发主机 */
    CHECK_EQ(sdk_usb_init(), BRIDGE_OK);
    CHECK_EQ(stub_usb_init_count(), 1u);
    stub_usb_set_configured(true);

    {
        uint8_t rep[8] = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };

        sdk_rf24_on_recv(rep, 8u);
        CHECK_EQ(g_stub_usb_tx_len, 8u);
        CHECK(memcmp(g_stub_usb_tx, rep, 8u) == 0);

        /* BLE 侧的 LED Output Report 同样经 USB 上行 */
        g_stub_usb_tx_len = 0u;
        sdk_ble_on_led_report(0x07u);
        CHECK_EQ(g_stub_usb_tx_len, 1u);
        CHECK_EQ(g_stub_usb_tx[0], 0x07u);

        /* 非法入参不得触碰 USB */
        g_stub_usb_tx_len = 0u;
        sdk_rf24_on_recv(0, 0u);
        sdk_rf24_on_recv(rep, 0u);
        CHECK_EQ(g_stub_usb_tx_len, 0u);

        /* 心跳包（类型前缀 0x03）：仅保活，不得派发为 HID */
        g_stub_usb_tx_len = 0u;
        {
            uint8_t hb = BRIDGE_RADIO_PKT_HEARTBEAT;
            sdk_rf24_on_recv(&hb, 1u);
        }
        CHECK_EQ(g_stub_usb_tx_len, 0u);

        /* 透传类型前缀（0x02）：去前缀后经 USB Raw HID 上行主机 */
        g_stub_usb_tx_len = 0u;
        {
            uint8_t tp[3] = { BRIDGE_RADIO_PKT_TUNNEL, 0xAAu, 0xBBu };
            sdk_rf24_on_recv(tp, 3u);
        }
        CHECK_EQ(g_stub_usb_tx_len, 2u);
        CHECK_EQ(g_stub_usb_tx[0], 0xAAu);
        CHECK_EQ(g_stub_usb_tx[1], 0xBBu);

        /* HID 类型前缀（0x01）：去前缀后为 8B 键盘报文 */
        g_stub_usb_tx_len = 0u;
        {
            uint8_t hp[9] = { BRIDGE_RADIO_PKT_HID };
            sdk_rf24_on_recv(hp, 9u);
        }
        CHECK_EQ(g_stub_usb_tx_len, 8u);

        /* 主机下发的 LED 状态 → 经 2.4G 回传键盘 */
        CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_OK);
        stub_rf24_set_connected(true);
        sdk_usb_on_led_report(0x05u);
        CHECK_EQ(g_stub_rf24_tx_len, 1u);
        CHECK_EQ(g_stub_rf24_tx[0], 0x05u);

        /* 2.4G 未连接时静默丢弃（sdk_rf24_send → STATE），抓包不刷新 */
        stub_rf24_set_connected(false);
        sdk_usb_on_led_report(0x06u);
        CHECK_EQ(g_stub_rf24_tx_len, 1u);
        CHECK_EQ(g_stub_rf24_tx[0], 0x05u);

        /* 主机挂起/恢复（USB 边沿，sdk_usb_poll 内检测）→ 经 2.4G 下发键盘 */
        g_stub_rf24_tx_len = 0u;
        sdk_usb_on_host_power(true);
        CHECK_EQ(g_stub_rf24_tx_len, 1u);
        CHECK_EQ(g_stub_rf24_tx[0], BRIDGE_HOST_SUSPEND);
        sdk_usb_on_host_power(false);
        CHECK_EQ(g_stub_rf24_tx[0], BRIDGE_HOST_AWAKE);
    }
#else
    {
        uint8_t pay[8];
        uint8_t len = 0u;

        /* 键盘侧：射频收到的 1B LED 报文 → EVT_LEDS 回传 SK32 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        sdk_ble_on_led_report(0x07u);
        pump(10u);

        CHECK(tx_find(EVT_LEDS, pay, &len));
        CHECK_EQ(len, 1u);
        CHECK_EQ(pay[0], 0x07u);
        CHECK_EQ(bridge_hid_get_leds(), 0x07u);

        /* 2.4G 上行同格式报文走同一路径 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t leds = 0x02u;
            sdk_rf24_on_recv(&leds, 1u);
        }
        pump(10u);
        CHECK(tx_find(EVT_LEDS, pay, &len));
        CHECK_EQ(pay[0], 0x02u);

        /* 长度 != 1 的报文在键盘侧不翻译（键盘侧只收主机 Output Report） */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t big[8];
            memset(big, 0x11u, sizeof(big));
            sdk_rf24_on_recv(big, 8u);
        }
        pump(10u);
        CHECK(!tx_find(EVT_LEDS, pay, &len));

        /* 心跳包（类型前缀 0x03）：仅保活，不得上报任何 EVT */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t hb = BRIDGE_RADIO_PKT_HEARTBEAT;
            sdk_rf24_on_recv(&hb, 1u);
        }
        pump(10u);
        CHECK_EQ(g_stub_u1_tx_len, 0u);

        /* 透传类型前缀（0x02）：去前缀后 EVT_TUNNEL_USB 上报 SK32 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t tp[3] = { BRIDGE_RADIO_PKT_TUNNEL, 0xCCu, 0xDDu };
            sdk_rf24_on_recv(tp, 3u);
        }
        pump(10u);
        CHECK(tx_find(EVT_TUNNEL_USB, pay, &len));
        CHECK_EQ(len, 2u);
        CHECK_EQ(pay[0], 0xCCu);
        CHECK_EQ(pay[1], 0xDDu);

        /* PC 对模组的休眠控制（BLE HID Control Point）：
         * MANUAL 策略下未授权时不许可休眠；主机 Suspend 后即许可（保持浅睡链路）。 */
        CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_MANUAL), BRIDGE_OK);
        CHECK(!bridge_mode_sleep_permitted());        /* 未指令许可 → 不许可 */

        stub_advance_ms(50u);
        stub_u1_tx_reset();
        sdk_ble_on_host_power(true);                  /* Suspend(0x00) */
        pump(10u);
        CHECK(tx_find(EVT_HOST_POWER, pay, &len));
        CHECK_EQ(len, 1u);
        CHECK_EQ(pay[0], BRIDGE_HOST_SUSPEND);
        CHECK(!bridge_mode_host_awake());
        CHECK(bridge_mode_sleep_permitted());         /* 主机挂起 → 许可休眠 */

        stub_advance_ms(50u);
        stub_u1_tx_reset();
        sdk_ble_on_host_power(false);                 /* Exit Suspend(0x01) */
        pump(10u);
        CHECK(tx_find(EVT_HOST_POWER, pay, &len));
        CHECK_EQ(pay[0], BRIDGE_HOST_AWAKE);
        CHECK(bridge_mode_host_awake());

        /* 2.4G 路径（Dongle 转发的主机状态）走同一状态机 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t hp[2] = { BRIDGE_RADIO_PKT_HOST, BRIDGE_HOST_SUSPEND };
            sdk_rf24_on_recv(hp, 2u);
        }
        pump(10u);
        CHECK(tx_find(EVT_HOST_POWER, pay, &len));
        CHECK_EQ(pay[0], BRIDGE_HOST_SUSPEND);
        CHECK(!bridge_mode_host_awake());

        /* 重复状态不重复上报 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t hp[2] = { BRIDGE_RADIO_PKT_HOST, BRIDGE_HOST_SUSPEND };
            sdk_rf24_on_recv(hp, 2u);
        }
        pump(10u);
        CHECK_EQ(g_stub_u1_tx_len, 0u);

        /* 恢复现场：主机回到 awake、策略回 AUTO */
        sdk_ble_on_host_power(false);
        pump(10u);
        CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);

        /* 空指针 / 零长度不得崩 */
        sdk_rf24_on_recv(0, 0u);
        sdk_rf24_on_recv(pay, 0u);
    }
#endif

    /* 链路回调（两侧共用） */
    sdk_rf24_on_link(true);
    CHECK(bridge_mode_is_connected());
    sdk_rf24_on_link(false);
    CHECK(!bridge_mode_is_connected());
}

/* ---------------- VIA over BLE（HOGP vendor 报表）用例 ---------------- */

/* VIA 走 BLE 的双向通路：
 *   入站  主机 write HID vendor Output(RID 0 隐式, 32B) → sdk_ble_on_raw_report
 *         → EVT_TUNNEL_BLE 上报 SK32（raw_hid_receive）
 *   出站  SK32 raw_hid_send → CMD_TUNNEL_BLE → port_tunnel_send(BLE)
 *         → HOGP vendor Input(RID 0 隐式) notify，usevia.app 经 WebHID 收包
 */
static void test_tunnel_via_ble(void)
{
    CASE("VIA 透传：BLE 出站走 HID vendor 报表(RID 0) / 入站翻译为 EVT_TUNNEL_BLE");

    radio_baseline();

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 侧透传出口只有 USB Raw HID，与 kind 无关 */
    CHECK_EQ(sdk_usb_init(), BRIDGE_OK);
    stub_usb_set_configured(true);

    {
        const uint8_t d[3] = { 0x01u, 0x02u, 0x03u };

        g_stub_usb_tx_len = 0u;
        CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_BLE, d, 3u), BRIDGE_OK);
        CHECK_EQ(g_stub_usb_tx_len, 3u);
        CHECK(memcmp(g_stub_usb_tx, d, 3u) == 0);

        g_stub_usb_tx_len = 0u;
        CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_USB, d, 3u), BRIDGE_OK);
        CHECK_EQ(g_stub_usb_tx_len, 3u);

        /* 入站（键盘/上位机方向的透传）同样送 USB Raw HID */
        g_stub_usb_tx_len = 0u;
        bridge_ch58x_radio_tunnel_in(BRIDGE_TUNNEL_BLE, d, 3u);
        CHECK_EQ(g_stub_usb_tx_len, 3u);
    }
#else
    {
        uint8_t pay[64];
        uint8_t len = 0u;
        uint8_t raw[BRIDGE_HID_RAW_REPORT_LEN];

        /* 出站：BLE → HOGP vendor 报表（RID 0 隐式，32B Input） */
        CHECK_EQ(port_radio_init(BRIDGE_RADIO_BLE), BRIDGE_OK);
        stub_ble_set_connected(true);

        memset(raw, 0u, sizeof(raw));
        raw[0]      = 0xA5u;
        raw[31]     = 0x5Au;
        CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_BLE, raw, BRIDGE_HID_RAW_REPORT_LEN),
                 BRIDGE_OK);
        CHECK_EQ(g_stub_ble_tx_len, BRIDGE_HID_RAW_REPORT_LEN);
        CHECK_EQ(g_stub_ble_tx_rid, BRIDGE_HID_RID_RAW);
        CHECK(memcmp(g_stub_ble_tx, raw, BRIDGE_HID_RAW_REPORT_LEN) == 0);

        /* 出站：USB/2.4G → 2.4G 隧道，且不污染 BLE 抓包 */
        CHECK_EQ(port_radio_init(BRIDGE_RADIO_2G4), BRIDGE_OK);
        stub_rf24_set_connected(true);
        g_stub_ble_tx_len = 0u;
        g_stub_ble_tx_rid = 0u;
        {
            const uint8_t d[2] = { 0x77u, 0x88u };

            CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_USB, d, 2u), BRIDGE_OK);
        }
        CHECK_EQ(g_stub_rf24_tx_len, 2u);
        CHECK_EQ(g_stub_rf24_tx[0], 0x77u);
        CHECK_EQ(g_stub_rf24_tx[1], 0x88u);
        CHECK_EQ(g_stub_ble_tx_len, 0u);
        CHECK_EQ(g_stub_ble_tx_rid, 0u);

        /* 参数校验：空指针 / 零长度 */
        CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_BLE, 0, 4u), BRIDGE_ERR_PARAM);
        CHECK_EQ(port_tunnel_send(BRIDGE_TUNNEL_BLE, raw, 0u), BRIDGE_ERR_PARAM);

        /* 入站：主机 write vendor Output(RID 0) → EVT_TUNNEL_BLE 上报 SK32 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t req[BRIDGE_HID_RAW_REPORT_LEN];

            memset(req, 0u, sizeof(req));
            req[0] = 0xEEu;
            req[1] = 0xFFu;
            sdk_ble_on_raw_report(req, BRIDGE_HID_RAW_REPORT_LEN);
        }
        pump(10u);
        CHECK(tx_find(EVT_TUNNEL_BLE, pay, &len));
        CHECK_EQ(len, BRIDGE_HID_RAW_REPORT_LEN);
        CHECK_EQ(pay[0], 0xEEu);
        CHECK_EQ(pay[1], 0xFFu);

        /* 超长（> 32B 上限）在 core 层被丢弃，不上报 */
        stub_advance_ms(50u);
        stub_u1_tx_reset();
        {
            uint8_t big[BRIDGE_TUNNEL_BLE_LEN + 1u];

            memset(big, 0x5Au, sizeof(big));
            sdk_ble_on_raw_report(big, (uint8_t)sizeof(big));
        }
        pump(10u);
        CHECK(!tx_find(EVT_TUNNEL_BLE, pay, &len));

        /* 空指针 / 零长度不得崩 */
        sdk_ble_on_raw_report(0, 0u);
        sdk_ble_on_raw_report(pay, 0u);
    }
#endif
}

/* ---------------- 2.4G 配对 / 绑定用例 ---------------- */

static void test_rf24_pairing(void)
{
    CASE("2.4G 配对：REQ/RESP 交换 + 绑定建链 + 陈旧应答拒绝");

    radio_baseline();
    stub_rf24_set_connected(false);

    /* 进入 2.4G 配对窗口（Dongle 配对键 / SK32 CMD_PAIR_START 同路径） */
    CHECK_EQ(bridge_mode_request((uint8_t)MODE_2G4_PAIR, true), BRIDGE_OK);
    CHECK_EQ(bridge_mode_get(), MODE_2G4_PAIR);

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：收到键盘 PAIR_REQ → 落盘 + 应答 + 建链 */
    g_stub_rf24_tx_len = 0u;
    {
        uint8_t req[9] = { RF24_PAIR_REQ,
                           0x33u, 0x44u, 0xABu, 0xCDu,     /* kbd_id */
                           0x01u, 0x02u, 0x03u, 0x04u };   /* rand  */
        uint8_t frame[10];
        frame[0] = BRIDGE_RADIO_PKT_PAIR;
        memcpy(&frame[1], req, 9u);
        sdk_rf24_on_recv(frame, 10u);
    }
    CHECK(bridge_rf24pair_is_bound());
    CHECK_EQ(g_stub_rf24_tx_len, 13u);
    CHECK_EQ(g_stub_rf24_tx[0], RF24_PAIR_RESP);
    CHECK_EQ(g_stub_rf24_tx[1], 0x33u);     /* 回显 kbd_id */
    CHECK_EQ(g_stub_rf24_tx[5], 0x01u);     /* 回显 rand */
    CHECK_EQ(bridge_mode_get(), MODE_2G4);  /* 绑定完成退出配对窗口 */
    CHECK(bridge_mode_is_connected());
#else
    /* 键盘：构造 RESP（rand 取自本端已发 REQ）→ 绑定建链 */
    g_stub_rf24_tx_len = 0u;
    bridge_rf24pair_begin_pairing();
    {
        uint8_t req[16];
        uint8_t rlen = 0u;
        bridge_rf24pair_build_req(req, &rlen);
        CHECK_EQ(rlen, 9u);
        CHECK_EQ(req[0], RF24_PAIR_REQ);

        uint8_t resp[13];
        resp[0] = RF24_PAIR_RESP;
        memcpy(&resp[1], &req[1], 4u);                 /* kbd_id 回显 */
        memcpy(&resp[5], &req[5], 4u);                 /* rand  回显 */
        memcpy(&resp[9], "\x10\x20\x30\x40", 4u);      /* key */
        uint8_t frame[14];
        frame[0] = BRIDGE_RADIO_PKT_PAIR;
        memcpy(&frame[1], resp, 13u);
        sdk_rf24_on_recv(frame, 14u);
    }
    CHECK(bridge_rf24pair_is_bound());
    CHECK_EQ(bridge_rf24pair_get()->key[0], 0x10u);
    CHECK_EQ(bridge_mode_get(), MODE_2G4);             /* 绑定完成退出配对窗口 */
    CHECK(bridge_mode_is_connected());

    /* 陈旧应答（rand 不匹配）不得落盘 */
    bridge_rf24pair_forget();
    {
        uint8_t resp[13];
        resp[0] = RF24_PAIR_RESP;
        memcpy(&resp[1], "\x00\x00\x00\x00", 4u);      /* kbd_id 无关 */
        memcpy(&resp[5], "\x99\x99\x99\x99", 4u);      /* 错误 rand */
        memcpy(&resp[9], "\x01\x01\x01\x01", 4u);
        uint8_t frame[14];
        frame[0] = BRIDGE_RADIO_PKT_PAIR;
        memcpy(&frame[1], resp, 13u);
        sdk_rf24_on_recv(frame, 14u);
    }
    CHECK(!bridge_rf24pair_is_bound());
#endif
}

/* ---------------- 休眠与配对键 ---------------- */

static void test_sleep_decision(void)
{
    CASE("休眠判定：仅 MODE_SLEEP 且无待发帧 / SRRC 期间禁眠");

    radio_baseline();
    /* 默认已为 MANUAL（关闭自动休眠）：本用例正向断言需 AUTO 才允许自动深睡 */
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);

    /* 非 MODE_SLEEP：禁止休眠（须维持连接事件） */
    CHECK(bridge_mode_get() != MODE_SLEEP);
    CHECK(!bridge_ch58x_can_sleep());

    /* 进入 MODE_SLEEP，此时 EVT_STATE 仍在待发路径上 */
    stub_advance_ms(50u);
    CHECK_EQ(bridge_mode_request((uint8_t)MODE_SLEEP, false), BRIDGE_OK);
    CHECK_EQ(bridge_mode_get(), MODE_SLEEP);
    /* 方案 A（深睡镜像）：接收活动保活窗口内禁止深睡。stub_reset 后
     * s_last_rx_ms=0，须先推进时间越过保活窗口，深睡判据才成立。 */
    stub_advance_ms(BRIDGE_RX_HOLD_AWAKE_MS + 1u);
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 UART1 数据面（EVT_STATE 不经串口外发），进入 SLEEP 即满足休眠条件 */
    CHECK(bridge_ch58x_can_sleep());
#else
    CHECK(!bridge_ch58x_can_sleep());       /* EVT_STATE 待发，串口占线 */
#endif

    /* 排空串口后可休眠 */
    for (uint8_t i = 0u; i < 6u; i++) {
        bridge_ch58x_loop();
        stub_u1_tx_drain();
    }
    CHECK(!port_uart_tx_busy());
    CHECK(bridge_ch58x_can_sleep());

    /* SRRC 定频发射期间禁止休眠 */
    CHECK_EQ(bridge_test_set_mode(1u, 19u, 2u), BRIDGE_OK);
    CHECK(bridge_test_is_active());
    CHECK(!bridge_ch58x_can_sleep());
    CHECK_EQ(bridge_test_set_mode(0u, 0u, 0u), BRIDGE_OK);
    CHECK(!bridge_test_is_active());
    CHECK(bridge_ch58x_can_sleep());        /* 退出定频后恢复可休眠 */

    {
        uint32_t s0 = stub_sleep_count();
        bridge_ch58x_sleep();
        CHECK_EQ(stub_sleep_count(), s0 + 1u);
        CHECK_EQ(stub_sleep_last_ms(), 1000u);   /* 与 1s 心跳周期对齐 */
    }
}

/* ---------------- 浅睡（IDLE）：保持链路的 20ms 有界窗口 ---------------- */
static void test_shallow_sleep(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    CASE("浅睡：Dongle 禁睡");
    radio_baseline();
    CHECK(!bridge_ch58x_shallow_ok());
#else
    CASE("浅睡：非 MODE_SLEEP 空闲时 20ms 窗口 + RXD1 唤醒开关");

    radio_baseline();
    /* 默认已为 MANUAL（关闭自动休眠）：本用例正向断言需 AUTO 才自动浅睡 */
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    CHECK(bridge_mode_get() != MODE_SLEEP);

    /* 排空初始 EVT_STATE，使 TX 空闲 */
    for (uint8_t i = 0u; i < 6u; i++) {
        bridge_ch58x_loop();
        stub_u1_tx_drain();
    }
    stub_advance_ms(BRIDGE_RX_HOLD_AWAKE_MS + 1u);
    CHECK(!port_uart_tx_busy());

    /* BLE 模式禁浅睡（回归守卫）：BLE 连接/广播事件由协议栈按固定间隔调度，
     * 本工程浅睡是"盲睡固定窗口"并会覆写协议栈的 RTC 触发时刻，且睡眠期间
     * 无法被射频事件唤醒 → 会睡过射频窗口致基带卡死。 */
    CHECK(!bridge_ch58x_shallow_ok());
    {
        uint32_t s0 = stub_sleep_count();
        bridge_ch58x_idle();
        CHECK_EQ(stub_sleep_count(), s0);          /* BLE 下不得进睡 */
    }
    /* 配对窗口（BLE_PAIR）走同一射频路径，同样禁浅睡 */
    CHECK_EQ(bridge_mode_request((uint8_t)MODE_BLE_PAIR, false), BRIDGE_OK);
    CHECK_EQ(bridge_mode_get(), MODE_BLE_PAIR);
    CHECK(!bridge_ch58x_shallow_ok());

    /* 切到 2.4G 作为浅睡正向断言基线（厂商 RF 无协议栈 RTC 事件调度） */
    CHECK_EQ(bridge_mode_request((uint8_t)MODE_2G4, false), BRIDGE_OK);
    CHECK_EQ(bridge_mode_get(), MODE_2G4);
    for (uint8_t i = 0u; i < 6u; i++) {
        bridge_ch58x_loop();
        stub_u1_tx_drain();
    }
    /* 方案 A：接收活动保活窗口内禁止浅睡。stub_reset 后 s_ms=0 且
     * s_last_rx_ms=0 → gap=0，需先推进时间越过保活窗口才能浅睡。 */
    stub_advance_ms(BRIDGE_RX_HOLD_AWAKE_MS + 1u);
    CHECK(!port_uart_tx_busy());
    CHECK(bridge_ch58x_shallow_ok());

    /* 方案 A 核心：睡眠窗口内 RXD1 被唤醒 → port_sleep_enter 续期活动时间戳
     * → 保活窗口内再次禁止浅睡（break-in 后保持清醒）。 */
    stub_uart1_rx_woke_set(true);
    bridge_ch58x_idle();                            /* 进一次浅睡并读清唤醒痕迹 */
    CHECK(bridge_uart_rx_gap_ms() < BRIDGE_RX_HOLD_AWAKE_MS);
    CHECK(!bridge_ch58x_shallow_ok());
    stub_advance_ms(BRIDGE_RX_HOLD_AWAKE_MS + 1u);  /* 越过保活窗口即恢复可浅睡 */
    CHECK(bridge_ch58x_shallow_ok());

    /* 睡眠策略 MANUAL（指令控制）：未经 CMD_SLEEP 许可不得自动浅睡 */
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_MANUAL), BRIDGE_OK);
    CHECK(!bridge_ch58x_shallow_ok());
    bridge_mode_sleep_permit(true);                 /* 模拟 CMD_SLEEP(IDLE) 置位许可 */
    CHECK(bridge_ch58x_shallow_ok());
    bridge_mode_sleep_permit(false);
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    CHECK(bridge_ch58x_shallow_ok());               /* 回 AUTO：恢复自动浅睡 */

    {
        uint32_t s0 = stub_sleep_count();
        uint32_t w0 = stub_uart1_rx_wake_count();

        bridge_ch58x_idle();
        CHECK_EQ(stub_sleep_count(), s0 + 1u);
        CHECK_EQ(stub_sleep_last_ms(), BRIDGE_SHALLOW_SLEEP_MS);
        CHECK_EQ(stub_uart1_rx_wake_count(), w0 + 2u);   /* 睡前开 / 醒后关 */
        CHECK(!stub_uart1_rx_wake_on());
    }

    /* SRRC 定频期间禁浅睡 */
    CHECK_EQ(bridge_test_set_mode(1u, 19u, 2u), BRIDGE_OK);
    CHECK(!bridge_ch58x_shallow_ok());
    {
        uint32_t s0 = stub_sleep_count();
        bridge_ch58x_idle();
        CHECK_EQ(stub_sleep_count(), s0);                /* 不得进睡 */
    }
    CHECK_EQ(bridge_test_set_mode(0u, 0u, 0u), BRIDGE_OK);
#endif
}

/* ---------------- 固件升级端口钩子（两角色都经 port→sdk 写 Stage） ---------------- */
static void test_fw_port(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    CASE("固件升级端口：Dongle 侧 sdk 桩联动（App 经 USB HID 收 FW_*）");
    {
        uint8_t d[4] = {1u, 2u, 3u, 4u};

        CHECK_EQ(port_fw_begin(0u), BRIDGE_ERR_PARAM);        /* 长度 0 拒绝 */
        CHECK_EQ(port_fw_begin(64u), BRIDGE_OK);
        CHECK_EQ(port_fw_write(0u, d, 4u), BRIDGE_OK);
        CHECK_EQ(port_fw_write(0u, d, 4u), BRIDGE_ERR_PARAM); /* 偏移必须顺序推进 */
        port_fw_abort();
    }
#else
    CASE("固件升级端口：键盘侧 sdk 桩联动 + 核心状态机贯通");
    {
        uint8_t  img[16];
        uint8_t  frame[4u + 16u];
        uint16_t crc;
        uint32_t f0 = stub_fw_finish_count();

        for (uint8_t i = 0u; i < 16u; i++) {
            img[i] = (uint8_t)(0x30u + i);
        }
        crc = bridge_crc16_ccitt(img, 16u);

        CHECK_EQ(port_fw_begin(16u), BRIDGE_OK);
        CHECK_EQ(port_fw_write(0u, img, 16u), BRIDGE_OK);

        {
            uint8_t bp[6];

            bp[0] = 16u; bp[1] = 0u; bp[2] = 0u; bp[3] = 0u;
            bp[4] = (uint8_t)(crc & 0xFFu); bp[5] = (uint8_t)(crc >> 8);
            CHECK_EQ(bridge_ota_begin(bp, 6u), BRIDGE_OK);

            frame[0] = 0u; frame[1] = 0u; frame[2] = 0u; frame[3] = 0u;
            memcpy(&frame[4], img, 16u);
            CHECK_EQ(bridge_ota_data(frame, (uint8_t)sizeof(frame)), BRIDGE_OK);
            CHECK_EQ(bridge_ota_end(0, 0u), BRIDGE_OK);

            bridge_ota_apply();
            CHECK_EQ(stub_fw_finish_count(), f0 + 1u);
            CHECK_EQ(stub_fw_finish_crc(), crc);
        }
    }
#endif
}

#if BRIDGE_BOARD_HAS_PAIR_KEY
static void test_pair_key(void)
{
    CASE("Dongle 配对键：20ms 去抖 → 2.4G 配对窗口");

    radio_baseline();

    /* 按下（低有效）但不足去抖窗口：不得触发 */
    stub_gpio_set_in(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM, false);
    stub_advance_ms(1u);
    bridge_ch58x_loop();
    CHECK(bridge_mode_get() != MODE_2G4_PAIR);

    /* 持续按下跨过 20ms：进入 2.4G 配对窗口并触发厂商配对流程 */
    stub_advance_ms(30u);
    bridge_ch58x_loop();
    CHECK_EQ(bridge_mode_get(), MODE_2G4_PAIR);
    CHECK(stub_rf24_init_count() >= 1u);
    CHECK(stub_rf24_pair_count() >= 1u);

    /* 松开不改变已进入的配对窗口 */
    stub_gpio_set_in(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM, true);
    stub_advance_ms(30u);
    bridge_ch58x_loop();
    CHECK_EQ(bridge_mode_get(), MODE_2G4_PAIR);
}
#endif

/* ---------------- BLE 多主机 4 槽标志（bridge_ble_multihost） ---------------- */
static void test_ble_multihost(void)
{
    CASE("BLE 多主机 4 槽标志");
    stub_flash_clear();
    bridge_mh_init();

    /* 初始：无任何已配对 Profile */
    for (uint8_t p = 1u; p <= BRIDGE_MH_PROFILE_MAX; p++) {
        CHECK(!bridge_mh_has(p));
    }
    CHECK_EQ(bridge_mh_active(), 0);

    /* 非法入参 */
    CHECK_EQ(bridge_mh_mark_bound(0u), BRIDGE_MH_ERR_PARAM);
    CHECK_EQ(bridge_mh_mark_bound(5u), BRIDGE_MH_ERR_PARAM);
    CHECK_EQ(bridge_mh_set_active(0u), BRIDGE_MH_ERR_PARAM);
    CHECK_EQ(bridge_mh_erase(5u), BRIDGE_MH_ERR_PARAM);

    /* 标记 slot1 已配对 → has=1 且成为活跃槽 */
    CHECK_EQ(bridge_mh_mark_bound(1u), BRIDGE_MH_OK);
    CHECK(bridge_mh_has(1u));
    CHECK_EQ(bridge_mh_active(), 1u);

    /* 标记 slot2 → slot1 保持，活跃槽变为 2（多主机互不覆盖） */
    CHECK_EQ(bridge_mh_mark_bound(2u), BRIDGE_MH_OK);
    CHECK(bridge_mh_has(1u) && bridge_mh_has(2u));
    CHECK_EQ(bridge_mh_active(), 2u);

    /* 切活跃槽不影响已配对标志 */
    CHECK_EQ(bridge_mh_set_active(1u), BRIDGE_MH_OK);
    CHECK_EQ(bridge_mh_active(), 1u);
    CHECK(bridge_mh_has(2u));

    /* 掉电重载：init 从 NV 索引表恢复 */
    bridge_mh_init();
    CHECK(bridge_mh_has(1u) && bridge_mh_has(2u));
    CHECK_EQ(bridge_mh_active(), 1u);

    /* 解除单槽：slot1 标记清 0，且它是活跃槽 → last_active 归 0；slot2 仍在 */
    CHECK_EQ(bridge_mh_erase(1u), BRIDGE_MH_OK);
    CHECK(!bridge_mh_has(1u));
    CHECK(bridge_mh_has(2u));
    CHECK_EQ(bridge_mh_active(), 0u);

    /* 索引表 CRC 失效（模拟写坏）→ init 重建空表（掉电保护）
     * 此刻 status[] = [0,1,0,0]（slot2 仍在），改写 status[1]（偏移 4）1→0，
     * 数据变了但 CRC 仍是旧值 → 校验失败 → 重建空表。 */
    {
        uint32_t addr = NV_TEST_BASE + (uint32_t)BRIDGE_NV_ID_MH_IDX * 64u + 4u;
        uint8_t  bad  = 0x00u;                  /* 覆盖 status[1]，使 CRC 不符 */
        stub_flash_poke(addr, &bad, 1u);
        bridge_mh_init();
        CHECK(!bridge_mh_has(2u));
        CHECK_EQ(bridge_mh_active(), 0u);
    }

    /* 解除全部 */
    CHECK_EQ(bridge_mh_mark_bound(1u), BRIDGE_MH_OK);
    CHECK_EQ(bridge_mh_erase_all(), BRIDGE_MH_OK);
    for (uint8_t p = 1u; p <= BRIDGE_MH_PROFILE_MAX; p++) {
        CHECK(!bridge_mh_has(p));
    }
    CHECK_EQ(bridge_mh_active(), 0u);
}

/* ---------------- BLE 多主机槽状态查询（port_ble_slots） ---------------- */
static void test_ble_slots(void)
{
    uint8_t s[5];

    CASE("BLE 多主机槽状态查询 port_ble_slots");

    CHECK_EQ(port_ble_slots(NULL), BRIDGE_ERR_PARAM);

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 BLE 主机绑定 → UNSUPPORTED */
    CHECK_EQ(port_ble_slots(s), BRIDGE_ERR_UNSUPPORTED);
#else
    stub_flash_clear();
    bridge_mh_init();

    /* 初始：4 槽全空、无活跃 */
    CHECK_EQ(port_ble_slots(s), BRIDGE_OK);
    CHECK_EQ(s[0], 0u); CHECK_EQ(s[1], 0u); CHECK_EQ(s[2], 0u); CHECK_EQ(s[3], 0u);
    CHECK_EQ(s[4], 0u);

    /* 标记 Profile 1、3 已配对 → 对应位为 1，last_active 跟随最近标记(3) */
    CHECK_EQ(bridge_mh_mark_bound(1u), BRIDGE_MH_OK);
    CHECK_EQ(bridge_mh_mark_bound(3u), BRIDGE_MH_OK);
    CHECK_EQ(port_ble_slots(s), BRIDGE_OK);
    CHECK_EQ(s[0], 1u); CHECK_EQ(s[1], 0u); CHECK_EQ(s[2], 1u); CHECK_EQ(s[3], 0u);
    CHECK_EQ(s[4], 3u);

    /* 解除 Profile 3 → s3 归零，且因它正是活跃槽 → last_active 归 0 */
    CHECK_EQ(bridge_mh_erase(3u), BRIDGE_MH_OK);
    CHECK_EQ(port_ble_slots(s), BRIDGE_OK);
    CHECK_EQ(s[0], 1u); CHECK_EQ(s[2], 0u);
    CHECK_EQ(s[4], 0u);

    /* 全部解除 → 回到全空 */
    CHECK_EQ(bridge_mh_erase_all(), BRIDGE_MH_OK);
    CHECK_EQ(port_ble_slots(s), BRIDGE_OK);
    CHECK_EQ(s[0], 0u); CHECK_EQ(s[1], 0u); CHECK_EQ(s[2], 0u); CHECK_EQ(s[3], 0u);
    CHECK_EQ(s[4], 0u);
#endif
}

int main(void)
{
    printf("=== CH582F bridge port test (board=%s, baud=%lu, role=%s) ===\n",
           BRIDGE_BOARD_NAME, (unsigned long)BRIDGE_UART_BAUD,
           BRIDGE_BOARD_IS_DONGLE ? "dongle" : "keyboard");

    /* 必须首个运行：port 层的日志 UART0 惰性初始化全程只发生一次 */
    stub_reset();
    test_log_lazy_init();

    test_uart_init();
#if !BRIDGE_BOARD_IS_DONGLE
    test_uart_tx_backpressure();
    test_uart_isr_rx();
#endif
    test_nv_rw();
    test_nv_param_and_errors();
    test_battery();
    test_gpio();
    test_chip_and_system();

    test_radio_init();
    test_radio_send();
    test_radio_pair_profile();
    test_radio_report_period();
    test_radio_event();
    test_callback_translation();
    test_tunnel_via_ble();
    test_rf24_pairing();
    test_ble_multihost();
    test_ble_slots();
    test_sleep_decision();
    test_shallow_sleep();
    test_fw_port();
#if BRIDGE_BOARD_HAS_PAIR_KEY
    test_pair_key();
#endif

    printf("=== checks: %d passed, %d failed ===\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
