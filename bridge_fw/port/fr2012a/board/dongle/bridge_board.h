/* 接收器（USB Dongle）板级配置 —— FR2012A
 *
 * 引脚（FR2012A-U Dongle 参考设计）：
 *   PA4 / USB_DP、PA5 / USB_DM   （功能号 0x05）
 *   PB1                          LED_STAT（输出，链路指示）
 *   PB0                          PAIR_KEY（输入上拉，按下=进入配对）
 *   PA1 / UART0_TXD              调试日志（仅 TX）
 */
#ifndef BRIDGE_BOARD_H
#define BRIDGE_BOARD_H

#define BRIDGE_BOARD_NAME            "dongle"
#define BRIDGE_BOARD_IS_DONGLE       1
#define BRIDGE_BOARD_HAS_USB         1
#define BRIDGE_BOARD_HAS_PAIR_KEY    1

#ifndef BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_BOARD_LOG_ENABLE      1
#endif
#ifndef BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_BOARD_LOG_LEVEL       4
#endif

/* Dongle 无 SK32 串口；数据面 UART0 不启用，占位宏保持 port 层可编译 */
#define BRIDGE_PIN_UART_RX_PORT      'A'
#define BRIDGE_PIN_UART_RX_NUM       2
#define BRIDGE_PIN_UART_TX_PORT      'A'
#define BRIDGE_PIN_UART_TX_NUM       3

/* ---- 状态指示 / 配对按键 ---- */
#define BRIDGE_PIN_LED_STAT_PORT     'B'
#define BRIDGE_PIN_LED_STAT_NUM      1
#define BRIDGE_PIN_PAIR_KEY_PORT     'B'
#define BRIDGE_PIN_PAIR_KEY_NUM      0

/* ---- USB1（Dongle 必用）---- */
#define BRIDGE_PIN_USB_DP_PORT       'A'
#define BRIDGE_PIN_USB_DP_NUM        4
#define BRIDGE_PIN_USB_DM_PORT       'A'
#define BRIDGE_PIN_USB_DM_NUM        5

/* ---- 调试串口（仅 TX）：UART0 / PA1 ---- */
#define BRIDGE_PIN_DBG_TX_PORT       'A'
#define BRIDGE_PIN_DBG_TX_NUM        1
#define BRIDGE_DBG_UART              0

/* ---- USB 设备描述符标识（与目标键盘 via.json 保持一致）---- */
#define BRIDGE_USB_VID               0x0194u
#define BRIDGE_USB_PID               0x0463u

#endif /* BRIDGE_BOARD_H */
