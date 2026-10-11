/* Wireless bridge client frame layer (pure logic, no platform dependency).
 *
 * This is the SK32/QMK-side client re-export of the CH582F module bridge
 * protocol under the wb_* / WB_* namespace.  Wire format and all protocol
 * constant VALUES are identical to wireless/ch582f_fw/core/bridge_proto.h —
 * only the client-side symbol names changed (bridge/br/BR prefixes became
 * wb/WB).  There are no embedded comment terminators anywhere in this file.
 * Do NOT change any numeric value here: the module firmware (and its Dongle /
 * boot / iap peers) decode the exact same bytes.
 *
 * Wire format (little-endian CRC, matching wireless/ch582f_fw/core):
 *   AA 55 | LEN | CMD | SEQ | FLAGS | PAYLOAD(LEN) | CRC16(LE)
 *   +--------+-----+-----+-----+-------+-------------+-------+
 *   | SOF    | 1B  | 1B  | 1B  |  1B   |   N B       | 2B(LE)|
 *   +--------+-----+-----+-----+-------+-------------+-------+
 * CRC covers LEN+CMD+SEQ+FLAGS+PAYLOAD using CRC-16/CCITT-FALSE
 * (init 0xFFFF, poly 0x1021, no reflection).  Self-test vector:
 * "123456789" -> 0x29B1.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------- commands: SK32 -> bridge ---------------- */
#define WB_CMD_GET_VERSION    0x01u
#define WB_CMD_GET_STATUS     0x02u
#define WB_CMD_SET_MODE       0x03u
#define WB_CMD_SET_PROFILE    0x04u
#define WB_CMD_PAIR_START     0x05u
#define WB_CMD_UNPAIR         0x06u
#define WB_CMD_RESET          0x07u
#define WB_CMD_ENTER_BOOT     0x08u
#define WB_CMD_SET_CONFIG     0x09u
#define WB_CMD_GET_MAC        0x0Au
#define WB_CMD_SET_TEST_MODE  0x0Bu
#define WB_CMD_GET_MODE       0x0Cu   /* 模式状态查询：应答 [mode][connected][radio] */
#define WB_CMD_SET_HEARTBEAT  0x0Du   /* 心跳周期开关：[enable u8]（1=开/0=关） */
#define WB_CMD_GET_WIRELESS_STATE 0x0Eu /* 无线综合状态查询：应答 [wl_state u8]（WB_WL_*） */
#define WB_CMD_TUNNEL_USB     0x18u
#define WB_CMD_TUNNEL_BLE     0x19u
#define WB_CMD_NV_WRITE       0x1Au
#define WB_CMD_NV_READ        0x1Bu
#define WB_CMD_SET_BATTERY    0x1Cu   /* 下发电量：[pct u8] */
#define WB_CMD_SLEEP          0x1Du
#define WB_CMD_GET_ADC        0x1Eu
#define WB_CMD_SET_NAME       0x1Fu   /* [target][name...]，target 见 WB_NAME_TARGET_* */
#define WB_CMD_SET_USB_ID     0x20u   /* [vid LE(2)][pid LE(2)] */
/* 键盘侧 UART OTA（WCH IAP 分区，仅键盘侧支持）：
 *   0x21 FW_BEGIN [total_len u32 LE][crc16 LE(2)] -> 应答 [chunk_max u8]
 *   0x22 FW_DATA  [offset u32 LE][data <=60B]
 *   0x23 FW_END   无载荷（CRC 校验通过则置标志复位进 IAP）
 *   0x24 FW_ABORT 无载荷 */
#define WB_CMD_FW_BEGIN       0x21u
#define WB_CMD_FW_DATA        0x22u
#define WB_CMD_FW_END         0x23u
#define WB_CMD_FW_ABORT       0x24u
#define WB_CMD_HID_KEYBOARD   0x10u
#define WB_CMD_HID_NKRO       0x11u
#define WB_CMD_HID_CONSUMER   0x12u
#define WB_CMD_HID_MOUSE      0x13u
#define WB_CMD_HID_SYSTEM     0x14u
#define WB_CMD_HID_RELEASE    0x15u
#define WB_CMD_PING           0x7Fu

/* ---------------- events: bridge -> SK32 ---------------- */
#define WB_EVT_STATE          0x80u
#define WB_EVT_LEDS           0x81u
#define WB_EVT_BATTERY        0x82u
#define WB_EVT_PAIR_RESULT    0x83u
#define WB_EVT_ERROR          0x84u
#define WB_EVT_TUNNEL_USB     0x85u
#define WB_EVT_TUNNEL_BLE     0x86u
#define WB_EVT_HOST_POWER     0x87u   /* 主机电源状态变化：[state]（WB_HOST_*） */
#define WB_EVT_PONG           0x7Fu

/* ---------------- frame flags ---------------- */
#define WB_FLAG_ACK_REQ       0x01u
#define WB_FLAG_ACK           0x02u
#define WB_FLAG_ERR           0x04u

/* ---------------- report lengths ---------------- */
#define WB_REPORT_KBD_LEN     8u
#define WB_REPORT_NKRO_LEN    16u
#define WB_REPORT_CONSUMER_LEN 2u
#define WB_REPORT_MOUSE_LEN   4u
#define WB_REPORT_SYSTEM_LEN  1u

/* tunnel max payload (VIA raw HID is 32 bytes here) */
#define WB_TUNNEL_MAX_LEN     32u

/* ---------------- host power state (EVT_HOST_POWER payload) ---------------- */
#define WB_HOST_AWAKE         0u   /* 主机运行 / 已从挂起恢复 */
#define WB_HOST_SUSPEND       1u   /* 主机挂起（睡眠） */

/* ---------------- wireless aggregate state (CMD_GET_WIRELESS_STATE) -------
 * Mirrors BRIDGE_WL_* (wireless/ch582f_fw/core/bridge_types.h): the module
 * folds [mode][connected][radio] into one byte so SK32 needs no combination
 * logic. */
#define WB_WL_USB             0u   /* 有线 USB（射频关） */
#define WB_WL_IDLE            1u   /* 无线空闲：非配对窗口且未连接 */
#define WB_WL_BLE_PAIRING     2u   /* BLE 配对窗口进行中 */
#define WB_WL_2G4_PAIRING     3u   /* 2.4G 配对窗口进行中 */
#define WB_WL_BLE_LINKED      4u   /* BLE 已连接 */
#define WB_WL_2G4_LINKED      5u   /* 2.4G 已连接 */
#define WB_WL_BLE_DISCONN     6u   /* BLE 已配对但当前无连接（重连中） */
#define WB_WL_2G4_DISCONN     7u   /* 2.4G 无连接 */
#define WB_WL_OTA             8u   /* OTA 升级中 */
#define WB_WL_SLEEP           9u   /* 休眠中 */
#define WB_WL_UNKNOWN         0xFFu /* 尚未查询到 */

/* ---------------- SET_CONFIG keys (CMD_SET_CONFIG) ---------------- */
#define WB_CFG_KEY_REPORT_PERIOD 0x01u
#define WB_CFG_KEY_LOG_LEVEL     0x02u
#define WB_CFG_KEY_ADV_NAME_MAC  0x03u
#define WB_CFG_KEY_CONN_INTERVAL 0x04u
#define WB_CFG_KEY_BATTERY_SRC   0x05u
#define WB_CFG_KEY_SLEEP_POLICY  0x06u

/* ---------------- battery source (WB_CFG_KEY_BATTERY_SRC) ---------------- */
#define WB_BATT_SRC_AUTO      0u
#define WB_BATT_SRC_LOCAL     1u
#define WB_BATT_SRC_EXTERNAL  2u

/* ---------------- sleep policy (WB_CFG_KEY_SLEEP_POLICY) ---------------- */
#define WB_SLEEP_POLICY_AUTO   0u
#define WB_SLEEP_POLICY_MANUAL 1u

/* ---------------- name target (CMD_SET_NAME) ---------------- */
#define WB_NAME_TARGET_BLE    0u
#define WB_NAME_TARGET_USB    1u

/* ---------------- frame layout ---------------- */
#define WB_SOF0               0xAAu
#define WB_SOF1               0x55u
#define WB_SOF_LEN            2u
#define WB_CRC_LEN            2u
#define WB_HDR_LEN            4u  /* LEN + CMD + SEQ + FLAGS */
#define WB_FRAME_OVERHEAD     (WB_SOF_LEN + WB_HDR_LEN + WB_CRC_LEN) /* 8 */
#define WB_MAX_PAYLOAD        64u
#define WB_FRAME_MAX          (WB_FRAME_OVERHEAD + WB_MAX_PAYLOAD)  /* 72 */

/* ---------------- modes (bridge_mode.h) ---------------- */
#define WB_MODE_USB           0u
#define WB_MODE_BLE           1u
#define WB_MODE_BLE_PAIR      2u
#define WB_MODE_2G4           3u
#define WB_MODE_2G4_PAIR      4u
#define WB_MODE_OTA           5u
#define WB_MODE_SLEEP         6u

/* ---------------- sleep levels ---------------- */
#define WB_SLEEP_WAKE         0u
#define WB_SLEEP_IDLE         1u
#define WB_SLEEP_DEEP         2u

/* ---------------- EVT_ERROR codes ---------------- */
#define WB_ECODE_CRC          0x01u
#define WB_ECODE_UART_LOST    0x02u
#define WB_ECODE_RADIO        0x03u
#define WB_ECODE_PAIR         0x04u
#define WB_ECODE_ACK_TIMEOUT  0x05u
#define WB_ECODE_PARAM        0x06u
#define WB_ECODE_STATE        0x07u
#define WB_ECODE_ROLLOVER     0x08u

/* ---------------- decoded frame ---------------- */
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t flags;
    uint8_t len;
    uint8_t payload[WB_MAX_PAYLOAD];
} wb_frame_t;

/* ---------------- API ---------------- */

/* CRC-16/CCITT-FALSE over len bytes, init 0xFFFF. */
uint16_t wb_crc16(const uint8_t *data, uint16_t len);

/* Encode a frame into out (capacity cap).  Returns frame length, or 0 if it
 * does not fit. */
uint16_t wb_encode(uint8_t *out, uint16_t cap, uint8_t cmd, uint8_t seq,
                   uint8_t flags, const uint8_t payload[], uint8_t len);

/* Incremental RX state machine: feed one byte at a time.  Returns true and
 * fills *out when a complete, CRC-valid frame was assembled.  Bad SOF / length
 * / CRC reset the parser. */
void     wb_rx_reset(void);
bool     wb_rx_byte(uint8_t b, wb_frame_t *out);

/* Monotonic-millis source plugged by the transport layer (wb_uart.c). */
extern uint32_t wb_now_ms(void);

/* Sequence number helpers. */
uint8_t  wb_seq_next(void);
uint8_t  wb_seq_peek(void);
