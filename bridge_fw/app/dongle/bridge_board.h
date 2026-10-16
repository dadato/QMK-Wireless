/* Dongle 侧（CH582F）板级配置 —— QFN28_4x4
 *
 * 依据：wireless/bridge_fw/docs/pinmap_ch58x.md 第 4 章（Dongle 侧管脚分配）
 * Dongle 不使用 UART1 与 SK32 对接（无 SK32），仅保留调试串口，
 * 通过 USB1 向主机上报 HID。
 */
#ifndef BRIDGE_BOARD_H
#define BRIDGE_BOARD_H

#define BRIDGE_BOARD_NAME            "dongle"
#define BRIDGE_BOARD_IS_DONGLE       1

#ifndef BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_BOARD_LOG_ENABLE      1
#endif
#ifndef BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_BOARD_LOG_LEVEL       4
#endif

/* ---- Dongle 无 SK32，也没有 MODE_SEL[1:0] 拨码：
 * port_gpio_read_mode_sel 固定返回 0（core 取 USB 值后强制回 2.4G）。 ---- */
#define BRIDGE_BOARD_HAS_MODE_SEL    0

/* ---- Dongle 无 SK32 侧串口；此处仅给占位，避免编译期引用为空 ---- */
#define BRIDGE_PIN_UART_RX_PORT      'A'
#define BRIDGE_PIN_UART_RX_NUM       8
#define BRIDGE_PIN_UART_TX_PORT      'A'
#define BRIDGE_PIN_UART_TX_NUM       9

/* ---- 状态指示 / 配对按键 ---- */
#define BRIDGE_PIN_LED_STAT_PORT     'A'    /* PA4 pin 23 */
#define BRIDGE_PIN_LED_STAT_NUM      4
#define BRIDGE_PIN_PAIR_KEY_PORT     'A'    /* PA5 pin 24 */
#define BRIDGE_PIN_PAIR_KEY_NUM      5

/* ---- 调试串口 UART0 ---- */
#define BRIDGE_PIN_DBG_TX_PORT       'B'    /* PB7 pin 14 */
#define BRIDGE_PIN_DBG_TX_NUM        7
#define BRIDGE_PIN_DBG_RX_PORT       'B'    /* PB4 pin 15 */
#define BRIDGE_PIN_DBG_RX_NUM        4

/* ---- 量产 ISP 配置引脚 ---- */
#define BRIDGE_PIN_ISP_CFG_PORT      'B'    /* PB22 pin 17 */
#define BRIDGE_PIN_ISP_CFG_NUM       22

/* ---- USB1（Dongle 必用）：PB10=UD- pin13 / PB11=UD+ pin12 ---- */
#define BRIDGE_BOARD_HAS_USB         1
#define BRIDGE_BOARD_HAS_PAIR_KEY    1

/* ---- USB 设备描述符标识（VIA 按 (VID,PID) 匹配键盘定义，必须与目标键盘
 *      via.json 一致；默认取 KB17 0x0194/0x0463，配套 LD7_OLED 改 0x0464）---- */
#define BRIDGE_USB_VID               0x0194u
#define BRIDGE_USB_PID               0x0463u

/* Dongle 无电池采样 */
#define BRIDGE_PIN_BAT_ADC_PORT      'A'
#define BRIDGE_PIN_BAT_ADC_NUM       14
#define BRIDGE_ADC_CH_BAT            4
#define BRIDGE_PIN_CHRG_PORT         'A'
#define BRIDGE_PIN_CHRG_NUM          15

#define BRIDGE_BAT_DIV_NUM           1
#define BRIDGE_BAT_DIV_DEN           1

#endif /* BRIDGE_BOARD_H */
