/* 主机端功能验证：协议帧机 / D14 时序 / HID 去重与降级 / 模式切换 / 日志开关
 *
 * 编译运行见 test_host/Makefile 的 `make test`。
 */
#include <stdio.h>
#include <string.h>

#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_proto.h"
#include "bridge_uart.h"
#include "bridge_hid.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"
#include "bridge_power.h"
#include "bridge_test.h"
#include "bridge_log.h"
#include "bridge_crc16.h"
#include "bridge_ringbuf.h"
#include "bridge_app.h"
#include "bridge_port.h"
#include "bridge_rf24pair.h"
#include "host_port.h"

/* ---------------- 迷你测试框架 ---------------- */
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

/* 关键指标：D14 —— 16B 帧 @921600 占线时间 */
static void test_bandwidth_budget(void)
{
    CASE("D14 带宽预算");
    /* 10 bit/byte 921600bps → 92160 B/s */
    CHECK_EQ(921600u / 10u, 92160u);

    uint8_t buf[128];
    uint16_t n = bridge_uart_encode(buf, sizeof(buf), CMD_HID_KEYBOARD, 0u, 0u,
                                    (const uint8_t *)"12345678", BRIDGE_REPORT_KEYBOARD_LEN);
    CHECK_EQ(n, 16u);                       /* 2 SOF + 4 HDR + 8 payload + 2 CRC */
    n = bridge_uart_encode(buf, sizeof(buf), CMD_HID_NKRO, 0u, 0u, buf,
                           BRIDGE_REPORT_NKRO_LEN);
    CHECK_EQ(n, 24u);

    /* 占线时间(us) = 帧长 * 10 * 1e6 / baud */
    CHECK_EQ((16u * 10u * 1000000u) / 921600u, 173u);   /* 远小于 4.5ms KPI */
    CHECK_EQ((24u * 10u * 1000000u) / 921600u, 260u);
}

static void inject(uint8_t cmd, uint8_t seq, uint8_t flags,
                   const uint8_t *payload, uint8_t len);

static void reset_all(void)
{
    host_nv_clear();
    host_radio_reset();
    host_radio_clear_links();
    host_tx_reset();
    host_log_reset();
    host_name_reset();
    host_time_reset();
    host_set_vbus(false);
    host_set_mode_sel(1u);          /* 01 = BLE */
    host_set_bat_mv(3800u);
    host_set_charger(false);
    host_uart_force_busy(false);
    bridge_app_init();

    /* 让上电 EVT_STATE 帧发完（12B@921600 ≈ 0.13ms），避免抢占首个用例的发送窗口 */
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 5u);
    bridge_proto_poll();
    /* 模拟 SK32 对上电 EVT_STATE 的应答，清掉 ACK 追踪，避免后续用例被重试干扰 */
    inject(EVT_STATE, 0u, BRIDGE_FLAG_ACK, 0, 0u);
    bridge_app_poll();

    /* 用例从干净的时间轴与抓包缓冲开始 */
    host_tx_reset();
    host_log_reset();
}

/* 模拟"厂商射频链路已连接"并投递连接事件 */
static void connect_radio(void)
{
    uint8_t r = BRIDGE_RADIO_NONE;
    bridge_mode_t m = bridge_mode_get();

    if (m == MODE_BLE || m == MODE_BLE_PAIR) {
        r = BRIDGE_RADIO_BLE;
    } else if (m == MODE_2G4 || m == MODE_2G4_PAIR) {
        r = BRIDGE_RADIO_2G4;
    }
    if (r != BRIDGE_RADIO_NONE) {
        host_radio_set_connected(r, true);
    }
    bridge_mode_on_radio_event(BRIDGE_RADIO_EVT_CONNECTED);
}

/* 构造并注入一帧 */
static void inject(uint8_t cmd, uint8_t seq, uint8_t flags,
                   const uint8_t *payload, uint8_t len)
{
    uint8_t raw[BRIDGE_FRAME_OVERHEAD + BRIDGE_MAX_PAYLOAD];
    uint16_t n = bridge_uart_encode(raw, sizeof(raw), cmd, seq, flags, payload, len);
    host_uart_inject(raw, n);
}

/* 注入一条命令并跑主循环（两轮，跨过最小帧间隔后再排空发送队列）。
 * 按 D14，命令的应答/状态事件可能排在彼此后面，一轮不足以全部落地。 */
static void run_cmd(uint8_t cmd, uint8_t seq, uint8_t flags,
                    const uint8_t *payload, uint8_t len)
{
    inject(cmd, seq, flags, payload, len);
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1u);
    bridge_app_poll();
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1u);
    bridge_app_poll();
}

/* 反复推进时间并跑主循环，直到 TX 出现指定帧（最多 rounds 轮） */
static int pump_find(uint8_t cmd, uint8_t *flags, uint8_t *pay, uint8_t *len,
                     uint8_t rounds)
{
    for (uint8_t i = 0u; i < rounds; i++) {
        if (host_tx_find_frame(cmd, flags, pay, len)) {
            return 1;
        }
        host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1u);
        bridge_app_poll();
    }
    return host_tx_find_frame(cmd, flags, pay, len);
}

/* ---------------- 用例 ---------------- */

static void test_crc16(void)
{
    CASE("CRC-16/CCITT-FALSE");
    const uint8_t v[] = "123456789";
    CHECK_EQ(bridge_crc16_ccitt(v, 9u), 0x29B1u);
    CHECK_EQ(bridge_crc16_ccitt(0, 0u), 0xFFFFu);
}

static void test_ringbuf(void)
{
    CASE("环形缓冲");
    uint8_t store[4];
    bridge_ringbuf_t rb;
    bridge_ringbuf_init(&rb, store, sizeof(store));
    CHECK_EQ(bridge_ringbuf_space(&rb), 3u);      /* 留 1 字节区分满/空 */
    CHECK(bridge_ringbuf_push(&rb, 'a'));
    CHECK(bridge_ringbuf_push(&rb, 'b'));
    CHECK(bridge_ringbuf_push(&rb, 'c'));
    CHECK(!bridge_ringbuf_push(&rb, 'd'));        /* 满 */
    CHECK_EQ(bridge_ringbuf_count(&rb), 3u);
    uint8_t b = 0u;
    CHECK(bridge_ringbuf_pop(&rb, &b) && b == 'a');
    CHECK(bridge_ringbuf_push(&rb, 'e'));
    CHECK(bridge_ringbuf_pop(&rb, &b) && b == 'b');
    CHECK(bridge_ringbuf_pop(&rb, &b) && b == 'c');
    CHECK(bridge_ringbuf_pop(&rb, &b) && b == 'e');
    CHECK(!bridge_ringbuf_pop(&rb, &b));
}

static void test_frame_roundtrip(void)
{
    CASE("帧编解码回环");
    reset_all();

    const uint8_t pay[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    inject(CMD_SET_PROFILE, 0x2Au, BRIDGE_FLAG_ACK_REQ, pay, 4u);

    bridge_frame_t f;
    memset(&f, 0, sizeof(f));
    CHECK(bridge_uart_get_frame(&f));
    CHECK_EQ(f.cmd, CMD_SET_PROFILE);
    CHECK_EQ(f.seq, 0x2Au);
    CHECK_EQ(f.flags, BRIDGE_FLAG_ACK_REQ);
    CHECK_EQ(f.len, 4u);
    CHECK(memcmp(f.payload, pay, 4u) == 0);
    CHECK(!bridge_uart_get_frame(&f));    /* 无更多帧 */
}

static void test_sof_resync(void)
{
    CASE("SOF 重同步 / 载荷含 AA55");
    reset_all();

    /* 垃圾前缀 + 载荷里塞 AA 55，长度定长读取不应误判 */
    uint8_t junk[3] = { 0x00u, 0xAAu, 0x7Fu };
    host_uart_inject(junk, 3u);

    const uint8_t pay[8] = { 0xAAu, 0x55u, 0xAAu, 0xAAu, 0x55u, 0x01u, 0x02u, 0x03u };
    inject(CMD_HID_KEYBOARD, 0x11u, 0u, pay, 8u);

    bridge_frame_t f;
    CHECK(bridge_uart_get_frame(&f));
    CHECK_EQ(f.cmd, CMD_HID_KEYBOARD);
    CHECK_EQ(f.seq, 0x11u);
    CHECK_EQ(f.len, 8u);
    CHECK(memcmp(f.payload, pay, 8u) == 0);
    CHECK_EQ(bridge_uart_crc_error_count(), 0u);
}

static void test_crc_error(void)
{
    CASE("CRC 错误丢弃与计数");
    reset_all();

    uint8_t raw[32];
    uint16_t n = bridge_uart_encode(raw, sizeof(raw), CMD_GET_STATUS, 1u, 0u, 0, 0u);
    raw[4] ^= 0xFFu;   /* 破坏 CMD 字节（在 CRC 覆盖范围内） */
    host_uart_inject(raw, n);

    bridge_frame_t f;
    CHECK(!bridge_uart_get_frame(&f));
    CHECK_EQ(bridge_uart_crc_error_count(), 1u);

    /* 坏帧后仍能解析下一帧 */
    inject(CMD_GET_STATUS, 2u, 0u, 0, 0u);
    CHECK(bridge_uart_get_frame(&f));
    CHECK_EQ(f.seq, 2u);
}

static void test_half_frame_timeout(void)
{
    CASE("半帧超时切分");
    reset_all();

    uint8_t raw[32];
    uint16_t n = bridge_uart_encode(raw, sizeof(raw), CMD_GET_STATUS, 1u, 0u, 0, 0u);
    host_uart_inject(raw, 4u);            /* 只喂前 4 字节 */

    bridge_frame_t f;
    CHECK(!bridge_uart_get_frame(&f));

    host_advance_ms(BRIDGE_RX_HALF_FRAME_MS + 1u);

    inject(CMD_GET_VERSION, 3u, 0u, 0, 0u);   /* 新帧应当能被完整解析 */
    CHECK(bridge_uart_get_frame(&f));
    CHECK_EQ(f.cmd, CMD_GET_VERSION);
    CHECK_EQ(f.seq, 3u);
    CHECK(bridge_uart_drop_count() >= 1u);
    (void)n;
}

static void test_tx_busy_and_gap(void)
{
    CASE("D14 帧间隔与忙判定");
    reset_all();
    host_advance_ms(10u);          /* 越过 init EVT_STATE 的占线 */

    CHECK(bridge_uart_tx_ready());
    CHECK_EQ(bridge_uart_send(CMD_PING, 1u, 0u, 0, 0u), BRIDGE_OK);

    /* 8B 帧占线 2ms：立即再发 → BUSY */
    CHECK_EQ(bridge_uart_send(CMD_PING, 2u, 0u, 0, 0u), BRIDGE_ERR_BUSY);

    host_advance_ms(2u);
    CHECK(!bridge_uart_tx_ready());               /* 未过 5ms 最小间隔 */
    CHECK_EQ(bridge_uart_send(CMD_PING, 3u, 0u, 0, 0u), BRIDGE_ERR_GAP);

    host_advance_ms(3u);                          /* 累计 5ms */
    CHECK(bridge_uart_tx_ready());
    CHECK_EQ(bridge_uart_send(CMD_PING, 4u, 0u, 0, 0u), BRIDGE_OK);
}

static void test_hid_dedup(void)
{
    CASE("HID 去重（D14）");
    reset_all();
    connect_radio();

    const uint8_t press[8]  = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };
    const uint8_t press2[8] = { 0u, 0u, 0x04u, 0x05u, 0u, 0u, 0u, 0u };

    host_radio_reset();
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_OK);
    CHECK_EQ(g_host_radio_count, 1u);
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_ERR_DUP);
    CHECK_EQ(g_host_radio_count, 1u);
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press2, 8u), BRIDGE_OK);
    CHECK_EQ(g_host_radio_count, 2u);

    /* 全 0 释放后，再次按下同一键必须能通过（缓存已清） */
    CHECK_EQ(bridge_hid_release_all(), BRIDGE_OK);
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press2, 8u), BRIDGE_OK);

    /* 长度与命令不匹配必须拒绝 */
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 4u), BRIDGE_ERR_PARAM);
}

static void test_nkro_downgrade(void)
{
    CASE("NKRO 位图 → 6KRO 降级");
    uint8_t boot[8];

    /* 7 个键（usage 4..10）→ 6 键 + Rollover */
    uint8_t nkro7[16];
    memset(nkro7, 0, sizeof(nkro7));
    nkro7[0] = 0x02u;      /* 左 Shift */
    nkro7[1] = 0x7Fu;      /* usage 4..10 全按 */
    memset(boot, 0xAAu, sizeof(boot));
    CHECK_EQ(bridge_hid_nkro_to_boot(nkro7, boot), 6u);
    CHECK_EQ(boot[0], 0x02u);
    CHECK_EQ(boot[1], BRIDGE_HID_ROLLOVER_ERROR);
    CHECK_EQ(boot[2], 4u);
    CHECK_EQ(boot[7], 9u);   /* 前 6 个 usage: 4 5 6 7 8 9 */

    /* usage 超出 Boot 可表达范围（>0x65）应被丢弃 */
    uint8_t nkro_hi[16];
    memset(nkro_hi, 0, sizeof(nkro_hi));
    nkro_hi[15] = 0x80u;   /* bit 最高位 → usage = 4 + 14*8 + 7 = 123 (0x7B) > 0x65 */
    CHECK_EQ(bridge_hid_nkro_to_boot(nkro_hi, boot), 0u);
    CHECK_EQ(boot[1], 0u);
}

static void test_nkro_mode_routing(void)
{
    CASE("NKRO 在 BLE 降级 / 2.4G 直传（D3+D4）");
    reset_all();
    /* 归一化起始模式为 BLE（Dongle 上电默认 2G4，本用例测的是 core 路由逻辑） */
    CHECK_EQ(bridge_mode_request(MODE_BLE, false), BRIDGE_OK);
    connect_radio();

    uint8_t nkro[16];
    memset(nkro, 0, sizeof(nkro));
    nkro[0] = 0x02u;
    nkro[1] = 0x03u;   /* usage 4('a'), 5('b') */

    /* BLE：降级为 8B Boot 报文 */
    host_radio_reset();
    CHECK_EQ(bridge_hid_send(CMD_HID_NKRO, nkro, 16u), BRIDGE_OK);
    CHECK_EQ(g_host_radio_count, 1u);
    CHECK_EQ(g_host_radio[0].mode, BRIDGE_RADIO_BLE);
    CHECK_EQ(g_host_radio[0].len, 8u);
    CHECK_EQ(g_host_radio[0].data[0], 0x02u);
    CHECK_EQ(g_host_radio[0].data[2], 4u);
    CHECK_EQ(g_host_radio[0].data[3], 5u);
    CHECK_EQ(g_host_radio[0].data[4], 0u);

    /* 切到 2.4G：NKRO 16B 直传 */
    CHECK_EQ(bridge_mode_request(MODE_2G4, false), BRIDGE_OK);
    connect_radio();
    host_radio_reset();
    CHECK_EQ(bridge_hid_send(CMD_HID_NKRO, nkro, 16u), BRIDGE_OK);
    CHECK_EQ(g_host_radio_count, 1u);
    CHECK_EQ(g_host_radio[0].mode, BRIDGE_RADIO_2G4);
    CHECK_EQ(g_host_radio[0].len, 16u);
    CHECK(memcmp(g_host_radio[0].data, nkro, 16u) == 0);
}

static void test_release_before_mode_switch(void)
{
    CASE("模式切换前全键释放（防卡键）");
    reset_all();
    /* 归一化起始模式为 BLE，保证"按 → 切 2G4"确实跨模式（Dongle 默认 2G4） */
    CHECK_EQ(bridge_mode_request(MODE_BLE, false), BRIDGE_OK);
    connect_radio();

    const uint8_t press[8] = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };
    host_radio_reset();
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_OK);
    CHECK_EQ(g_host_radio_count, 1u);

    CHECK_EQ(bridge_mode_request(MODE_2G4, false), BRIDGE_OK);

    /* 切换事务第 1 步必须先发全 0 释放报文 */
    CHECK(g_host_radio_count >= 2u);
    CHECK_EQ(g_host_radio[1].len, 8u);
    bool all_zero = true;
    for (uint8_t i = 0u; i < 8u; i++) {
        if (g_host_radio[1].data[i] != 0u) {
            all_zero = false;
        }
    }
    CHECK(all_zero);
}

static void test_vbus_priority(void)
{
    CASE("VBUS 优先级（规格 5.2）");
    reset_all();
    /* 上电默认无线模式按板级：键盘=BLE / Dongle=2G4 */
    CHECK_EQ(bridge_mode_get(), (bridge_mode_t)bridge_mode_default());

    host_set_vbus(true);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), MODE_USB);

    /* VBUS 在场拒绝切无线 */
    CHECK_EQ(bridge_mode_request(MODE_BLE, true), BRIDGE_ERR_VBUS);
    CHECK_EQ(bridge_mode_get(), MODE_USB);

    /* 拔出后回到记忆模式 */
    host_set_vbus(false);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), (bridge_mode_t)bridge_mode_default());
    CHECK(bridge_mode_is_wireless());
}

static void test_proto_get_version(void)
{
    CASE("GET_VERSION 应答帧");
    reset_all();

    run_cmd(CMD_GET_VERSION, 9u, BRIDGE_FLAG_ACK_REQ, 0, 0u);

    uint8_t flags = 0u, pay[16], len = 0u;
    CHECK(pump_find(CMD_GET_VERSION, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    CHECK_EQ(len, 3u);
    CHECK_EQ(pay[0], 0u);
    CHECK_EQ(pay[1], 1u);
    CHECK_EQ(pay[2], 0u);
}

static void test_proto_ping_pong(void)
{
    CASE("PING → PONG 与链路活性");
    reset_all();

    run_cmd(CMD_PING, 0x42u, 0u, 0, 0u);

    uint8_t flags = 0u, len = 0u;
    CHECK(pump_find(EVT_PONG, &flags, 0, &len, 4u));
    CHECK(bridge_uart_link_check());       /* 收到过合法帧 → 正常 */
    CHECK(bridge_uart_is_link_alive());
}

static void test_proto_unsupported(void)
{
    CASE("不支持命令 → 错误帧");
    reset_all();

    run_cmd(0x55u, 7u, BRIDGE_FLAG_ACK_REQ, 0, 0u);

    uint8_t flags = 0u, pay[4], len = 0u;
    CHECK(pump_find(0x55u, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
    CHECK_EQ(len, 1u);
    CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
}

static void test_get_mac(void)
{
    CASE("GET_MAC 返回 6 字节 MAC");
    reset_all();

    run_cmd(CMD_GET_MAC, 5u, BRIDGE_FLAG_ACK_REQ, 0, 0u);

    uint8_t pay[8], len = 0u, flags = 0u;
    CHECK(pump_find(CMD_GET_MAC, &flags, pay, &len, 4u));
    CHECK_EQ(len, 6u);
    CHECK_EQ(pay[4], 0xABu);
    CHECK_EQ(pay[5], 0xCDu);
}

static void test_link_lost_to_sleep(void)
{
    CASE("UART 失联 → 释放按键并休眠（规格 6.4；Dongle 无上游不睡）");
    reset_all();
#if !BRIDGE_BOARD_IS_DONGLE
    /* 默认已为 MANUAL（关闭自动休眠）：本用例验证 AUTO 策略下失联安全休眠 */
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
#endif
    connect_radio();

    const uint8_t press[8] = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_OK);

    host_radio_reset();
    bridge_uart_on_rx_frame();     /* 先确认链路可用，再停止心跳 */

    for (uint8_t i = 0u; i < BRIDGE_LINK_LOST_CNT + 1u; i++) {
        host_advance_ms(BRIDGE_PING_PERIOD_MS);
        bridge_app_poll();
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 SK32 上游：不做 UART 心跳/断链判定，失联不得入睡 */
    CHECK(bridge_mode_get() != MODE_SLEEP);
#else
    CHECK_EQ(bridge_mode_get(), MODE_SLEEP);
    /* 失联时必须已发全 0 释放报文 */
    CHECK(g_host_radio_count >= 1u);
    CHECK_EQ(g_host_radio[0].len, 8u);
    bool all_zero = true;
    for (uint8_t i = 0u; i < 8u; i++) {
        if (g_host_radio[0].data[i] != 0u) {
            all_zero = false;
        }
    }
    CHECK(all_zero);
#endif
}

static void test_ping_deferral(void)
{
    CASE("串口忙时 PING 延后重试");
    reset_all();
    host_advance_ms(10u);
    host_tx_reset();

    host_uart_force_busy(true);
    bridge_uart_ping();
    CHECK_EQ(host_tx_frame_count(), 0u);     /* 忙 → 未发出 */

    host_uart_force_busy(false);
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS);
    bridge_uart_poll();

    uint8_t len = 0u;
    CHECK(host_tx_find_frame(CMD_PING, 0, 0, &len));
}

static void test_control_queue_when_busy(void)
{
    CASE("串口不可用时控制帧排队重发");
    reset_all();
    host_advance_ms(10u);

    host_uart_force_busy(true);
    inject(CMD_GET_VERSION, 1u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
    inject(CMD_GET_VERSION, 2u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
    bridge_app_poll();
    CHECK_EQ(host_tx_frame_count(), 0u);

    host_uart_force_busy(false);
    host_advance_ms(200u);
    bridge_proto_poll();                       /* 队列只放行间隔允许的帧 */
    CHECK_EQ(host_tx_frame_count(), 1u);

    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS);  /* 跨过最小帧间隔 */
    bridge_proto_poll();
    CHECK_EQ(host_tx_frame_count(), 2u);
}

static void test_log_switch(void)
{
    CASE("日志等级运行期开关（D13）");
    reset_all();

    CHECK_EQ(bridge_log_get_level(), BRIDGE_LOG_LEVEL);
    CHECK(BRIDGE_LOG_ENABLE == 1);

    /* SET_CONFIG(LOG_LEVEL=1) */
    uint8_t pay[2] = { BRIDGE_CFG_KEY_LOG_LEVEL, BRIDGE_LOG_WARN };
    run_cmd(CMD_SET_CONFIG, 1u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
    CHECK_EQ(bridge_log_get_level(), BRIDGE_LOG_WARN);

    host_log_reset();
    LOG_I("test", "should be suppressed");
    CHECK_EQ(g_host_log_lines, 0u);
    LOG_E("test", "should appear");
    CHECK_EQ(g_host_log_lines, 1u);

    /* 非法 key → 错误帧 */
    pay[0] = 0x77u;
    host_tx_reset();
    run_cmd(CMD_SET_CONFIG, 2u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
    uint8_t flags = 0u;
    CHECK(pump_find(CMD_SET_CONFIG, &flags, 0, 0, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
}

static void test_cfg_persist(void)
{
    CASE("配置掉电保持 + CRC 校验");
    reset_all();

    CHECK_EQ(bridge_cfg_set_profile(3u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_set_rf24_report_ms(2u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_get()->ble_profile, 3u);

    /* 重新 init 应读回 */
    bridge_cfg_init();
    CHECK_EQ(bridge_cfg_get()->ble_profile, 3u);
    CHECK_EQ(bridge_cfg_get()->rf24_report_ms, 2u);

    /* 破坏 NV 后应回落默认值并由 CRC 检出 */
    host_nv_clear();
    bridge_cfg_init();
    CHECK_EQ(bridge_cfg_get()->ble_profile, 1u);

    CHECK_EQ(bridge_cfg_set_profile(9u), BRIDGE_ERR_PARAM);
    CHECK_EQ(bridge_cfg_set_rf24_report_ms(0u), BRIDGE_ERR_PARAM);
}

static void test_battery_curve(void)
{
    CASE("电量曲线与阈值");
    CHECK_EQ(bridge_power_mv_to_percent(4200u), 100u);
    CHECK_EQ(bridge_power_mv_to_percent(3830u), 50u);
    CHECK_EQ(bridge_power_mv_to_percent(3700u), 20u);
    CHECK_EQ(bridge_power_mv_to_percent(3400u), 5u);
    CHECK_EQ(bridge_power_mv_to_percent(3000u), 0u);

    reset_all();
    host_set_bat_mv(3650u);      /* 10% → 低电告警 */
    bridge_power_sample();
    CHECK(bridge_power_alarm_flags() & BRIDGE_BATT_ALARM_LOW);
    CHECK_EQ(bridge_power_battery_percent(), 10u);

    host_set_bat_mv(3300u);      /* <5% → 严重告警 */
    bridge_power_sample();
    CHECK(bridge_power_alarm_flags() & BRIDGE_BATT_ALARM_CRIT);
}

static void test_evt_battery_report(void)
{
    CASE("EVT_BATTERY 周期上报");
    reset_all();

    host_advance_ms(BRIDGE_BATT_SAMPLE_MS + 1u);
    bridge_app_poll();                     /* 采样并构造 EVT_BATTERY */
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1u);
    bridge_app_poll();                     /* 跨过帧间隔后真正发出 */

    uint8_t pay[8], len = 0u, flags = 0u;
    CHECK(pump_find(EVT_BATTERY, &flags, pay, &len, 4u));
    CHECK_EQ(len, 3u);
    CHECK_EQ(pay[0], (uint8_t)(3800u & 0xFFu));
    CHECK_EQ(pay[1], (uint8_t)(3800u >> 8));
    CHECK_EQ(pay[2], bridge_power_battery_percent());
}

static void test_srrc_test_mode(void)
{
    CASE("SRRC 定频测试模式（D9）");
    reset_all();
    connect_radio();

    CHECK(!bridge_test_is_active());
    CHECK_EQ(bridge_test_set_mode(1u, 19u, 2u), BRIDGE_OK);
    CHECK(bridge_test_is_active());
    CHECK_EQ(bridge_test_channel(), 19u);

    /* 定频期间禁止正常转发 */
    CHECK_EQ(bridge_mode_radio_id(), BRIDGE_RADIO_NONE);
    const uint8_t press[8] = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_ERR_STATE);

    CHECK_EQ(bridge_test_set_mode(1u, 40u, 2u), BRIDGE_ERR_PARAM);   /* 信道越界 */
    CHECK_EQ(bridge_test_set_mode(1u, 19u, 9u), BRIDGE_ERR_PARAM);   /* 功率越界 */

    CHECK_EQ(bridge_test_set_mode(0u, 0u, 0u), BRIDGE_OK);
    CHECK(!bridge_test_is_active());
    CHECK(bridge_mode_radio_id() != BRIDGE_RADIO_NONE);
}

static void test_set_mode_via_uart(void)
{
    CASE("SET_MODE 命令事务");
    reset_all();
    connect_radio();

    uint8_t pay[2] = { (uint8_t)MODE_2G4, 0u };
    host_tx_reset();
    run_cmd(CMD_SET_MODE, 4u, BRIDGE_FLAG_ACK_REQ, pay, 2u);

    CHECK_EQ(bridge_mode_get(), MODE_2G4);
    uint8_t flags = 0u;
    CHECK(pump_find(CMD_SET_MODE, &flags, 0, 0, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);

    /* 切到 USB 后射频关闭，HID 转发被拒 */
    pay[0] = (uint8_t)MODE_USB;
    run_cmd(CMD_SET_MODE, 5u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
    CHECK_EQ(bridge_mode_get(), MODE_USB);
    CHECK_EQ(bridge_mode_radio_id(), BRIDGE_RADIO_NONE);

    /* 配对模式不再允许经 SET_MODE 进入：统一走 PAIR_START，旧路径回 PARAM 错误 */
    pay[0] = (uint8_t)MODE_BLE_PAIR;
    host_tx_reset();
    run_cmd(CMD_SET_MODE, 6u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
    uint8_t f2 = 0u;
    CHECK(pump_find(CMD_SET_MODE, &f2, 0, 0, 4u));
    CHECK_EQ(f2 & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
    CHECK_EQ(bridge_mode_get(), MODE_USB);          /* 未进入配对窗口 */
}

static void test_get_mode_and_heartbeat(void)
{
    uint8_t flags = 0u, pay[8], len = 0u;

    CASE("GET_MODE 模式状态查询 + SET_HEARTBEAT 心跳周期开关");
    reset_all();
    connect_radio();

    /* 连接事件产生的 EVT_STATE 带 ACK_REQ：先应答清掉待决状态，
     * 否则其 ACK 重试帧会混入后续“关闭心跳后无发送”的帧计数。 */
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 5u);
    bridge_proto_poll();
    inject(EVT_STATE, 0u, BRIDGE_FLAG_ACK, 0, 0u);
    bridge_app_poll();
    host_tx_reset();

    /* GET_MODE：[mode][connected][radio] */
    host_tx_reset();
    run_cmd(CMD_GET_MODE, 0x50u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
    CHECK(pump_find(CMD_GET_MODE, &flags, pay, &len, 4u));
    CHECK_EQ(len, 3u);
    CHECK_EQ(pay[0], (uint8_t)bridge_mode_get());
    CHECK_EQ(pay[1], bridge_mode_is_connected() ? 1u : 0u);
    CHECK_EQ(pay[2], bridge_mode_radio_id());

    /* 越界开关值 → ERR */
    {
        uint8_t bad = 2u;
        host_tx_reset();
        run_cmd(CMD_SET_HEARTBEAT, 0x51u, BRIDGE_FLAG_ACK_REQ, &bad, 1u);
        CHECK(pump_find(CMD_SET_HEARTBEAT, &flags, 0, 0, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
    }

#if !BRIDGE_BOARD_IS_DONGLE
    /* 关闭心跳：不发 PING、失联判定停止、收到 PING 也不回 PONG */
    {
        uint8_t off = 0u;
        host_tx_reset();
        run_cmd(CMD_SET_HEARTBEAT, 0x52u, BRIDGE_FLAG_ACK_REQ, &off, 1u);
        CHECK(pump_find(CMD_SET_HEARTBEAT, &flags, 0, 0, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK(!bridge_uart_heartbeat_enabled());

        host_tx_reset();
        for (uint8_t i = 0u; i < BRIDGE_LINK_LOST_CNT + 1u; i++) {
            host_advance_ms(BRIDGE_PING_PERIOD_MS);
            bridge_app_poll();
        }
        CHECK_EQ(host_tx_frame_count(), 0u);            /* 未发送 PING */
        CHECK(bridge_uart_link_check());                /* 失联判定停止 → 恒正常 */
        CHECK(bridge_mode_get() != MODE_SLEEP);         /* 不因断链入睡 */

        host_tx_reset();
        inject(CMD_PING, 0x53u, 0u, 0, 0u);
        bridge_app_poll();
        CHECK_EQ(host_tx_frame_count(), 0u);            /* 不回 PONG */
    }

    /* 重新开启：PING 恢复发送 */
    {
        uint8_t on = 1u;
        host_tx_reset();
        run_cmd(CMD_SET_HEARTBEAT, 0x54u, BRIDGE_FLAG_ACK_REQ, &on, 1u);
        CHECK(bridge_uart_heartbeat_enabled());
        host_advance_ms(BRIDGE_PING_PERIOD_MS + 1u);
        bridge_app_poll();
        CHECK(host_tx_find_frame(CMD_PING, 0, 0, &len));
    }
#endif
}

/* 回归：UNPAIR 的射频选择必须按“当前模式”，不能按“是否已连接” */
static void test_unpair_radio_select(void)
{
    uint8_t flags = 0u, pay[4], len = 0u;

    CASE("UNPAIR 射频选择：2G4_PAIR（未连接）→ 落 2.4G，不误落 BLE");

    reset_all();
    host_radio_unpair_reset();
    host_tx_reset();

    CHECK_EQ(bridge_mode_request((uint8_t)MODE_2G4, false), BRIDGE_OK);
    CHECK_EQ(bridge_mode_request((uint8_t)MODE_2G4_PAIR, false), BRIDGE_OK);
    {
        uint8_t p = 0xFFu;
        run_cmd(CMD_UNPAIR, 0x70u, 0u, &p, 1u);
        CHECK(pump_find(CMD_UNPAIR, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    CHECK_EQ(host_radio_unpair_count(2u), 1u);   /* BRIDGE_RADIO_2G4=2：0xFF 全清透传一次 */
    CHECK_EQ(host_radio_unpair_count(1u), 0u);   /* BRIDGE_RADIO_BLE = 1（不得误落） */
}

/* 配对窗口超时：core 记录窗口起点，超时后回落非配对模式、停配对动作，
 * 并复用 EVT_PAIR_RESULT(1) 上报一次瞬时「配对失败/超时」。 */
static void test_pair_window_timeout(void)
{
    bridge_mode_t pair_mode;
    bridge_mode_t fallback_mode;
    uint8_t       expect_radio;
    uint8_t       flags = 0u, pay[4], len = 0u;

    CASE("配对窗口超时：回落非配对模式 + EVT_PAIR_RESULT(1)");

    reset_all();
    host_radio_pair_stop_reset();
    host_tx_reset();

    /* 进入配对窗口 */
    run_cmd(CMD_PAIR_START, 0x7Au, BRIDGE_FLAG_ACK_REQ, 0, 0u);
#if BRIDGE_BOARD_IS_DONGLE
    pair_mode     = MODE_2G4_PAIR;
    fallback_mode = MODE_2G4;
    expect_radio  = BRIDGE_RADIO_2G4;
#else
    pair_mode     = MODE_BLE_PAIR;
    fallback_mode = MODE_BLE;
    expect_radio  = BRIDGE_RADIO_BLE;
#endif
    CHECK_EQ(bridge_mode_get(), pair_mode);

    /* 超时前（窗口内）仍停在配对模式 */
    host_advance_ms((uint32_t)(BRIDGE_PAIR_TIMEOUT_S - 1u) * 1000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), pair_mode);
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 0u);

    /* 超过 BRIDGE_PAIR_TIMEOUT_S：停配对窗口、回落、并上报 EVT_PAIR_RESULT(1) */
    host_tx_reset();
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1001u);
    bridge_mode_poll();
    CHECK(pump_find(EVT_PAIR_RESULT, &flags, pay, &len, 4u));
    CHECK_EQ(pay[0], 1u);
    CHECK_EQ(bridge_mode_get(), fallback_mode);
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 1u);

    /* 回落后不再是配对窗口：不得重复停窗口 */
    host_advance_ms((uint32_t)BRIDGE_PAIR_TIMEOUT_S * 1000u);
    bridge_mode_poll();
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 1u);

#if !BRIDGE_BOARD_IS_DONGLE
    /* 2.4G 已配对后掉线：不得重入配对窗口/重开广播，保持 MODE_2G4 并射频静默；
     * AUTO 策略下无连接且空闲即按期自动深睡（不再经配对窗口超时回落路径）。 */
    reset_all();
    /* 默认已为 MANUAL（关闭自动休眠）：本用例验证 AUTO 策略下掉线自动深睡 */
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    CHECK_EQ(bridge_mode_request(MODE_2G4, false), BRIDGE_OK);
    connect_radio();
    CHECK_EQ(bridge_mode_get(), MODE_2G4);
    host_radio_pair_stop_reset();                       /* 断链前清零，单测断链静默动作 */
    bridge_mode_on_radio_event(BRIDGE_RADIO_EVT_DISCONNECTED);
    CHECK_EQ(bridge_mode_get(), MODE_2G4);              /* 不重入配对窗口 */
    CHECK_EQ(host_radio_pair_stop_count(BRIDGE_RADIO_2G4), 1u);   /* 停配对/广播，射频静默 */

    host_advance_ms((uint32_t)BRIDGE_IDLE_TO_SLEEP_S * 1000u + 1000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), MODE_SLEEP);
#endif
}

/* PAIR_START 持续时间参数 [duration_s LE16]（单位秒）：
 *   省略（无载荷）= 回默认 BRIDGE_PAIR_TIMEOUT_S；0 = 窗口不自动关闭；
 *   非 0/2 字节 → ERR PARAM（不进入配对窗口）。 */
static void test_pair_duration(void)
{
    bridge_mode_t pair_mode;
    bridge_mode_t fallback_mode;
    uint8_t       expect_radio;
    uint8_t       flags = 0u, pay[4], len = 0u;
    uint8_t       dur[2];

    CASE("PAIR_START 持续时间：短窗口超时 / 0=不自动关闭 / 非法长度");

    reset_all();
    host_radio_pair_stop_reset();
    host_tx_reset();

#if BRIDGE_BOARD_IS_DONGLE
    pair_mode     = MODE_2G4_PAIR;
    fallback_mode = MODE_2G4;
    expect_radio  = BRIDGE_RADIO_2G4;
#else
    pair_mode     = MODE_BLE_PAIR;
    fallback_mode = MODE_BLE;
    expect_radio  = BRIDGE_RADIO_BLE;
#endif

    /* duration_s = 5：窗口 5s */
    dur[0] = 5u; dur[1] = 0u;
    run_cmd(CMD_PAIR_START, 0x70u, BRIDGE_FLAG_ACK_REQ, dur, 2u);
    CHECK_EQ(bridge_mode_get(), pair_mode);

    /* 4s（窗口内）仍停在配对模式 */
    host_advance_ms(4000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), pair_mode);
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 0u);

    /* 超过 5s：停配对窗口、回落非配对模式、上报 EVT_PAIR_RESULT(1) */
    host_tx_reset();
    host_advance_ms(2000u);
    bridge_mode_poll();
    CHECK(pump_find(EVT_PAIR_RESULT, &flags, pay, &len, 4u));
    CHECK_EQ(pay[0], 1u);
    CHECK_EQ(bridge_mode_get(), fallback_mode);
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 1u);

    /* duration_s = 0：不自动关闭窗口，长时间推进仍停在配对模式 */
    reset_all();
    host_radio_pair_stop_reset();
    host_tx_reset();
    dur[0] = 0u; dur[1] = 0u;
    run_cmd(CMD_PAIR_START, 0x71u, BRIDGE_FLAG_ACK_REQ, dur, 2u);
    CHECK_EQ(bridge_mode_get(), pair_mode);

    host_advance_ms((uint32_t)(BRIDGE_PAIR_TIMEOUT_S + 30u) * 1000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), pair_mode);          /* 0：不自动关闭 */
    CHECK_EQ(host_radio_pair_stop_count(expect_radio), 0u);

    /* 非法长度（1 字节）→ ERR PARAM，且不进入配对窗口 */
    reset_all();
    host_tx_reset();
    {
        uint8_t bad[1] = { 0x05u };

        run_cmd(CMD_PAIR_START, 0x72u, BRIDGE_FLAG_ACK_REQ, bad, 1u);
    }
    CHECK(pump_find(CMD_PAIR_START, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
    CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    CHECK(bridge_mode_get() != MODE_BLE_PAIR);
    CHECK(bridge_mode_get() != MODE_2G4_PAIR);
}

static void test_pair_and_unpair(void)
{
    CASE("PAIR_START / UNPAIR");
    reset_all();
    connect_radio();

    run_cmd(CMD_PAIR_START, 1u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
    /* 当前无线模式决定配对目标：键盘(BLE)→BLE_PAIR，Dongle(2G4)→2G4_PAIR */
#if BRIDGE_BOARD_IS_DONGLE
    CHECK_EQ(bridge_mode_get(), MODE_2G4_PAIR);
#else
    CHECK_EQ(bridge_mode_get(), MODE_BLE_PAIR);
#endif

    /* 2.4G 已配对后掉线：保持常规模式、射频静默（不得重入配对窗口） */
    CHECK_EQ(bridge_mode_request(MODE_2G4, false), BRIDGE_OK);
    connect_radio();
    CHECK_EQ(bridge_mode_get(), MODE_2G4);
    bridge_mode_on_radio_event(BRIDGE_RADIO_EVT_DISCONNECTED);
    CHECK_EQ(bridge_mode_get(), MODE_2G4);

    bridge_mode_on_radio_event(BRIDGE_RADIO_EVT_PAIRED);
    CHECK_EQ(bridge_mode_get(), MODE_2G4);
    CHECK(bridge_mode_is_connected());

    /* 越界 profile 参数 → 错误帧 */
    uint8_t bad = 9u;
    host_tx_reset();
    run_cmd(CMD_SET_PROFILE, 6u, BRIDGE_FLAG_ACK_REQ, &bad, 1u);
    uint8_t flags = 0u;
    CHECK(pump_find(CMD_SET_PROFILE, &flags, 0, 0, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
}

/* 回归：配对窗口内配对/连接成功后深睡，唤醒必须回到「已连接态」而非 *_PAIR。
 * 原缺陷：PAIR_START 落盘 last_mode=*_PAIR，配对上仅改内存模式不落盘，
 * 深睡唤醒按 last_mode 恢复到配对窗口（只广播 / 只重发，不走 connect_profile），
 * 表现为"唤醒后连不上、需手动切模式再切回才恢复"。 */
static void test_sleep_wake_restore_mode(void)
{
    bridge_mode_t pair_mode, linked_mode;
    uint8_t       flags = 0u, pay[4], len = 0u;

    CASE("深睡唤醒恢复到已连接态（非配对窗口）");

    reset_all();

    /* 进入配对窗口：落盘 *_PAIR */
    run_cmd(CMD_PAIR_START, 0x80u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
#if BRIDGE_BOARD_IS_DONGLE
    pair_mode   = MODE_2G4_PAIR;
    linked_mode = MODE_2G4;
#else
    pair_mode   = MODE_BLE_PAIR;
    linked_mode = MODE_BLE;
#endif
    CHECK_EQ(bridge_mode_get(), pair_mode);
    CHECK_EQ(bridge_cfg_get()->last_mode, (uint8_t)pair_mode);

    /* 配对/连接成功：必须同步脱离配对窗口并落盘 */
    connect_radio();
    CHECK_EQ(bridge_mode_get(), linked_mode);
    CHECK_EQ(bridge_cfg_get()->last_mode, (uint8_t)linked_mode);
    CHECK(bridge_mode_is_connected());

    /* 深睡：不得改变 last_mode */
    {
        uint8_t lv = BRIDGE_SLEEP_DEEP;

        run_cmd(CMD_SLEEP, 0x81u, BRIDGE_FLAG_ACK_REQ, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(bridge_mode_get(), MODE_SLEEP);
        CHECK_EQ(bridge_cfg_get()->last_mode, (uint8_t)linked_mode);
    }

    /* 唤醒：必须回到已连接态（缺陷回归点，不得停在配对窗口） */
    {
        uint8_t lv = BRIDGE_SLEEP_WAKE;

        run_cmd(CMD_SLEEP, 0x82u, BRIDGE_FLAG_ACK_REQ, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(bridge_mode_get(), linked_mode);
        CHECK(bridge_mode_get() != pair_mode);
    }

    /* 记忆模式 → 恢复目标映射（各恢复路径共用） */
    CHECK_EQ(bridge_mode_restore_target((uint8_t)MODE_BLE_PAIR), (uint8_t)MODE_BLE);
    CHECK_EQ(bridge_mode_restore_target((uint8_t)MODE_2G4_PAIR), (uint8_t)MODE_2G4);
    CHECK_EQ(bridge_mode_restore_target((uint8_t)MODE_BLE), (uint8_t)MODE_BLE);
    CHECK_EQ(bridge_mode_restore_target((uint8_t)MODE_USB), (uint8_t)MODE_USB);
}

static void test_evt_state_ack_retry(void)
{
    CASE("EVT_STATE 未应答重试 3 次后报错");
    reset_all();

    bridge_evt_state();                     /* 发出并等待应答 */
    CHECK(host_tx_find_frame(EVT_STATE, 0, 0, 0));

    host_tx_reset();
    for (uint8_t i = 0u; i < BRIDGE_ACK_MAX_RETRY + 2u; i++) {
        host_advance_ms(BRIDGE_ACK_TIMEOUT_MS + BRIDGE_FRAME_MIN_GAP_MS + 1u);
        bridge_proto_poll();
    }

    /* 重试 3 次 + 最终 EVT_ERROR(ACK_TIMEOUT) */
    CHECK_EQ(host_tx_frame_count(), (uint16_t)(BRIDGE_ACK_MAX_RETRY + 1u));
    uint8_t pay[4], len = 0u, flags = 0u;
    CHECK(host_tx_find_frame(EVT_ERROR, &flags, pay, &len));
    CHECK_EQ(pay[0], BRIDGE_ECODE_ACK_TIMEOUT);

    /* 收到 ACK 后不应再重试 */
    reset_all();
    bridge_evt_state();
    uint8_t ack = 0u;
    host_advance_ms(BRIDGE_FRAME_MIN_GAP_MS + 1u);
    bridge_app_poll();
    inject(EVT_STATE, 0u, BRIDGE_FLAG_ACK, &ack, 0u);
    bridge_app_poll();
    host_tx_reset();
    host_advance_ms((BRIDGE_ACK_TIMEOUT_MS + 1u) * 5u);
    bridge_proto_poll();
    CHECK_EQ(host_tx_frame_count(), 0u);
}

static void test_hid_rejected_when_disconnected(void)
{
    CASE("未连接时 HID 被拒绝");
    reset_all();                 /* 未触发 CONNECTED */
    const uint8_t press[8] = { 0u, 0u, 0x04u, 0u, 0u, 0u, 0u, 0u };
    host_radio_reset();
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_ERR_STATE);
    CHECK_EQ(g_host_radio_count, 0u);

    /* 射频发送失败 → IO 错误且不污染去重缓存 */
    connect_radio();
    host_radio_fail_next(true);
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_ERR_IO);
    CHECK_EQ(bridge_hid_send(CMD_HID_KEYBOARD, press, 8u), BRIDGE_OK);
}

/* ---------------- 借鉴市售模组「一、基本数据 / 二、控制命令」的能力 ---------------- */

static void test_tunnel_commands(void)
{
    CASE("透传命令：USB 0x18 / BLE 0x19 分流与上限");

    reset_all();
    host_tunnel_reset();

    /* USB 透传（上限 64B）：纯转发，不产生任何应答/事件帧 */
    {
        const uint8_t d[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };

        run_cmd(CMD_TUNNEL_USB, 0x21u, BRIDGE_FLAG_ACK_REQ, d, 4u);
        CHECK_EQ(g_host_tunnel_kind, BRIDGE_TUNNEL_USB);
        CHECK_EQ(g_host_tunnel_len, 4u);
        CHECK(memcmp(g_host_tunnel, "\xDE\xAD\xBE\xEF", 4u) == 0);
        CHECK_EQ(host_tx_frame_count(), 0u);
    }

    /* BLE 透传（上限 32B）：kind 正确分流 */
    host_tunnel_reset();
    {
        const uint8_t d[2] = { 0x11u, 0x22u };

        run_cmd(CMD_TUNNEL_BLE, 0x22u, 0u, d, 2u);
        CHECK_EQ(g_host_tunnel_kind, BRIDGE_TUNNEL_BLE);
        CHECK_EQ(g_host_tunnel_len, 2u);
        CHECK_EQ(g_host_tunnel[0], 0x11u);
    }

    /* 超过 BLE 上限 → 原 CMD + ERR 位错误帧，且不得落入透传缓冲 */
    host_tunnel_reset();
    host_tx_reset();
    {
        uint8_t big[BRIDGE_TUNNEL_BLE_LEN + 1u];
        uint8_t flags = 0u, pay[4], len = 0u;

        memset(big, 0x5Au, sizeof(big));
        run_cmd(CMD_TUNNEL_BLE, 0x23u, 0u, big, (uint8_t)sizeof(big));
        CHECK(pump_find(CMD_TUNNEL_BLE, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(len, 1u);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
        CHECK_EQ(g_host_tunnel_len, 0u);
    }
}

static void test_nv_user_commands(void)
{
    CASE("用户 NV：0x1A 写 / 0x1B 读（32B 单元）");

    reset_all();

    /* 真实 flash 未写槽可读回 0xFF；主机模型对"未写槽"返回错误，
     * 故先落一次空槽，模拟已擦除内容，使读-改-写路径可覆盖。 */
    {
        uint8_t erased[BRIDGE_NV_SLOT_SIZE];

        memset(erased, 0xFFu, sizeof(erased));
        CHECK_EQ(port_nv_write((uint16_t)BRIDGE_NV_USER_BASE, erased,
                               (uint16_t)sizeof(erased)), BRIDGE_OK);
    }

    /* 写单元 0：载荷 = [单元号][32B] */
    host_tx_reset();
    {
        uint8_t w[1u + BRIDGE_NV_UNIT];
        uint8_t flags = 0u, pay[4], len = 0u;

        w[0] = 0u;
        for (uint8_t i = 0u; i < BRIDGE_NV_UNIT; i++) {
            w[1u + i] = (uint8_t)(0xA0u + i);
        }
        run_cmd(CMD_NV_WRITE, 0x31u, 0u, w, (uint8_t)sizeof(w));
        CHECK(pump_find(CMD_NV_WRITE, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(len, 0u);
    }

    /* 读回：应答载荷 = [单元号][32B]，内容一致 */
    host_tx_reset();
    {
        uint8_t a = 0u;
        uint8_t flags = 0u, pay[1u + BRIDGE_NV_UNIT], len = 0u;

        run_cmd(CMD_NV_READ, 0x32u, 0u, &a, 1u);
        CHECK(pump_find(CMD_NV_READ, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(len, 1u + BRIDGE_NV_UNIT);
        CHECK_EQ(pay[0], 0u);
        CHECK_EQ(pay[1], 0xA0u);
        CHECK_EQ(pay[BRIDGE_NV_UNIT], (uint8_t)(0xA0u + BRIDGE_NV_UNIT - 1u));
    }

    /* 单元号越界 → 错误帧 PARAM */
    host_tx_reset();
    {
        uint8_t a = (uint8_t)BRIDGE_NV_USER_UNITS;
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_NV_READ, 0x33u, 0u, &a, 1u);
        CHECK(pump_find(CMD_NV_READ, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    }
}

/* ---------------- 键盘侧 UART OTA：FW_BEGIN/DATA/END 状态机 ---------------- */
static void test_fw_ota(void)
{
    uint8_t  img[64];
    uint16_t crc;
    uint8_t  flags = 0u, pay[64], len = 0u;

    CASE("UART OTA：FW_BEGIN/DATA/END 状态机 + CRC 门禁");

    reset_all();
    host_fw_reset();

    for (uint8_t i = 0u; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 3u);
    }
    crc = bridge_crc16_ccitt(img, (uint16_t)sizeof(img));

    /* 未 BEGIN 直接 DATA → ERR(STATE) */
    host_tx_reset();
    {
        uint8_t d[4] = {0u, 0u, 0u, 0u};
        run_cmd(CMD_FW_DATA, 0x52u, 0u, d, 4u);
        CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_STATE);
    }

    /* 非法 BEGIN（载荷长度错）→ ERR(PARAM) */
    host_tx_reset();
    {
        uint8_t b[3] = {1u, 2u, 3u};
        run_cmd(CMD_FW_BEGIN, 0x50u, 0u, b, 3u);
        CHECK(pump_find(CMD_FW_BEGIN, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    }

    /* 正常 BEGIN → 应答 chunk_max */
    host_tx_reset();
    {
        uint8_t b[6];
        b[0] = (uint8_t)(sizeof(img) & 0xFFu);
        b[1] = 0u;
        b[2] = 0u;
        b[3] = 0u;
        b[4] = (uint8_t)(crc & 0xFFu);
        b[5] = (uint8_t)(crc >> 8);
        run_cmd(CMD_FW_BEGIN, 0x51u, 0u, b, 6u);
        CHECK(pump_find(CMD_FW_BEGIN, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(len, 1u);
        CHECK_EQ(pay[0], BRIDGE_FW_CHUNK_MAX);
    }
    CHECK_EQ(bridge_mode_get(), MODE_OTA);      /* BEGIN 即关射频进 OTA 态 */

    /* 乱序 offset → ERR(PARAM) */
    host_tx_reset();
    {
        uint8_t d[4u + 16u];
        d[0] = 16u; d[1] = 0u; d[2] = 0u; d[3] = 0u;
        memcpy(&d[4], img, 16u);
        run_cmd(CMD_FW_DATA, 0x53u, 0u, d, (uint8_t)sizeof(d));
        CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    }

    /* 顺序 4×16B → ACK，镜像内容一致 */
    for (uint8_t k = 0u; k < 4u; k++) {
        uint8_t  d[4u + 16u];
        uint16_t off = (uint16_t)(k * 16u);

        d[0] = (uint8_t)(off & 0xFFu);
        d[1] = (uint8_t)(off >> 8);
        d[2] = 0u;
        d[3] = 0u;
        memcpy(&d[4], &img[off], 16u);
        host_tx_reset();
        run_cmd(CMD_FW_DATA, (uint8_t)(0x54u + k), 0u, d, (uint8_t)sizeof(d));
        CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    /* 重复帧（上位机因 ACK 丢失重传）→ 幂等 ACK，写入量不变 */
    {
        uint8_t d[4u + 16u];

        d[0] = 16u; d[1] = 0u; d[2] = 0u; d[3] = 0u;   /* 重传第 2 块 */
        memcpy(&d[4], &img[16], 16u);
        host_tx_reset();
        run_cmd(CMD_FW_DATA, 0x5Fu, 0u, d, (uint8_t)sizeof(d));
        CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }

    CHECK_EQ(host_fw_written(), 64u);
    {
        uint32_t n = 0u;
        const uint8_t *got = host_fw_data(&n);
        CHECK_EQ(n, 64u);
        CHECK(memcmp(got, img, 64u) == 0);
    }

    /* END 校验通过 → ACK + 完成回调（CRC 一致） */
    host_tx_reset();
    run_cmd(CMD_FW_END, 0x60u, 0u, 0, 0u);
    CHECK(pump_find(CMD_FW_END, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    CHECK_EQ(host_fw_finish_count(), 1u);
    CHECK_EQ(host_fw_finish_crc(), crc);

    /* CRC 不符：重走一遍（带错 CRC）→ ERR(CRC) 且不触发完成 */
    {
        uint8_t  b[6];
        uint16_t bad = (uint16_t)(crc ^ 0xFFFFu);

        b[0] = (uint8_t)(sizeof(img) & 0xFFu); b[1] = 0u; b[2] = 0u; b[3] = 0u;
        b[4] = (uint8_t)(bad & 0xFFu); b[5] = (uint8_t)(bad >> 8);
        host_tx_reset();
        run_cmd(CMD_FW_BEGIN, 0x61u, 0u, b, 6u);
        CHECK(pump_find(CMD_FW_BEGIN, &flags, pay, &len, 4u));

        for (uint8_t k = 0u; k < 4u; k++) {
            uint8_t  d[4u + 16u];
            uint16_t off = (uint16_t)(k * 16u);

            d[0] = (uint8_t)(off & 0xFFu); d[1] = (uint8_t)(off >> 8); d[2] = 0u; d[3] = 0u;
            memcpy(&d[4], &img[off], 16u);
            run_cmd(CMD_FW_DATA, (uint8_t)(0x62u + k), 0u, d, (uint8_t)sizeof(d));
            CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        }
        host_tx_reset();
        run_cmd(CMD_FW_END, 0x66u, 0u, 0, 0u);
        CHECK(pump_find(CMD_FW_END, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_CRC);
        CHECK_EQ(host_fw_finish_count(), 1u);   /* 未触发第二次完成 */
    }

    /* ABORT → 记一次放弃；之后 DATA → ERR(STATE) */
    host_tx_reset();
    run_cmd(CMD_FW_ABORT, 0x67u, 0u, 0, 0u);
    CHECK(pump_find(CMD_FW_ABORT, &flags, pay, &len, 4u));
    CHECK_EQ(host_fw_abort_count(), 1u);
    host_tx_reset();
    {
        uint8_t d[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        run_cmd(CMD_FW_DATA, 0x68u, 0u, d, (uint8_t)sizeof(d));
        CHECK(pump_find(CMD_FW_DATA, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_STATE);
    }
}

static void test_misc_commands(void)
{
    CASE("下发电量 0x1C / 睡眠分级 0x1D / 按需 ADC 0x1E");

    /* 下发电量：ACK + 立即 EVT_STATE 上报（state[3] = 电量） */
    reset_all();
    host_tx_reset();
    {
        uint8_t pct = 77u;
        uint8_t flags = 0u, pay[8], len = 0u;

        run_cmd(CMD_SET_BATTERY, 0x41u, 0u, &pct, 1u);
        CHECK(pump_find(CMD_SET_BATTERY, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(bridge_power_has_external(), true);
        CHECK_EQ(bridge_power_battery_percent(), 77u);

        host_tx_reset();
        bridge_evt_state();
        CHECK(pump_find(EVT_STATE, &flags, pay, &len, 4u));
        CHECK_EQ(len, 4u);
        CHECK_EQ(pay[3], 77u);
    }

    /* 睡眠分级：IDLE → 浅睡（模式不变、射频保持）；DEEP → MODE_SLEEP；WAKE → 退出深睡 */
    reset_all();
    host_tx_reset();
    {
        uint8_t lv = BRIDGE_SLEEP_IDLE;
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_SLEEP, 0x42u, 0u, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK(bridge_mode_get() != MODE_SLEEP);       /* 浅睡：不改模式、不关射频 */

        host_tx_reset();
        lv = BRIDGE_SLEEP_DEEP;
        run_cmd(CMD_SLEEP, 0x45u, 0u, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(bridge_mode_get(), MODE_SLEEP);

        host_tx_reset();
        lv = BRIDGE_SLEEP_WAKE;
        run_cmd(CMD_SLEEP, 0x43u, 0u, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK(bridge_mode_get() != MODE_SLEEP);
    }

    /* 非法睡眠等级 → 错误帧 PARAM */
    host_tx_reset();
    {
        uint8_t lv = (uint8_t)(BRIDGE_SLEEP_DEEP + 1u);
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_SLEEP, 0x44u, 0u, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    }

    /* 按需 ADC：应答 2B 小端毫伏（3999 = 0x0F9F → 9F 0F） */
    reset_all();
    host_set_bat_mv(3999u);
    host_tx_reset();
    {
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_GET_ADC, 0x45u, 0u, 0, 0u);
        CHECK(pump_find(CMD_GET_ADC, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(len, 2u);
        CHECK_EQ(pay[0], 0x9Fu);
        CHECK_EQ(pay[1], 0x0Fu);
    }
}

/* BLE 多主机槽状态查询（GET_BLE_SLOTS 0x0F）：主机桩应答 5B [s1..s4][last_active] */
static void test_get_ble_slots(void)
{
    uint8_t flags = 0u, pay[8], len = 0u;

    CASE("BLE 多主机槽状态 0x0F");

    reset_all();
    host_tx_reset();
    run_cmd(CMD_GET_BLE_SLOTS, 0x46u, 0u, 0, 0u);
    CHECK(pump_find(CMD_GET_BLE_SLOTS, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    CHECK_EQ(len, 5u);
    CHECK_EQ(pay[0], 0u);
    CHECK_EQ(pay[1], 0u);
    CHECK_EQ(pay[2], 0u);
    CHECK_EQ(pay[3], 0u);
    CHECK_EQ(pay[4], 0u);
}

/* 睡眠策略（SET_CONFIG key 0x06）：AUTO 自动休眠 / MANUAL 指令控制休眠 */
static void test_sleep_policy(void)
{
    CASE("睡眠策略：默认 MANUAL 指令控制休眠 / AUTO 自动休眠");
    reset_all();

    /* 默认 MANUAL（关闭自动休眠）：未指令许可 → 不得休眠 */
    CHECK_EQ(bridge_cfg_get()->sleep_policy, BRIDGE_SLEEP_POLICY_MANUAL);
    CHECK(!bridge_mode_sleep_permitted());

    /* SET_CONFIG(0x06) 切到 AUTO（自动休眠） */
    {
        uint8_t pay[2] = { BRIDGE_CFG_KEY_SLEEP_POLICY, BRIDGE_SLEEP_POLICY_AUTO };
        uint8_t flags = 0u;

        run_cmd(CMD_SET_CONFIG, 0x60u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
        CHECK(pump_find(CMD_SET_CONFIG, &flags, 0, 0, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    CHECK_EQ(bridge_cfg_get()->sleep_policy, BRIDGE_SLEEP_POLICY_AUTO);
    CHECK(bridge_mode_sleep_permitted());           /* AUTO：恒许可休眠 */

    /* 掉电保持 */
    bridge_cfg_init();
    CHECK_EQ(bridge_cfg_get()->sleep_policy, BRIDGE_SLEEP_POLICY_AUTO);

    /* GET_STATUS 回读策略字节（payload[4]） */
    {
        uint8_t flags = 0u, pay[8], len = 0u;

        run_cmd(CMD_GET_STATUS, 0x61u, BRIDGE_FLAG_ACK_REQ, 0, 0u);
        CHECK(pump_find(CMD_GET_STATUS, &flags, pay, &len, 4u));
        CHECK_EQ(len, 5u);
        CHECK_EQ(pay[4], BRIDGE_SLEEP_POLICY_AUTO);
    }

    /* 切回默认 MANUAL（指令控制），供后续 IDLE/WAKE/DEEP 子用例 */
    {
        uint8_t pay[2] = { BRIDGE_CFG_KEY_SLEEP_POLICY, BRIDGE_SLEEP_POLICY_MANUAL };
        uint8_t flags = 0u;

        run_cmd(CMD_SET_CONFIG, 0x66u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
        CHECK(pump_find(CMD_SET_CONFIG, &flags, 0, 0, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    CHECK_EQ(bridge_cfg_get()->sleep_policy, BRIDGE_SLEEP_POLICY_MANUAL);
    CHECK(!bridge_mode_sleep_permitted());

    /* MANUAL：CMD_SLEEP(IDLE) 置位许可；CMD_SLEEP(WAKE) 清除 */
    {
        uint8_t lv = BRIDGE_SLEEP_IDLE;
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_SLEEP, 0x62u, BRIDGE_FLAG_ACK_REQ, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK(bridge_mode_sleep_permitted());

        host_tx_reset();
        lv = BRIDGE_SLEEP_WAKE;
        run_cmd(CMD_SLEEP, 0x63u, BRIDGE_FLAG_ACK_REQ, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK(!bridge_mode_sleep_permitted());
    }

    /* MANUAL：CMD_SLEEP(DEEP) 许可并进 MODE_SLEEP */
    {
        uint8_t lv = BRIDGE_SLEEP_DEEP;
        uint8_t flags = 0u, pay[4], len = 0u;

        run_cmd(CMD_SLEEP, 0x64u, BRIDGE_FLAG_ACK_REQ, &lv, 1u);
        CHECK(pump_find(CMD_SLEEP, &flags, pay, &len, 4u));
        CHECK_EQ(bridge_mode_get(), MODE_SLEEP);
    }

    /* 非法策略值 → PARAM 错误帧 */
    {
        uint8_t pay[2] = { BRIDGE_CFG_KEY_SLEEP_POLICY, 0x7Fu };
        uint8_t flags = 0u, out[4], len = 0u;

        host_tx_reset();
        run_cmd(CMD_SET_CONFIG, 0x65u, BRIDGE_FLAG_ACK_REQ, pay, 2u);
        CHECK(pump_find(CMD_SET_CONFIG, &flags, out, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(out[0], BRIDGE_ECODE_PARAM);
    }

#if !BRIDGE_BOARD_IS_DONGLE
    /* AUTO：30s 无活动自动深睡；MANUAL（默认）：不自动进睡（仅指令控制） */
    reset_all();
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    host_advance_ms((uint32_t)(BRIDGE_IDLE_TO_SLEEP_S + 1u) * 1000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), MODE_SLEEP);

    reset_all();
    host_advance_ms((uint32_t)(BRIDGE_IDLE_TO_SLEEP_S + 1u) * 1000u);
    bridge_mode_poll();
    CHECK(bridge_mode_get() != MODE_SLEEP);          /* 默认 MANUAL：不自动进睡 */
#endif

    /* 复位回默认 MANUAL，避免影响后续用例 */
    (void)bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_MANUAL);
}

/* SLEEP_CFG (0x25)：payload [idle_s LE16][deep_s LE16]，单位秒，落盘；
 * 仅 AUTO 策略下生效，0 = 禁用对应级自动休眠。 */
static void test_sleep_cfg(void)
{
    uint8_t flags = 0u, pay[6], len = 0u;

    CASE("SLEEP_CFG 0x25：配置/落盘/字节序/非法长度 + AUTO 深睡门限");

    reset_all();
    /* 出厂默认：idle_s = 1（配合 port 1500ms 硬下限），deep_s = BRIDGE_IDLE_TO_SLEEP_S */
    CHECK_EQ(bridge_cfg_get()->sleep_idle_s, 1u);
    CHECK_EQ(bridge_cfg_get()->sleep_deep_s, (uint16_t)BRIDGE_IDLE_TO_SLEEP_S);

    /* 配置 idle=5 / deep=7 → ACK */
    {
        uint8_t p[4] = { 5u, 0u, 7u, 0u };

        run_cmd(CMD_SLEEP_CFG, 0x80u, BRIDGE_FLAG_ACK_REQ, p, 4u);
        CHECK(pump_find(CMD_SLEEP_CFG, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    CHECK_EQ(bridge_cfg_get()->sleep_idle_s, 5u);
    CHECK_EQ(bridge_cfg_get()->sleep_deep_s, 7u);

    /* 落盘：重新初始化配置后仍保持 */
    bridge_cfg_init();
    CHECK_EQ(bridge_cfg_get()->sleep_idle_s, 5u);
    CHECK_EQ(bridge_cfg_get()->sleep_deep_s, 7u);

    /* 小端解析：idle = 0x0102(258) / deep = 0x0304(772) */
    {
        uint8_t p[4] = { 0x02u, 0x01u, 0x04u, 0x03u };

        run_cmd(CMD_SLEEP_CFG, 0x81u, BRIDGE_FLAG_ACK_REQ, p, 4u);
        CHECK(pump_find(CMD_SLEEP_CFG, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
    }
    CHECK_EQ(bridge_cfg_get()->sleep_idle_s, 258u);
    CHECK_EQ(bridge_cfg_get()->sleep_deep_s, 772u);

    /* 非法长度（3 字节）→ ERR PARAM，值不变 */
    host_tx_reset();
    {
        uint8_t p[3] = { 1u, 0u, 0u };

        run_cmd(CMD_SLEEP_CFG, 0x82u, BRIDGE_FLAG_ACK_REQ, p, 3u);
    }
    CHECK(pump_find(CMD_SLEEP_CFG, &flags, pay, &len, 4u));
    CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
    CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    CHECK_EQ(bridge_cfg_get()->sleep_idle_s, 258u);
    CHECK_EQ(bridge_cfg_get()->sleep_deep_s, 772u);

#if !BRIDGE_BOARD_IS_DONGLE
    /* AUTO + deep_s=5：空闲 4s 不睡、6s 进 MODE_SLEEP */
    reset_all();
    CHECK_EQ(bridge_cfg_set_sleep_times(1u, 5u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    host_advance_ms(4000u);
    bridge_mode_poll();
    CHECK(bridge_mode_get() != MODE_SLEEP);
    host_advance_ms(3000u);
    bridge_mode_poll();
    CHECK_EQ(bridge_mode_get(), MODE_SLEEP);

    /* AUTO + deep_s=0：禁用自动深睡，长时间空闲也不进睡 */
    reset_all();
    CHECK_EQ(bridge_cfg_set_sleep_times(1u, 0u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_AUTO), BRIDGE_OK);
    host_advance_ms((uint32_t)(BRIDGE_IDLE_TO_SLEEP_S + 30u) * 1000u);
    bridge_mode_poll();
    CHECK(bridge_mode_get() != MODE_SLEEP);
#endif

    /* 复位默认，避免影响后续用例 */
    reset_all();
    (void)bridge_cfg_set_sleep_times(1u, (uint16_t)BRIDGE_IDLE_TO_SLEEP_S);
    (void)bridge_cfg_set_sleep_policy(BRIDGE_SLEEP_POLICY_MANUAL);
}

static void test_set_name_and_usb_id(void)
{
    CASE("CMD_SET_NAME 0x1F / CMD_SET_USB_ID 0x20 本地生效与中继");

    reset_all();

    /* BLE 名（键盘侧本地生效；Dongle 无 BLE，不中继） */
    {
        uint8_t p[4] = { BRIDGE_NAME_TARGET_BLE, 'K', 'B', 'D' };
        uint8_t flags = 0u, pay[4], len = 0u;

        host_name_reset();
        host_tx_reset();
        run_cmd(CMD_SET_NAME, 0x51u, BRIDGE_FLAG_ACK_REQ, p, 4u);
        CHECK(pump_find(CMD_SET_NAME, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(bridge_cfg_ble_name_len(), 3u);
        CHECK(strcmp(bridge_cfg_ble_name(), "KBD") == 0);
        CHECK_EQ(host_cfg_relay_len(), -1);
#if BRIDGE_BOARD_IS_DONGLE
        CHECK_EQ(host_name_target(), -1);
#else
        CHECK_EQ(host_name_target(), (int)BRIDGE_NAME_TARGET_BLE);
#endif
    }

    /* USB 产品名（Dongle 本地生效；键盘侧经 2.4G 中继给 Dongle） */
    {
        uint8_t p[3] = { BRIDGE_NAME_TARGET_USB, 'U', 'S' };
        uint8_t flags = 0u, pay[4], len = 0u;

        host_name_reset();
        host_tx_reset();
        run_cmd(CMD_SET_NAME, 0x52u, BRIDGE_FLAG_ACK_REQ, p, 3u);
        CHECK(pump_find(CMD_SET_NAME, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(bridge_cfg_usb_name_len(), 2u);
        CHECK(strcmp(bridge_cfg_usb_name(), "US") == 0);
#if BRIDGE_BOARD_IS_DONGLE
        CHECK_EQ(host_name_target(), (int)BRIDGE_NAME_TARGET_USB);
        CHECK_EQ(host_cfg_relay_len(), -1);
#else
        CHECK_EQ(host_name_target(), -1);
        {
            uint8_t rl = 0u;
            const uint8_t *rf = host_cfg_relay(&rl);
            CHECK_EQ(host_cfg_relay_len(), 4);
            CHECK(rf != 0 && rf[0] == CMD_SET_NAME && rf[1] == BRIDGE_NAME_TARGET_USB);
        }
#endif
    }

    /* VID/PID（Dongle 本地生效；键盘侧中继） */
    {
        uint8_t p[4] = { 0x34u, 0x12u, 0x78u, 0x56u };   /* vid=0x1234 pid=0x5678 */
        uint8_t flags = 0u, pay[4], len = 0u;

        host_name_reset();
        host_tx_reset();
        run_cmd(CMD_SET_USB_ID, 0x53u, BRIDGE_FLAG_ACK_REQ, p, 4u);
        CHECK(pump_find(CMD_SET_USB_ID, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ACK, BRIDGE_FLAG_ACK);
        CHECK_EQ(bridge_cfg_get()->usb_vid, 0x1234u);
        CHECK_EQ(bridge_cfg_get()->usb_pid, 0x5678u);
#if BRIDGE_BOARD_IS_DONGLE
        CHECK_EQ(host_usb_vid(), 0x1234);
        CHECK_EQ(host_usb_pid(), 0x5678);
        CHECK_EQ(host_cfg_relay_len(), -1);
#else
        CHECK_EQ(host_usb_vid(), -1);
        {
            uint8_t rl = 0u;
            const uint8_t *rf = host_cfg_relay(&rl);
            CHECK_EQ(host_cfg_relay_len(), 5);
            CHECK(rf != 0 && rf[0] == CMD_SET_USB_ID && memcmp(&rf[1], p, 4u) == 0);
        }
#endif
    }

    /* 非法 target / 长度不匹配 → 错误帧 PARAM */
    {
        uint8_t bad_target[2] = { 0x07u, 'X' };
        uint8_t flags = 0u, pay[4], len = 0u;

        host_tx_reset();
        run_cmd(CMD_SET_NAME, 0x54u, BRIDGE_FLAG_ACK_REQ, bad_target, 2u);
        CHECK(pump_find(CMD_SET_NAME, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);

        host_tx_reset();
        run_cmd(CMD_SET_USB_ID, 0x55u, BRIDGE_FLAG_ACK_REQ, bad_target, 2u);
        CHECK(pump_find(CMD_SET_USB_ID, &flags, pay, &len, 4u));
        CHECK_EQ(flags & BRIDGE_FLAG_ERR, BRIDGE_FLAG_ERR);
        CHECK_EQ(pay[0], BRIDGE_ECODE_PARAM);
    }
}

static void test_apply_ident_on_boot(void)
{
    CASE("上电应用持久化名称 / VID-PID");

    reset_all();
    host_name_reset();

    /* 默认配置不应触发任何端口调用（避免无谓的 USB 重枚举） */
    bridge_app_init();
    CHECK_EQ(host_name_target(), -1);
    CHECK_EQ(host_usb_vid(), -1);

    const uint8_t ble[] = "KBD-01";
    const uint8_t usb[] = "DONGLE-01";
    CHECK_EQ(bridge_cfg_set_name(BRIDGE_NAME_TARGET_BLE, ble, 6u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_set_name(BRIDGE_NAME_TARGET_USB, usb, 9u), BRIDGE_OK);
    CHECK_EQ(bridge_cfg_set_usb_ids(0x1234u, 0x5678u), BRIDGE_OK);

    host_name_reset();
    bridge_app_init();      /* 模拟重启：从 NV 读回并应用到端口层 */

    uint8_t n = 0u;
    const uint8_t *nm = host_name(&n);
#if BRIDGE_BOARD_IS_DONGLE
    CHECK_EQ(host_name_target(), (int)BRIDGE_NAME_TARGET_USB);
    CHECK_EQ(n, 9u);
    CHECK(nm != 0 && memcmp(nm, usb, 9u) == 0);
    CHECK_EQ(host_usb_vid(), 0x1234);
    CHECK_EQ(host_usb_pid(), 0x5678);
#else
    CHECK_EQ(host_name_target(), (int)BRIDGE_NAME_TARGET_BLE);
    CHECK_EQ(n, 6u);
    CHECK(nm != 0 && memcmp(nm, ble, 6u) == 0);
    CHECK_EQ(host_usb_vid(), -1);          /* 键盘侧无 USB，未落地 */
    CHECK_EQ(host_usb_pid(), -1);
#endif
}

static void test_rf24pair_persist(void)
{
    CASE("2.4G 绑定掉电记忆：store → 重载 → forget");

    reset_all();

    /* 上电未绑定 */
    bridge_rf24pair_init();
    CHECK(!bridge_rf24pair_is_bound());
    CHECK_EQ(bridge_rf24pair_link_id(), (int)BRIDGE_RF24_LINK_ID_NONE);

    /* 落盘后：绑定 + link_id = CRC16(key) */
    const uint8_t key[4]   = { 0x11u, 0x22u, 0x33u, 0x44u };
    const uint8_t peer[4]  = { 0xAAu, 0xBBu, 0xCCu, 0xDDu };
    const uint8_t rand[4]  = { 0x01u, 0x02u, 0x03u, 0x04u };
    bridge_rf24pair_store(key, peer, rand);
    CHECK(bridge_rf24pair_is_bound());
    CHECK_EQ(bridge_rf24pair_link_id(), (int)bridge_crc16_ccitt(key, 4u));

    /* 模拟重启：init 从 NV 读回 */
    bridge_rf24pair_init();
    CHECK(bridge_rf24pair_is_bound());
    CHECK_EQ(bridge_rf24pair_link_id(), (int)bridge_crc16_ccitt(key, 4u));
    CHECK_EQ(bridge_rf24pair_get()->peer_id[0], 0xAAu);

    /* 解绑：清密钥并落盘，重启后保持未绑定 */
    bridge_rf24pair_forget();
    CHECK(!bridge_rf24pair_is_bound());
    bridge_rf24pair_init();
    CHECK(!bridge_rf24pair_is_bound());
}

static void test_mode_name(void)
{
    CASE("模式名与 sel 映射");
    CHECK(strcmp(bridge_mode_name(MODE_USB), "USB") == 0);
    CHECK(strcmp(bridge_mode_name(MODE_2G4), "2G4") == 0);
    CHECK_EQ(bridge_mode_from_sel(0u), (uint8_t)MODE_USB);
    CHECK_EQ(bridge_mode_from_sel(1u), (uint8_t)MODE_BLE);
    CHECK_EQ(bridge_mode_from_sel(2u), (uint8_t)MODE_2G4);
    CHECK_EQ(bridge_mode_from_sel(3u), (uint8_t)MODE_BLE_PAIR);
}

int main(void)
{
    printf("=== CH582F bridge core test (board=%s, baud=%lu, role=%s) ===\n",
           BRIDGE_BOARD_NAME, (unsigned long)BRIDGE_UART_BAUD,
           BRIDGE_BOARD_IS_DONGLE ? "dongle" : "keyboard");

    test_bandwidth_budget();
    test_crc16();
    test_ringbuf();
    test_frame_roundtrip();
    test_sof_resync();
    test_crc_error();
    test_half_frame_timeout();
    test_tx_busy_and_gap();
    test_hid_dedup();
    test_nkro_downgrade();
    test_nkro_mode_routing();
    test_release_before_mode_switch();
    test_vbus_priority();
    test_proto_get_version();
    test_proto_ping_pong();
    test_proto_unsupported();
    test_get_mac();
    test_link_lost_to_sleep();
    test_ping_deferral();
    test_control_queue_when_busy();
    test_log_switch();
    test_cfg_persist();
    test_battery_curve();
    test_evt_battery_report();
    test_srrc_test_mode();
    test_set_mode_via_uart();
    test_get_mode_and_heartbeat();
    test_pair_and_unpair();
    test_unpair_radio_select();
    test_pair_window_timeout();
    test_pair_duration();
    test_sleep_wake_restore_mode();
    test_evt_state_ack_retry();
    test_hid_rejected_when_disconnected();
    test_tunnel_commands();
    test_nv_user_commands();
    test_misc_commands();
    test_get_ble_slots();
    test_sleep_policy();
    test_sleep_cfg();
    test_fw_ota();
    test_set_name_and_usb_id();
    test_apply_ident_on_boot();
    test_rf24pair_persist();
    test_mode_name();

    printf("=== checks: %d passed, %d failed ===\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
