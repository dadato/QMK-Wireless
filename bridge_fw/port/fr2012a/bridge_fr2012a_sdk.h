/* FR2012A SDK 原语边界（平台唯一接触点）
 *
 * 与 port/ch58x 同构：port 层逻辑只依赖本头文件，SDK 头仅在本文件的 .c 中出现，
 * 因此 port 逻辑可被 test_host/sdk_stub.c 顶替后验证。
 */
#ifndef BRIDGE_FR2012A_SDK_H
#define BRIDGE_FR2012A_SDK_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 系统 / 时间 ---------------- */
void     sdk_sys_init(void);
/* 1ms 时基（os_timer）：协议栈初始化之后调用 */
void     sdk_tick_init(void);
uint32_t sdk_millis(void);
void     sdk_delay_ms(uint32_t ms);

/* ---------------- UART0：桥数据面 ---------------- */
void     sdk_data_uart_init(uint32_t baud);
/* 写一个字节（等待 TX FIFO 非满）；返回 false 表示超时/未就绪 */
bool     sdk_data_uart_write_byte(uint8_t b);
bool     sdk_data_uart_tx_empty(void);
void     sdk_data_uart_irq_enable(bool en);

/* ---------------- 调试串口：printf 日志（仅 TX） ----------------
 * 键盘 = UART2(PA5)，接收器 = UART0(PA1)，由板级 BRIDGE_DBG_UART 选择。 */
void     sdk_dbg_uart_init(uint32_t baud);
void     sdk_dbg_uart_write(const uint8_t *d, uint16_t len);

/* ---------------- GPIO ---------------- */
void     sdk_gpio_out_init(char port, uint8_t pin, bool high);
void     sdk_gpio_in_pullup(char port, uint8_t pin);
void     sdk_gpio_write(char port, uint8_t pin, bool high);
bool     sdk_gpio_read(char port, uint8_t pin);
/* 把 UART0 的 RX/TX 引脚印到给定端口/引脚（板级头指定） */
void     sdk_data_uart_mux(char rx_port, uint8_t rx_pin,
                           char tx_port, uint8_t tx_pin);
void     sdk_gpio_mux_gpio(char port, uint8_t pin);

/* ---------------- Flash（内部 512KB） ---------------- */
int      sdk_flash_read(uint32_t addr, void *buf, uint16_t len);
int      sdk_flash_write(uint32_t addr, const void *buf, uint16_t len);
int      sdk_flash_erase(uint32_t addr, uint32_t len);
uint32_t sdk_flash_id(void);

/* ---------------- 芯片信息 / 系统控制 ---------------- */
int      sdk_mac_get(uint8_t mac[6]);
void     sdk_reset(void);
void     sdk_enter_bootloader(void);

/* ---------------- 低功耗（v1 保持常醒） ---------------- */
void     sdk_sleep_disable(void);

/* ---------------- 射频增强（厂商完整 SDK 才具备的接口） ----------------
 * 依据 FR800x-U_SDK_Guide.pdf §9「保证快速响应对应接口」：
 *   disenable_conn_param_check() + evt_time_min_slot_enable(true) + double rate
 *   可把最小连接间隔从 7.5ms 压到 1.25ms。
 * 本包随附的 libfr800x_stack.a 未导出这些符号，故全部按**弱符号**接入：
 *   存在 → 自动启用并返回 true；不存在 → 跳过并返回 false（回落到 7.5ms）。
 * 换用导出这些接口的完整 SDK 库后无需改代码即生效。 */
bool     sdk_rf_fast_response_enable(void);
bool     sdk_rf_fast_response_supported(void);

/* 按 SDK Guide §4 设置发射功率（本库导出 system_set_tx_power） */
void     sdk_rf_set_tx_power_max(void);

#endif /* BRIDGE_FR2012A_SDK_H */
