/* 键盘侧（Bridge on keyboard）板级配置 —— CH582F / QFN28_4x4
 *
 * 依据：wireless/ch582f_pinmap.md 第 3 章（键盘侧管脚分配）
 * 本文件只含纯数据（端口字符 + 引脚号 + 布尔），不含任何 SDK 类型，
 * 以便 core/ 在主机端也能编译验证。
 */
#ifndef BRIDGE_BOARD_H
#define BRIDGE_BOARD_H

#define BRIDGE_BOARD_NAME            "keyboard"
#define BRIDGE_BOARD_IS_DONGLE       0

/* ---- 日志（D13）：键盘侧调试构建默认开 ---- */
#ifndef BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_BOARD_LOG_ENABLE      1
#endif
#ifndef BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_BOARD_LOG_LEVEL       4      /* DEBUG */
#endif

/* ---- 桥↔SK32 串口 UART1（D2/D14）---- */
#define BRIDGE_PIN_UART_RX_PORT      'A'    /* PA8 / RXD1  pin 6  */
#define BRIDGE_PIN_UART_RX_NUM       8
#define BRIDGE_PIN_UART_TX_PORT      'A'    /* PA9 / TXD1  pin 7  */
#define BRIDGE_PIN_UART_TX_NUM       9

/* ---- 控制信号 ---- */
/* 键盘侧由 SK32 驱动 MODE_SEL[1:0] 拨码；Dongle 无此信号 */
#define BRIDGE_BOARD_HAS_MODE_SEL    1
#define BRIDGE_PIN_MODE_SEL0_PORT    'B'    /* PB12 pin 11 输入 */
#define BRIDGE_PIN_MODE_SEL0_NUM     12
#define BRIDGE_PIN_MODE_SEL1_PORT    'B'    /* PB13 pin 10 输入 */
#define BRIDGE_PIN_MODE_SEL1_NUM     13
#define BRIDGE_PIN_WAKE_PORT         'A'    /* PA5  pin 24 输出 高有效 */
#define BRIDGE_PIN_WAKE_NUM          5
#define BRIDGE_PIN_LINK_PORT         'A'    /* PA13 pin 27 输出 可选 */
#define BRIDGE_PIN_LINK_NUM          13

/* ---- 电池 / 充电 ---- */
#define BRIDGE_PIN_BAT_ADC_PORT      'A'    /* PA14 pin 26 AIN4 = ADC ch4 */
#define BRIDGE_PIN_BAT_ADC_NUM       14
#define BRIDGE_ADC_CH_BAT            4
#define BRIDGE_PIN_CHRG_PORT         'A'    /* PA15 pin 25 输入 */
#define BRIDGE_PIN_CHRG_NUM          15

/* ---- 量产 ISP 配置引脚（必须 10k 上拉）---- */
#define BRIDGE_PIN_ISP_CFG_PORT      'B'    /* PB22 pin 17 */
#define BRIDGE_PIN_ISP_CFG_NUM       22

/* ---- 调试串口 UART0（printf 日志，严禁改用 UART1）---- */
#define BRIDGE_PIN_DBG_TX_PORT       'B'    /* PB7 pin 14 UART0 TXD */
#define BRIDGE_PIN_DBG_TX_NUM        7
#define BRIDGE_PIN_DBG_RX_PORT       'B'    /* PB4 pin 15 UART0 RXD */
#define BRIDGE_PIN_DBG_RX_NUM        4

/* ---- 键盘侧不使用 USB / PAIR_KEY / LED_STAT ---- */
#define BRIDGE_BOARD_HAS_USB         0
#define BRIDGE_BOARD_HAS_PAIR_KEY    0

/* ---- 电池分压比（需按实际电阻标定）---- */
#define BRIDGE_BAT_DIV_NUM           1
#define BRIDGE_BAT_DIV_DEN           2

#endif /* BRIDGE_BOARD_H */
