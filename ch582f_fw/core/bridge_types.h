/* 公共类型与错误码 */
#ifndef BRIDGE_TYPES_H
#define BRIDGE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------- 命令码：SK32 → 桥 ---------------- */
#define CMD_GET_VERSION      0x01u
#define CMD_GET_STATUS       0x02u
#define CMD_SET_MODE         0x03u
#define CMD_SET_PROFILE      0x04u
#define CMD_PAIR_START       0x05u
#define CMD_UNPAIR           0x06u
#define CMD_RESET            0x07u
#define CMD_ENTER_BOOT       0x08u
#define CMD_SET_CONFIG       0x09u
#define CMD_GET_MAC          0x0Au
#define CMD_SET_TEST_MODE    0x0Bu
#define CMD_GET_MODE         0x0Cu   /* 模式状态查询：应答 [mode][connected][radio] */
#define CMD_SET_HEARTBEAT    0x0Du   /* 心跳周期开关：payload [enable u8]（1=开/0=关） */
#define CMD_GET_WIRELESS_STATE 0x0Eu /* 无线综合状态查询：应答 [wl_state u8]（BRIDGE_WL_*），免组合判断 */
#define CMD_GET_BLE_SLOTS    0x0Fu   /* BLE 多主机槽绑定状态查询：应答 [s1][s2][s3][s4][last_active] */

/* ---- 借鉴市售模组指令集（「一、基本数据」/「二、控制命令」）新增 ----
 * 仅取其能力语义，帧格式仍用本协议（SOF+LEN+CMD+SEQ+FLAGS+PAYLOAD+CRC）：
 *   0x88/0x23 透传数据(64B)  → CMD_TUNNEL_USB / EVT_TUNNEL_USB（VIA Raw HID 通道）
 *   0x89/0x24 BLE 透传(32B)  → CMD_TUNNEL_BLE / EVT_TUNNEL_BLE
 *                              （手机 APP GATT 通道 / usevia.app 的 WebHID Vendor 通道）
 *   0x87      下发电量       → CMD_SET_BATTERY
 *   0x4A/4B/4C 睡眠分级      → CMD_SLEEP
 *   0x4D      获取 ADC       → CMD_GET_ADC（应答 2B 小端 mV）
 *   0x8A/0x8B 读写 EEPROM    → CMD_NV_WRITE / CMD_NV_READ（32B/单元） */
#define CMD_TUNNEL_USB       0x18u
#define CMD_TUNNEL_BLE       0x19u
#define CMD_NV_WRITE         0x1Au
#define CMD_NV_READ          0x1Bu
#define CMD_SET_BATTERY      0x1Cu
#define CMD_SLEEP            0x1Du
#define CMD_GET_ADC          0x1Eu
#define CMD_SET_NAME         0x1Fu   /* payload: [target][name...]，见 BRIDGE_NAME_TARGET_* */
#define CMD_SET_USB_ID       0x20u   /* payload: [vid LE(2)][pid LE(2)] */

/* ---- 键盘侧 UART OTA（WCH IAP 分区，仅键盘侧支持；Dongle 返回 ERR STATE） ----
 *   0x21 FW_BEGIN：payload [total_len u32 LE][crc16 LE(2)] → 应答 [chunk_max u8]
 *   0x22 FW_DATA ：payload [offset u32 LE][data ≤60B]（offset 必须=已写入量，4 对齐）
 *   0x23 FW_END  ：无载荷（CRC 在线计算；校验通过 → ACK → 置标志复位进 IAP）
 *   0x24 FW_ABORT：无载荷（放弃本次传输，恢复默认无线模式） */
#define CMD_FW_BEGIN         0x21u
#define CMD_FW_DATA          0x22u
#define CMD_FW_END           0x23u
#define CMD_FW_ABORT         0x24u

/* 0x25 睡眠时间配置（AUTO 自动休眠策略下的 IDLE/DEEP 门限）：
 *   payload [idle_s LE16][deep_s LE16]，单位秒，各 2 字节。
 *     idle_s：距最近一次串口接收活动达该秒数后允许浅睡（IDLE），0 = 禁用自动浅睡；
 *     deep_s：未连接且空闲达该秒数后进深睡（DEEP），0 = 禁用自动深睡。
 *   落盘保存（bridge_cfg）；仅 AUTO 策略下生效。 */
#define CMD_SLEEP_CFG        0x25u

/* 0x26 电量上报配置（EVT_BATTERY 的开关与发送机制）：
 *   payload [enable][on_change][period_s]，各 1 字节。
 *     enable    ：EVT_BATTERY 总开关，0 = 关 / 1 = 开；
 *     on_change ：电量变化时立即上报，0 = 关 / 1 = 开；
 *     period_s  ：定时上报周期，单位 S，0 = 关闭定时上报。
 *   落盘保存（bridge_cfg）；EVT_BATTERY 仅在 battery_src = LOCAL（本地 ADC）时才会上报。 */
#define CMD_BATTERY_CFG      0x26u

/* 0x2A 诊断快照（仅排障用，不在市售模组指令集内）：
 * 应答 24B 基础 + 16B UART 接收腿取证，共 42B，布局见 port/ch58x/bridge_ch58x_sdk.h
 * 的 sdk_rf24_diag()。 */
#define CMD_GET_DIAG         0x2Au

/* 0x2B BLE HID 服务注册取证（仅排障用）：应答 26B，布局见
 * port/ch58x/bridge_ble_hidsvc.c 的 port_hid_diag()。 */
#define CMD_HID_DIAG         0x2Bu

#define CMD_HID_KEYBOARD     0x10u
#define CMD_HID_NKRO         0x11u
#define CMD_HID_CONSUMER     0x12u
#define CMD_HID_MOUSE        0x13u
#define CMD_HID_SYSTEM       0x14u
#define CMD_HID_RELEASE      0x15u

#define CMD_PING             0x7Fu

/* ---------------- 命令码：桥 → SK32 ---------------- */
#define EVT_STATE            0x80u
#define EVT_LEDS             0x81u
#define EVT_BATTERY          0x82u
#define EVT_PAIR_RESULT      0x83u
#define EVT_ERROR            0x84u
#define EVT_TUNNEL_USB       0x85u   /* 上报透传（USB/Dongle ↔ 上位机），<=64B */
#define EVT_TUNNEL_BLE       0x86u   /* 上报 BLE 透传（手机 APP / VIA WebHID），<=32B */
#define EVT_HOST_POWER       0x87u   /* 主机电源状态变化，payload [state]（BRIDGE_HOST_*） */
#define EVT_PONG             0x7Fu

/* ---------------- 透传通道类型（port_radio_tunnel_send 的 kind） ---------------- */
#define BRIDGE_TUNNEL_USB    0x01u
#define BRIDGE_TUNNEL_BLE    0x02u

/* ---------------- 名称设置目标（CMD_SET_NAME 的 target） ---------------- */
#define BRIDGE_NAME_TARGET_BLE  0x00u   /* BLE 广播名：键盘侧本地生效 */
#define BRIDGE_NAME_TARGET_USB  0x01u   /* Dongle USB 产品名：键盘侧经 2.4G 转发 */

/* ---------------- 电量来源（SET_CONFIG key 0x05） ---------------- */
#define BRIDGE_BATT_SRC_AUTO     0u   /* 外部有效则用外部，否则本地 ADC（默认，兼容旧行为） */
#define BRIDGE_BATT_SRC_LOCAL    1u   /* 仅本地 ADC */
#define BRIDGE_BATT_SRC_EXTERNAL 2u   /* 仅 SK32 上传 */

/* 透传单帧载荷上限（受 BRIDGE_MAX_PAYLOAD=64 限制） */
#define BRIDGE_TUNNEL_USB_LEN  64u
#define BRIDGE_TUNNEL_BLE_LEN  32u

/* 射频链路包类型：2.4G 数据面在载荷前置 1 字节类型（HID 报文 / 透传 / 心跳） */
#define BRIDGE_RADIO_PKT_HID       0x01u
#define BRIDGE_RADIO_PKT_TUNNEL    0x02u
#define BRIDGE_RADIO_PKT_HEARTBEAT 0x03u   /* 空载荷保活帧：仅维持链路活性，不派发 */
#define BRIDGE_RADIO_PKT_CFG       0x04u   /* 配置中继：载荷 = [CMD][payload...]（键盘 → Dongle） */
#define BRIDGE_RADIO_PKT_PAIR      0x05u   /* 配对帧：载荷 = [sub][kbd_id 4][rand 4][...]（见 bridge_rf24pair.h） */
#define BRIDGE_RADIO_PKT_HOST      0x06u   /* 主机电源状态：载荷 = [state]（Dongle → 键盘，BRIDGE_HOST_*） */

/* ---------------- 主机电源状态（EVT_HOST_POWER / BRIDGE_RADIO_PKT_HOST） ---------------- */
#define BRIDGE_HOST_AWAKE    0u   /* 主机运行 / 已从挂起恢复 */
#define BRIDGE_HOST_SUSPEND  1u   /* 主机挂起（睡眠） */

/* ---------------- 睡眠分级（CMD_SLEEP 的 level） ---------------- */
#define BRIDGE_SLEEP_WAKE    0u   /* 退出睡眠 */
#define BRIDGE_SLEEP_IDLE    1u   /* 浅睡（保持连接） */
#define BRIDGE_SLEEP_DEEP     2u   /* 深睡（断连） */

/* ---------------- 睡眠策略（SET_CONFIG key 0x06） ----------------
 *  AUTO   ：模块自行判闲休眠（浅睡随主循环空闲自动进行；30s 无活动深睡）
 *  MANUAL ：仅指令控制休眠——不自动进睡，只有收到 CMD_SLEEP(IDLE/DEEP) 才睡 */
#define BRIDGE_SLEEP_POLICY_AUTO     0u
#define BRIDGE_SLEEP_POLICY_MANUAL   1u

/* ---------------- 帧标志 ---------------- */
#define BRIDGE_FLAG_ACK_REQ  0x01u
#define BRIDGE_FLAG_ACK      0x02u
#define BRIDGE_FLAG_ERR      0x04u

/* ---------------- 返回码 ---------------- */
#define BRIDGE_OK                0
#define BRIDGE_ERR_BUSY         (-1)   /* 串口忙 */
#define BRIDGE_ERR_GAP          (-2)   /* 距上一帧不足最小间隔 */
#define BRIDGE_ERR_DUP          (-3)   /* HID 报文与上次相同，已抑制 */
#define BRIDGE_ERR_PARAM        (-4)
#define BRIDGE_ERR_STATE        (-5)
#define BRIDGE_ERR_VBUS         (-6)   /* VBUS 在场，拒绝无线模式 */
#define BRIDGE_ERR_UNSUPPORTED  (-7)
#define BRIDGE_ERR_IO           (-8)
#define BRIDGE_ERR_CRC          (-9)   /* 固件镜像 CRC 校验失败（FW_END） */

/* ---------------- EVT_ERROR 错误码 ---------------- */
#define BRIDGE_ECODE_CRC         0x01u
#define BRIDGE_ECODE_UART_LOST   0x02u
#define BRIDGE_ECODE_RADIO       0x03u
#define BRIDGE_ECODE_PAIR        0x04u
#define BRIDGE_ECODE_ACK_TIMEOUT 0x05u
#define BRIDGE_ECODE_PARAM       0x06u
#define BRIDGE_ECODE_STATE       0x07u
#define BRIDGE_ECODE_ROLLOVER    0x08u

/* ---------------- 键值规模 ---------------- */
#define BRIDGE_REPORT_KEYBOARD_LEN  8u
#define BRIDGE_REPORT_NKRO_LEN      16u
#define BRIDGE_REPORT_CONSUMER_LEN  2u
#define BRIDGE_REPORT_MOUSE_LEN     4u
#define BRIDGE_REPORT_SYSTEM_LEN    1u

/* NKRO 位图 bit n 对应 HID usage = n + BRIDGE_NKRO_USAGE_BASE */
#define BRIDGE_NKRO_USAGE_BASE      4u
#define BRIDGE_NKRO_MAX_KEYS        6u
#define BRIDGE_HID_ROLLOVER_ERROR   0x01u

/* ---------------- 射频通道编号（port_radio_* 的 mode 参数） ---------------- */
#define BRIDGE_RADIO_NONE   0u
#define BRIDGE_RADIO_BLE    1u
#define BRIDGE_RADIO_2G4    2u

/* ---------------- 无线综合状态（CMD_GET_WIRELESS_STATE 应答单字节） ----------------
 * 把 [mode][connected][radio] 三要素折叠成一个枚举，SK32 直接查表即可，无需自行
 * 组合判断。配对成功/失败属瞬时事件（非稳态），仍由 EVT_PAIR_RESULT 上报。 */
#define BRIDGE_WL_USB          0u   /* 有线 USB 模式（射频关） */
#define BRIDGE_WL_IDLE         1u   /* 无线空闲：非配对窗口且未连接 */
#define BRIDGE_WL_BLE_PAIRING  2u   /* BLE 配对窗口进行中 */
#define BRIDGE_WL_2G4_PAIRING  3u   /* 2.4G 配对窗口进行中 */
#define BRIDGE_WL_BLE_LINKED   4u   /* BLE 已连接 */
#define BRIDGE_WL_2G4_LINKED   5u   /* 2.4G 已连接 */
#define BRIDGE_WL_BLE_DISCONN  6u   /* BLE 已配对但当前无连接（重连中） */
#define BRIDGE_WL_2G4_DISCONN  7u   /* 2.4G 无连接 */
#define BRIDGE_WL_OTA          8u   /* OTA 升级中 */
#define BRIDGE_WL_SLEEP        9u   /* 休眠中 */

/* Boot 键盘报文可表达的 usage 上界（0x65 之后为多媒体/系统键） */
#define BRIDGE_BOOT_USAGE_MAX       0x65u

#endif /* BRIDGE_TYPES_H */
