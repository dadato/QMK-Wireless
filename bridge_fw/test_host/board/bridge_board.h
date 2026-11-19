/* 主机验证用板级配置（test_host）—— 仅供 PC 端编译 core/ + port/ch58x 使用
 *
 * 保真约定：本文件按角色镜像 app/keyboard/bridge_board.h 与
 *           app/dongle/bridge_board.h 的**被引用宏**，取值必须完全一致。
 *           若在此处放宽（例如额外定义已删除的引脚宏），
 *           port_gpio_init() 就会走一条固件上不存在的分支，
 *           主机测试即失去意义。
 *
 * 角色由 -DBRIDGE_TEST_AS_DONGLE=1 切换，默认按键盘侧角色验证。
 */
#ifndef BRIDGE_BOARD_H
#define BRIDGE_BOARD_H

/* ---- 日志（D13） ---- */
#ifndef BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_BOARD_LOG_ENABLE      1
#endif
#ifndef BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_BOARD_LOG_LEVEL       4      /* DEBUG */
#endif

/* ---- 桥↔SK32 串口 UART1（D2/D14） ---- */
#define BRIDGE_PIN_UART_RX_PORT      'A'    /* PA8 / RXD1 */
#define BRIDGE_PIN_UART_RX_NUM       8
#define BRIDGE_PIN_UART_TX_PORT      'A'    /* PA9 / TXD1 */
#define BRIDGE_PIN_UART_TX_NUM       9

/* ---- 量产 ISP 配置引脚（必须 10k 上拉） ---- */
#define BRIDGE_PIN_ISP_CFG_PORT      'B'    /* PB22 */
#define BRIDGE_PIN_ISP_CFG_NUM       22

/* ---- 调试串口 UART0（仅 TX，日志单向输出） ---- */
#define BRIDGE_PIN_DBG_TX_PORT       'B'    /* PB7  UART0 TXD */
#define BRIDGE_PIN_DBG_TX_NUM        7

#if defined(BRIDGE_TEST_AS_DONGLE) && BRIDGE_TEST_AS_DONGLE

/* ================= Dongle 侧 ================= */
#define BRIDGE_BOARD_NAME            "dongle"
#define BRIDGE_BOARD_IS_DONGLE       1
#define BRIDGE_BOARD_HAS_USB         1
#define BRIDGE_BOARD_HAS_PAIR_KEY    1

/* Dongle 无 SK32，也没有模式选择信号：上电缺省 2.4G */
#define BRIDGE_PIN_LED_STAT_PORT     'A'    /* PA4 */
#define BRIDGE_PIN_LED_STAT_NUM      4
#define BRIDGE_PIN_PAIR_KEY_PORT     'A'    /* PA5 */
#define BRIDGE_PIN_PAIR_KEY_NUM      5

#else

/* ================= 键盘侧 ================= */
#define BRIDGE_BOARD_NAME            "keyboard"
#define BRIDGE_BOARD_IS_DONGLE       0
#define BRIDGE_BOARD_HAS_USB         0
#define BRIDGE_BOARD_HAS_PAIR_KEY    0

/* 控制信号（无 MODE_SEL / LINK） */
#define BRIDGE_PIN_WAKE_PORT         'A'    /* PA5  输出 高有效 */
#define BRIDGE_PIN_WAKE_NUM          5

#endif /* BRIDGE_TEST_AS_DONGLE */

#endif /* BRIDGE_BOARD_H */
