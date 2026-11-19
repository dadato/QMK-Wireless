/* FR2012A（FreqChip FR800x）BLE 协议栈配置 —— 取自厂商 ble_simple_central/code/app_config.h
 *
 * 只保留本工程用到的项；协议栈闭源库在 ble_stack_configure() 时按这些值分配资源。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* 96MHz：BLE 库要求（48MHz 亦可，但厂商量产配置为 96MHz） */
#define SYSTEM_CLOCK_SEL                5

/* 低功耗 RC 校准计数（driver_pmu.c 使用，取厂商默认） */
#define LP_RC_CALIB_CNT                 60
#define LP_RC_CALIB_LARGE_CNT           10000

/* 厂商日志：本工程不调用 log_init()，但为避免任何库内部路径占用数据面 UART0
 * （键盘 PA0/PA1）或调试口 UART2（PA5），把厂商 LOG 固定到 UART1(PA2/PA3)。 */
#define LOG_ENABLE
#define LOG_OUTPUT_RTT                  0
#define LOG_OUTPUT_UART0                1
#define LOG_OUTPUT_UART1                2
#define LOG_OUTPUT_SEL                  LOG_OUTPUT_UART1
#define LOG_USE_SIMPLIFIED_PRINTF
#define LOG_UART_BAUDRATE_CFG           9216
#define LOG_UART_RX_PORT_SEL            2       /* PA2 */
#define LOG_UART_TX_PORT_SEL            3       /* PA3 */

/* 512KB Flash 分区：BLE 绑定信息 / 对端服务表 / 密钥存储 */
#define FLASH_CAPACITY                  0x00080000
#define BLE_STACK_KEY_STORAGE_OFFSET    0x7F000
#define BLE_BONDING_INFO_SAVE_ADDR      0x7D000
#define BLE_REMOTE_SERVICE_SAVE_ADDR    0x7E000

/* 协议栈能力：模组/接收器都只需要 1 条链，这里留 2 条余量 */
#define BLE_STACK_ENABLE_MESH           0
#define BLE_STACK_ENABLE_CONNECTIONS    2
#define BLE_STACK_RX_BUFFER_CNT         8
#define BLE_STACK_RX_BUFFER_SIZE        64
#define BLE_STACK_TX_BUFFER_CNT         8
#define BLE_STACK_TX_BUFFER_SIZE        64
#define BLE_STACK_ADV_BUFFER_SIZE       31
#define BLE_STACK_RETENTION_RAM_SIZE    0xC000

#endif /* APP_CONFIG_H */
