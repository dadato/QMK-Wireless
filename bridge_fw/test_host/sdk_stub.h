/* 主机端 SDK 桩：在 PC 上顶替 bridge_ch58x_sdk.c
 *
 * 目的：port 层（bridge_port_ch58x.c / bridge_radio_ch58x.c /
 *       bridge_ch58x_callbacks.c）只依赖 bridge_ch58x_sdk.h，
 *       故用本桩顶替后即可在 PC 上真编译真运行，验证
 *       串口 FIFO 背压 / NV 读改写 / ADC 分压 / GPIO 极性 /
 *       射频参数翻译 / 回调翻译 等全部 port 层逻辑。
 *
 * 桩的保真约定（偏离处必须在此声明，避免"测了个假的"）：
 *   1. UART1 TX/RX FIFO 深度 = 8（与 CH58x 一致）；
 *      TX 写入超深即丢并计数，RX 注入超深即丢并计数。
 *   2. Flash 写入按 NOR 语义 **按位与**（只清位），擦除置 0xFF，
 *      擦除地址/长度必须 256B 对齐 —— 因此"漏擦除 / 漏读改写"
 *      这类缺陷会被真实暴露。
 *   3. GPIO 读：输出模式返回输出寄存器，输入模式返回注入电平。
 *   4. 时间只按毫秒累加，不模拟 0.625ms TMOS tick 换算。
 *      换算逻辑在 bridge_ch58x_sdk.c（含 SDK 头，不可主机编译），
 *      故 sdk_millis()/sdk_sleep() 的 tick 换算是**目标侧专有**、不在主机覆盖范围。
 *   5. sdk_reset()/sdk_enter_bootloader() 只计数，不真的复位。
 */
#ifndef SDK_STUB_H
#define SDK_STUB_H

#include <stdint.h>
#include <stdbool.h>

#define STUB_FIFO_DEPTH    8u
#define STUB_TX_BUF_SIZE   8192u
#define STUB_LOG_BUF_SIZE  4096u
#define STUB_RADIO_BUF     512u
#define STUB_FLASH_SIZE    0x8000u   /* DataFlash 全量 32KB */

/* ---------------- 复位 ---------------- */
void stub_reset(void);          /* 清全部桩状态（含 flash 镜像、GPIO、射频、时间） */

/* ---------------- 时间 ---------------- */
void     stub_advance_ms(uint32_t dt);
uint32_t stub_now_ms(void);

/* ---------------- UART1（桥数据面） ---------------- */
extern uint8_t  g_stub_u1_tx[STUB_TX_BUF_SIZE];   /* 已"移出" FIFO 的字节 */
extern uint16_t g_stub_u1_tx_len;
extern uint32_t g_stub_u1_tx_drop;                /* FIFO 溢出丢弃计数 */

void     stub_u1_tx_reset(void);
void     stub_u1_tx_drain(void);                  /* FIFO → g_stub_u1_tx */
void     stub_u1_rx_push(const uint8_t *d, uint16_t len);
uint32_t stub_u1_rx_drop(void);
uint32_t stub_u1_rx_reads(void);
uint32_t stub_u1_iir_reads(void);
bool     stub_u1_inited(void);
uint32_t stub_u1_baud(void);
bool     stub_u1_rx_irq(void);

/* ---------------- UART0（printf 日志） ---------------- */
extern uint8_t  g_stub_u0_tx[STUB_LOG_BUF_SIZE];  /* 以 '\0' 结尾，便于直接打印 */
extern uint16_t g_stub_u0_tx_len;

uint32_t stub_u0_init_count(void);
uint32_t stub_u0_baud(void);
void     stub_u0_tx_reset(void);

/* ---------------- Flash / DataFlash ---------------- */
void     stub_flash_clear(void);                  /* 全片 0xFF + 清计数与故障注入 */
int      stub_flash_peek(uint32_t addr, void *buf, uint16_t len);
void     stub_flash_poke(uint32_t addr, const uint8_t *d, uint16_t len);
void     stub_flash_fail_write(bool fail);
void     stub_flash_fail_erase(bool fail);
uint32_t stub_flash_erase_count(void);
uint32_t stub_flash_write_count(void);

/* ---------------- GPIO ---------------- */
#define STUB_GPIO_IN_FLOAT  0u
#define STUB_GPIO_IN_PU     1u
#define STUB_GPIO_OUT       2u
#define STUB_GPIO_IN_PD     3u

void     stub_gpio_set_all_in(bool level);
void     stub_gpio_set_in(uint8_t port, uint8_t pin, bool level);   /* 注入外部电平 */
bool     stub_gpio_get_out(uint8_t port, uint8_t pin);
uint8_t  stub_gpio_mode(uint8_t port, uint8_t pin);

/* ---------------- ADC ---------------- */
void     stub_adc_set_mv(uint8_t ch, uint16_t mv);
uint32_t stub_adc_reads(void);

/* ---------------- 射频（BLE / 2.4G / USB） ---------------- */
extern uint8_t  g_stub_ble_tx[STUB_RADIO_BUF];
extern uint16_t g_stub_ble_tx_len;
extern uint8_t  g_stub_ble_tx_rid;   /* 最近一次 sdk_ble_hid_send 的 Report ID */
extern uint8_t  g_stub_rf24_tx[STUB_RADIO_BUF];
extern uint16_t g_stub_rf24_tx_len;
extern uint8_t  g_stub_usb_tx[STUB_RADIO_BUF];
extern uint16_t g_stub_usb_tx_len;

/* 只清抓包与计数，**不动**链路状态（与 port_host.c 的约定一致） */
void     stub_radio_obs_reset(void);
/* 清链路状态（已初始化/已连接），用于回到"未上电"状态 */
void     stub_radio_clear_links(void);

void     stub_ble_set_connected(bool c);
uint32_t stub_ble_init_count(void);
uint32_t stub_ble_deinit_count(void);
uint32_t stub_ble_pair_count(void);
uint32_t stub_ble_pair_stop_count(void);
int      stub_ble_profile(void);            /* -1 = 未调用 */
int      stub_ble_adv_name_mac(void);       /* -1 = 未调用 */
int      stub_ble_interval_units(void);     /* -1 = 未调用 */
int      stub_ble_test_ch(void);            /* -1 = 未调用 */
int      stub_ble_unpair_profile(void);     /* -1 = 未调用 */
/* 名称 / USB 标识（CMD_SET_NAME / CMD_SET_USB_ID）观测器 */
const uint8_t *stub_ble_adv_name(uint8_t *len);   /* 已设置的 BLE 广播名；NULL = 未调用 */
const uint8_t *stub_usb_prod_name(uint8_t *len);  /* 已设置的 USB 产品名；NULL = 未调用 */
int      stub_usb_vid(void);                       /* -1 = 未调用 */
int      stub_usb_pid(void);                       /* -1 = 未调用 */
int      stub_rf24_cfg_len(void);                  /* 最近一次配置中继帧长度；-1 = 未调用 */

void     stub_rf24_fail_init(bool fail);
void     stub_rf24_fail_send(bool fail);
void     stub_rf24_set_connected(bool c);
uint32_t stub_rf24_init_count(void);
uint32_t stub_rf24_deinit_count(void);
uint32_t stub_rf24_pair_count(void);
uint32_t stub_rf24_pair_stop_count(void);
uint32_t stub_rf24_unpair_count(void);
int      stub_rf24_period_ms(void);         /* -1 = 未调用 */

void     stub_usb_set_configured(bool c);
void     stub_usb_fail_send(bool fail);
uint32_t stub_usb_init_count(void);

/* 透传（0x88/0x89）桩：抓包写入 g_stub_usb_tx / g_stub_rf24_tx / g_stub_ble_tx */
int      sdk_usb_tunnel_send(const uint8_t *d, uint8_t len);
int      sdk_rf24_send_tunnel(const uint8_t *d, uint8_t len);
int      sdk_ble_tunnel_send(const uint8_t *d, uint8_t len);
int      sdk_rf24_send_pair(const uint8_t *d, uint8_t len);  /* 2.4G 配对应答帧 */
int      sdk_rf24_send_host(uint8_t state);                  /* 2.4G 主机电源状态 */

/* ---------------- 系统 ---------------- */
uint32_t stub_sys_init_count(void);
uint32_t stub_reset_count(void);
uint32_t stub_boot_count(void);
uint32_t stub_ota_flag_count(void);    /* sdk_ota_flag_set_reset 调用次数 */
uint8_t  stub_ota_flag(void);          /* 最近一次写入的 OTA 标志 */
uint32_t stub_tmos_count(void);
uint32_t stub_sleep_count(void);
uint32_t stub_sleep_last_ms(void);
void     stub_ble_mac_set(const uint8_t mac[6]);

/* 看门狗桩 */
uint32_t stub_wdog_init_count(void);   /* sdk_wdog_init(非0) 调用次数 */
uint16_t stub_wdog_timeout_ms(void);   /* 最近一次超时参数 */
bool     stub_wdog_suspended(void);    /* 当前是否处于 suspend 状态 */

/* RXD1 唤醒源桩 */
uint32_t stub_uart1_rx_wake_count(void);   /* sdk_uart1_rx_wake_enable 调用次数 */
bool     stub_uart1_rx_wake_on(void);      /* 最近一次开关状态 */
/* 模拟「睡眠窗口内 RXD1 唤醒过」（sdk_uart1_rx_woke 读后自清） */
void     stub_uart1_rx_woke_set(bool v);

/* 固件升级桩 */
uint32_t stub_fw_finish_count(void);   /* sdk_fw_finish 调用次数 */
uint16_t stub_fw_finish_crc(void);     /* sdk_fw_finish 收到的 CRC */

#endif /* SDK_STUB_H */
