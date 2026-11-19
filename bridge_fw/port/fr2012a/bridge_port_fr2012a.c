/* FR2012A 芯片适配层：实现 hal/bridge_port.h
 *
 * 本文件不含任何 SDK 头：硬件原语全部经 bridge_fr2012a_sdk.h，
 * BLE/USB 经 bridge_ble_fr2012a.h / bridge_usb_fr2012a.h。
 */
#include <string.h>

#include "bridge_port.h"
#include "bridge_chip.h"
#include "bridge_fr2012a_sdk.h"
#include "bridge_ble_fr2012a.h"
#include "bridge_usb_fr2012a.h"
#include "bridge_fr2012a.h"
#include "bridge_config.h"
#include "bridge_ringbuf.h"
#include "bridge_log.h"
#include "bridge_types.h"
#include "bridge_mode.h"
#include "bridge_proto.h"
#include "bridge_app.h"
#include "bridge_board.h"

#define BRIDGE_MOD_PORT "port"

/* ============================ UART0 数据面 ============================ */

static uint8_t          s_tx_buf[BRIDGE_TX_RING_SIZE];
static bridge_ringbuf_t s_tx;

void port_uart_init(uint32_t baudrate)
{
    bridge_ringbuf_init(&s_tx, s_tx_buf, (uint16_t)sizeof(s_tx_buf));
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_data_uart_mux(BRIDGE_PIN_UART_RX_PORT, BRIDGE_PIN_UART_RX_NUM,
                      BRIDGE_PIN_UART_TX_PORT, BRIDGE_PIN_UART_TX_NUM);
    sdk_data_uart_init(baudrate);
    LOG_I(BRIDGE_MOD_PORT, "uart0 %.0lu 8N1 (rx=%c%u tx=%c%u)",
          (unsigned long)baudrate,
          BRIDGE_PIN_UART_RX_PORT, BRIDGE_PIN_UART_RX_NUM,
          BRIDGE_PIN_UART_TX_PORT, BRIDGE_PIN_UART_TX_NUM);
#else
    (void)baudrate;
    LOG_I(BRIDGE_MOD_PORT, "uart0 disabled (dongle uses usb path)");
#endif
}

/* 主循环把软件环灌入硬件 FIFO（不阻塞） */
static void port_uart_tx_drain(void)
{
#if !BRIDGE_BOARD_IS_DONGLE
    uint8_t guard = 64u;
    uint8_t b;

    while ((bridge_ringbuf_count(&s_tx) > 0u) && (guard-- > 0u)) {
        if (!bridge_ringbuf_pop(&s_tx, &b)) {
            break;
        }
        if (!sdk_data_uart_write_byte(b)) {
            LOG_W(BRIDGE_MOD_PORT, "uart0 tx stalled, drop 1B");
            break;
        }
    }
#endif
}

void port_uart_write(const uint8_t *data, uint16_t len)
{
    if (data == 0 || len == 0u) {
        return;
    }
#if BRIDGE_BOARD_IS_DONGLE
    return;
#else
    uint16_t n = bridge_ringbuf_write(&s_tx, data, len);
    if (n < len) {
        LOG_W(BRIDGE_MOD_PORT, "tx ring full, drop %u/%u",
              (unsigned)(len - n), len);
    }
#endif
}

bool port_uart_tx_busy(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    return false;
#else
    return (bridge_ringbuf_count(&s_tx) > 0u) || !sdk_data_uart_tx_empty();
#endif
}

void port_uart_tx_flush(void)
{
#if !BRIDGE_BOARD_IS_DONGLE
    for (uint8_t i = 0u; i < 200u; i++) {
        port_uart_tx_drain();
        if (!port_uart_tx_busy()) {
            return;
        }
        sdk_delay_ms(1u);
    }
#endif
}

void port_uart_irq_enable(bool en)
{
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_data_uart_irq_enable(en);
#else
    (void)en;
#endif
}

/* ============================ 时间 ============================ */

uint32_t port_millis(void)
{
    return sdk_millis();
}

void port_delay_ms(uint32_t ms)
{
    sdk_delay_ms(ms);
}

/* ============================ NV（4KB 扇区读-改-写） ============================ */

#define NV_BASE        0x7A000u
#define NV_SECTOR_SIZE 0x1000u
#define NV_SLOT_SIZE   64u
#define NV_SLOT_MAX    (NV_SECTOR_SIZE / NV_SLOT_SIZE)

static uint8_t s_nv_sector[NV_SECTOR_SIZE] __attribute__((aligned(4)));

static uint32_t nv_addr(uint16_t id)
{
    return NV_BASE + ((uint32_t)id * NV_SLOT_SIZE);
}

int port_nv_read(uint16_t id, void *buf, uint16_t len)
{
    if (buf == 0 || len > NV_SLOT_SIZE || id >= NV_SLOT_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    return (sdk_flash_read(nv_addr(id), buf, len) == 0) ? BRIDGE_OK
                                                        : BRIDGE_ERR_IO;
}

int port_nv_write(uint16_t id, const void *buf, uint16_t len)
{
    uint16_t off;

    if (buf == 0 || len > NV_SLOT_SIZE || id >= NV_SLOT_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    if (sdk_flash_read(NV_BASE, s_nv_sector, NV_SECTOR_SIZE) != 0) {
        return BRIDGE_ERR_IO;
    }
    off = (uint16_t)(id * NV_SLOT_SIZE);
    memcpy(&s_nv_sector[off], buf, len);
    if (sdk_flash_erase(NV_BASE, NV_SECTOR_SIZE) != 0) {
        return BRIDGE_ERR_IO;
    }
    if (sdk_flash_write(NV_BASE, s_nv_sector, NV_SECTOR_SIZE) != 0) {
        return BRIDGE_ERR_IO;
    }
    return BRIDGE_OK;
}

/* ============================ 日志（UART2/UART0 TX，按板选择） ============================ */

void port_log_write(const char *s, uint16_t len)
{
    static bool ready;

    if (s == 0 || len == 0u) {
        return;
    }
    if (!ready) {
        sdk_dbg_uart_init(BRIDGE_LOG_BAUD);
        ready = true;
    }
    sdk_dbg_uart_write((const uint8_t *)s, len);
}

/* ============================ GPIO ============================ */

void port_gpio_init(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    sdk_gpio_out_init(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM, false);
    sdk_gpio_in_pullup(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM);
#else
    sdk_gpio_out_init(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM, false);
    sdk_gpio_in_pullup(BRIDGE_PIN_NRESET_PORT, BRIDGE_PIN_NRESET_NUM);
#endif
}

void port_gpio_set_wake(bool active)
{
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_gpio_write(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM, active);
#else
    (void)active;
#endif
}

void port_gpio_set_link(bool connected)
{
#if BRIDGE_BOARD_IS_DONGLE
    sdk_gpio_write(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM, connected);
#else
    (void)connected;
#endif
}

/* ============================ 射频 ============================ */

int port_radio_init(uint8_t mode)
{
    if (mode != BRIDGE_RADIO_BLE && mode != BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    return bridge_fr_ble_start(mode);
}

int port_radio_deinit(uint8_t mode)
{
    if (mode != BRIDGE_RADIO_BLE && mode != BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    bridge_fr_ble_stop();
    return BRIDGE_OK;
}

int port_radio_send(uint8_t mode, const uint8_t *payload, uint8_t len)
{
    if (mode != BRIDGE_RADIO_BLE && mode != BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    return bridge_fr_ble_send(payload, len);
}

bool port_radio_is_connected(uint8_t mode)
{
    if (mode != BRIDGE_RADIO_BLE && mode != BRIDGE_RADIO_2G4) {
        return false;
    }
    return bridge_fr_ble_is_connected();
}

uint8_t port_radio_protocol_ver(uint8_t mode)
{
    (void)mode;
    return 1u;
}

int port_radio_pair_start(uint8_t mode)
{
    if (mode != BRIDGE_RADIO_BLE && mode != BRIDGE_RADIO_2G4) {
        return BRIDGE_ERR_PARAM;
    }
    bridge_fr_ble_pair_start();
    return BRIDGE_OK;
}

int port_radio_pair_stop(uint8_t mode)
{
    (void)mode;
    /* 广播/扫描由模式事务统一停止，配对窗口结束无需额外动作 */
    return BRIDGE_OK;
}

int port_radio_adv_resume(uint8_t mode)
{
    if ((mode != BRIDGE_RADIO_BLE) && (mode != BRIDGE_RADIO_2G4)) {
        return BRIDGE_ERR_PARAM;
    }
    /* 两种模式都靠广播/扫描重建链路：恢复可发现性，但不清绑定 */
    return bridge_fr_ble_start(mode);
}

int port_radio_connect_profile(uint8_t mode, uint8_t profile)
{
    (void)mode;
    (void)profile;
    return BRIDGE_OK;
}

int port_radio_unpair(uint8_t mode, uint8_t profile)
{
    (void)mode;
    (void)profile;
    bridge_fr_ble_unpair();
    return BRIDGE_OK;
}

int port_radio_set_report_period_ms(uint8_t mode, uint16_t ms)
{
    if ((mode != BRIDGE_RADIO_BLE) && (mode != BRIDGE_RADIO_2G4)) {
        return BRIDGE_ERR_PARAM;
    }
    if (ms == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    /* 本芯片两种无线模式共用 BLE 链路：ms → 连接间隔（下限 7.5ms） */
    return bridge_fr_ble_set_interval_ms(ms);
}

int port_radio_diag(uint8_t *out, uint8_t *out_len)
{
    (void)out;
    (void)out_len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_hid_diag(uint8_t *out, uint8_t *out_len)
{
    (void)out;
    (void)out_len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_ble_slots(uint8_t out[5])
{
    (void)out;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_tunnel_send(uint8_t kind, const uint8_t *payload, uint8_t len)
{
    (void)kind;
    (void)payload;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_set_name(uint8_t target, const uint8_t *name, uint8_t len)
{
#if BRIDGE_BOARD_IS_DONGLE
    if (target == BRIDGE_NAME_TARGET_USB) {
        /* 立即替换产品名字符串描述符并重新枚举 */
        return bridge_usb_fr2012a_set_name(name, len);
    }
    return BRIDGE_ERR_UNSUPPORTED;
#else
    if (target == BRIDGE_NAME_TARGET_BLE) {
        bridge_fr_ble_set_name(name, len);
        return BRIDGE_OK;
    }
    return BRIDGE_ERR_UNSUPPORTED;
#endif
}

int port_set_usb_ids(uint16_t vid, uint16_t pid)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* 立即改写设备描述符的 idVendor/idProduct 并重新枚举 */
    return bridge_usb_fr2012a_set_ids(vid, pid);
#else
    (void)vid;
    (void)pid;
    return BRIDGE_ERR_UNSUPPORTED;  /* 键盘侧无 USB：由 core 中继给接收器 */
#endif
}

bool port_is_dongle(void)
{
    return BRIDGE_BOARD_IS_DONGLE != 0;
}

int port_radio_cfg_send(const uint8_t *frame, uint8_t len)
{
    (void)frame;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_chip_get_mac(uint8_t mac[6])
{
    return (sdk_mac_get(mac) == 0) ? BRIDGE_OK : BRIDGE_ERR_IO;
}

int port_radio_test_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
    (void)mode;
    (void)channel;
    (void)power;
    return BRIDGE_ERR_UNSUPPORTED;
}

/* ============================ 固件升级（FR2012A v1 不支持） ============================ */

int port_fw_begin(uint32_t total_len)
{
    (void)total_len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int port_fw_write(uint32_t offset, const uint8_t *d, uint16_t len)
{
    (void)offset;
    (void)d;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

void port_fw_finish(uint16_t crc)
{
    (void)crc;
}

void port_fw_abort(void)
{
}

/* ============================ 电源 / 系统 ============================ */

bool port_vbus_present(void)
{
    return false;   /* 同 CH58x：VBUS 由 SK32 检测 */
}

void port_enter_bootloader(void)
{
    sdk_enter_bootloader();
}

void port_reset(void)
{
    sdk_reset();
}

void port_on_leds(uint8_t led_state)
{
    /* 键盘侧 LED 由 SK32 驱动；Dongle 的 LED 在 port_gpio_set_link 处理 */
    (void)led_state;
}

/* ============================ 芯片入口 ============================ */

void bridge_chip_poll(void)
{
    /* 串口 TX 排空 → BLE 链路维护 → USB(Dongle) → core 主循环 */
    port_uart_tx_drain();
    bridge_fr_ble_poll();
#if BRIDGE_BOARD_IS_DONGLE
    bridge_usb_fr2012a_poll();
#endif
    bridge_app_poll();
}

/* ============================ BLE 层 → core 上抛 ============================ */

static bool s_radio_connected;

void bridge_fr2012a_set_connected(bool on)
{
    s_radio_connected = on;
}

bool bridge_fr2012a_connected(void)
{
    return s_radio_connected;
}

void bridge_fr2012a_radio_evt(uint8_t evt)
{
    if ((evt != BRIDGE_RADIO_EVT_CONNECTED) && (evt != BRIDGE_RADIO_EVT_PAIRED) &&
        (evt != BRIDGE_RADIO_EVT_DISCONNECTED) && (evt != BRIDGE_RADIO_EVT_PAIR_FAILED)) {
        return;
    }
    /* core 的模式状态机负责防卡键（断链先 release_all）与 EVT_STATE 上报 */
    bridge_mode_on_radio_event(evt);
}

void bridge_fr2012a_hid_in(const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* 接收器：键盘上报的 HID 报文 → USB HID 转发主机 */
    if (bridge_usb_fr2012a_send(payload, len) != BRIDGE_OK) {
        LOG_W(BRIDGE_MOD_PORT, "usb hid send failed len=%u", len);
    }
#else
    /* 键盘侧：主机下发的 Output Report（LED，1 字节）→ 回传 SK32 */
    if (len == 1u) {
        bridge_evt_leds(payload[0]);
        port_on_leds(payload[0]);
    }
#endif
}

void bridge_fr2012a_link_rx(const uint8_t *payload, uint8_t len)
{
    if (payload == 0 || len == 0u) {
        return;
    }

#if BRIDGE_BOARD_IS_DONGLE
    /* 接收器是下行帧的发起方，不处理对端下行帧 */
    bridge_fr2012a_hid_in(payload, len);
#else
    /* 私有桥下行帧：[0x01][leds] / [0x02][host_power] */
    if ((len == 2u) && (payload[0] == 0x01u)) {
        bridge_fr2012a_hid_in(&payload[1], 1u);
    } else if ((len == 2u) && (payload[0] == 0x02u)) {
        bridge_mode_set_host_awake(payload[1] != 0u);
    }
#endif
}

