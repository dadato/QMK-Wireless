/* 键盘侧（Bridge on keyboard）板级配置 —— FR2012A
 *
 * 纯数据（端口字符 + 引脚号 + 布尔），不含任何 SDK 类型，
 * 以便 host 端（test_host）也能编译验证。
 *
 * 默认引脚（FR2012A-U EVB，均可在投板时改本文件）：
 *   PA0 / UART0_RXD  ← SK32 TX
 *   PA1 / UART0_TXD  → SK32 RX
 *   PB2              WAKE（输出，高有效）
 *   PB3              nRESET（输入，低有效；由 SK32 控制模组复位）
 *   PA5 / UART2_TXD  调试日志（仅 TX）
 *
 * 注意：PA0/PA1 同时是 ROM 串口烧录口（FREQCHIP_Download TX→PA0、RX→PA1），
 * 烧录时需断开与 SK32 的连接。
 *
 * 与 CH58x 版一致：无 MODE_SEL/LINK/BAT_ADC/CHRG_STAT 引脚；
 * 上电缺省 USB（射频关闭），模式由 SK32 经 SET_MODE 命令下发；
 * 电量/充电状态只由 SET_BATTERY 命令输入。
 */
#ifndef BRIDGE_BOARD_H
#define BRIDGE_BOARD_H

#define BRIDGE_BOARD_NAME            "keyboard"
#define BRIDGE_BOARD_IS_DONGLE       0
#define BRIDGE_BOARD_HAS_USB         0
#define BRIDGE_BOARD_HAS_PAIR_KEY    0

#ifndef BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_BOARD_LOG_ENABLE      1
#endif
#ifndef BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_BOARD_LOG_LEVEL       4
#endif

/* ---- 桥↔SK32 串口 UART0 ---- */
#define BRIDGE_PIN_UART_RX_PORT      'A'
#define BRIDGE_PIN_UART_RX_NUM       0
#define BRIDGE_PIN_UART_TX_PORT      'A'
#define BRIDGE_PIN_UART_TX_NUM       1

/* ---- 控制信号 ---- */
#define BRIDGE_PIN_WAKE_PORT         'B'
#define BRIDGE_PIN_WAKE_NUM          2
#define BRIDGE_PIN_NRESET_PORT       'B'
#define BRIDGE_PIN_NRESET_NUM        3

/* ---- 调试串口（仅 TX）：UART2 / PA5 ---- */
#define BRIDGE_PIN_DBG_TX_PORT       'A'
#define BRIDGE_PIN_DBG_TX_NUM        5
#define BRIDGE_DBG_UART              2

#endif /* BRIDGE_BOARD_H */
