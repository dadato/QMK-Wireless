/* 键盘侧（Bridge on keyboard）板级配置 —— CH582F / QFN28_4x4
 *
 * 依据：wireless/bridge_fw/docs/pinmap_ch58x.md 第 3 章（键盘侧管脚分配）
 * 本文件只含纯数据（端口字符 + 引脚号 + 布尔），不含任何 SDK 类型，
 * 以便 core/ 在主机端也能编译验证。
 *
 * 引脚裁剪（v1.4）：
 *   - 无 MODE_SEL0/1：上电缺省 USB（射频不初始化），模式由 SK32 经 SET_MODE 命令下发；
 *   - 无 BAT_ADC / CHRG_STAT：电量与充电状态一律由 SET_BATTERY(0x1C) 命令输入；
 *   - 无 LINK 指示线；
 *   - 调试串口仅 TX（日志单向输出，不需要 RX）。
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
#define BRIDGE_PIN_WAKE_PORT         'A'    /* PA5  pin 24 输出 高有效 */
#define BRIDGE_PIN_WAKE_NUM          5

/* ---- 量产 ISP 配置引脚（必须 10k 上拉）---- */
#define BRIDGE_PIN_ISP_CFG_PORT      'B'    /* PB22 pin 17 */
#define BRIDGE_PIN_ISP_CFG_NUM       22

/* ---- 调试串口 UART0（printf 日志，仅 TX；严禁改用 UART1）---- */
#define BRIDGE_PIN_DBG_TX_PORT       'B'    /* PB7 pin 14 UART0 TXD */
#define BRIDGE_PIN_DBG_TX_NUM        7

/* ---- 键盘侧不使用 USB / PAIR_KEY / LED_STAT ---- */
#define BRIDGE_BOARD_HAS_USB         0
#define BRIDGE_BOARD_HAS_PAIR_KEY    0

#endif /* BRIDGE_BOARD_H */
