/* printf 日志（D13）
 *
 * 硬约束（见规格 11.1 / 清单 4.11）：
 *   1. 输出必须与桥数据面串口分离：CH58x 用 UART0（PB4/PB7）、数据面 UART1；
 *      FR2012A 用 UART2（PA5）、数据面 UART0（PA0/PA1）
 *   2. bridge_hid 的 0x10~0x15 转发路径内不得调用 LOG_*
 *   3. ISR 内不得调用 bridge_log_printf()
 *   4. 量产 BRIDGE_LOG_ENABLE=0 时所有 LOG_* 宏展开为空，零开销
 */
#ifndef BRIDGE_LOG_H
#define BRIDGE_LOG_H

#include "bridge_config.h"
#include <stdint.h>

#define BRIDGE_LOG_OFF       0u
#define BRIDGE_LOG_ERROR     1u
#define BRIDGE_LOG_WARN      2u
#define BRIDGE_LOG_INFO      3u
#define BRIDGE_LOG_DEBUG     4u

void    bridge_log_init(uint32_t baudrate);
void    bridge_log_set_level(uint8_t level);
uint8_t bridge_log_get_level(void);

void bridge_log_printf(uint8_t level, const char *module, const char *fmt, ...);

#if BRIDGE_LOG_ENABLE
  #define LOG_E(mod, ...)  bridge_log_printf(BRIDGE_LOG_ERROR, mod, __VA_ARGS__)
  #define LOG_W(mod, ...)  bridge_log_printf(BRIDGE_LOG_WARN,  mod, __VA_ARGS__)
  #define LOG_I(mod, ...)  bridge_log_printf(BRIDGE_LOG_INFO,  mod, __VA_ARGS__)
  #define LOG_D(mod, ...)  bridge_log_printf(BRIDGE_LOG_DEBUG, mod, __VA_ARGS__)
#else
  #define LOG_E(mod, ...)  do { } while (0)
  #define LOG_W(mod, ...)  do { } while (0)
  #define LOG_I(mod, ...)  do { } while (0)
  #define LOG_D(mod, ...)  do { } while (0)
#endif

#endif /* BRIDGE_LOG_H */
