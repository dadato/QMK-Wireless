/* CH582F 透传桥 —— 编译期配置
 *
 * 对应 wireless/bridge_fw/docs/pinmap_ch58x.md 第 0 章 D1~D14 与
 *      wireless/bridge_fw/docs/bridge_impl_checklist.md 第 9 章。
 * 引脚定义在 app/<target>/bridge_board.h（由 -I 选择）。
 */
#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include "bridge_board.h"

/* ---------------- 版本 ---------------- */
#define BRIDGE_FW_VERSION            "0.1.0"
/* HID OTA GET_INFO 回读的数值版本（u16，供上位机比对）；与 BRIDGE_FW_VERSION 同步维护 */
#define BRIDGE_FW_REV                0x0001u

/* ---------------- 桥↔SK32 串口（D2 / D14） ---------------- */
#define BRIDGE_UART_BAUD             921600u
#define BRIDGE_MAX_PAYLOAD           64
/* 921600 时序约束：16B 帧占线约 174µs（10 bit/B）；5ms 为兼容低速/主控
 * 帧间处理开销的保守上界，不随波特率下调。 */
#define BRIDGE_FRAME_MIN_GAP_MS      5u
#define BRIDGE_TX_LATEST_ONLY        1
#define BRIDGE_HID_DEDUP             1

/* ---------------- 协议超时 ---------------- */
#define BRIDGE_PING_PERIOD_MS        1000u
#define BRIDGE_LINK_LOST_CNT         3u
#define BRIDGE_IDLE_TO_SLEEP_S       30u
#define BRIDGE_PAIR_TIMEOUT_S        60u
#define BRIDGE_RX_HALF_FRAME_MS      5u
#define BRIDGE_ACK_TIMEOUT_MS        300u
#define BRIDGE_ACK_MAX_RETRY         3u
#define BRIDGE_CRC_ERR_MAX           10u

/* ---------------- 看门狗（M6） ----------------
 * CH58x WDOG 计数 = Fsys/131072（60MHz → 1 count ≈ 2.185ms，8bit），
 * 上限约 557ms；取 400ms 并每轮主循环喂狗。
 * 睡眠 1s 长于该周期，故进睡前挂起复位使能、唤醒后重挂
 * （见 sdk_wdog_suspend/resume，bridge_port_ch58x.c）。
 * 置 0 即关闭看门狗（sdk_wdog_init(0) → WWDG_ResetCfg(DISABLE)）。 */
#ifndef BRIDGE_WDOG_TIMEOUT_MS
#define BRIDGE_WDOG_TIMEOUT_MS       400u
#endif

/* ---------------- 浅睡（IDLE，保持链路） ----------------
 * 主循环空闲时的有界休眠窗口：链路保持（射频中断唤醒），串口数据由
 * RXD1 下降沿 GPIO 唤醒（sdk_uart1_rx_wake_enable）。
 * 深睡（MODE_SLEEP，射频关闭、1s 窗口）另见 BRIDGE_IDLE_TO_SLEEP_S。 */
#define BRIDGE_SHALLOW_SLEEP_MS      20u

/* ---------------- 接收活动后保持清醒（方案 A） ----------------
 * LIGHT sleep 会停 Fsys → UART1 波特率时钟停，睡眠期间不采样；RXD1 下降沿
 * 只能把内核从 Sleep 唤醒，无法保回**唤醒延迟内已过去的帧首字节**（@921600
 * 单字节 ≈10.9µs），故浅睡期间进入的帧整帧丢失（实测命中率约 0.4%~1%）。
 * 对策：一旦检测到接收活动（RXD1 唤醒事件或收到合法帧），在此时长内禁止
 * 睡眠（浅睡 + 深睡），让主机连续帧得以完整收下——即"break-in 后保持清醒"。
 * 取值必须 > BRIDGE_PING_PERIOD_MS（1000ms）：SK32 主机按 1s 周期发 CMD_PING，
 * 若窗口小于心跳周期，则每次唤醒后都会在下一个 PING 到达前重新入睡，PING 的
 * 帧首字节永远丢失 → 对端 PING 永远收不到 → 链路永远无法恢复（自锁死）。
 * 1500ms 覆盖 1s 心跳 + 抖动与帧间隔背压。 */
#define BRIDGE_RX_HOLD_AWAKE_MS      1500u

/* ---------------- 睡眠窗口后的强制清醒监听窗口 ----------------
 * 每次睡眠（浅睡/深睡）结束后，无论 RXD1 唤醒标志是否置位，都必须再保持清醒
 * 至少该时长才允许下一次进睡。
 * 原因：RXD1 下降沿唤醒存在"唤醒延迟"（GPIO 唤醒 Long_Delay ≈ 3590 cycles），
 * 睡眠期间进入的帧首字节在唤醒延迟内即被丢掉 → 收不到完整帧；若此时唤醒标志
 * 也未置位（ISR 未服务/边沿太窄），rx_gap 不回落 → 设备立刻再睡 → 主机帧永远
 * 落在睡眠窗口里 → 自锁死（实测清醒占空比 <1%，应答率 0%）。
 * 强制监听窗口保证：主机只要在重发（无论是否被 GPIO 唤醒），总会有一次完整帧
 * 落在 UART 已上电的窗口内，从而刷新 BRIDGE_RX_HOLD_AWAKE_MS 并解除自锁。
 * 取值须 ≥ 921600 下一整帧占线时长（8+64=72B ≈ 0.78ms）并留余量。 */
#define BRIDGE_POST_SLEEP_LISTEN_MS  40u

/* ---------------- 键盘侧 UART OTA（WCH IAP 分区） ----------------
 * 分区与标志位布局见 port/ch58x/bridge_ota_layout.h（ImageA/ImageB/ImageIAP）。
 * 镜像上限 = ImageB 暂存区大小（216K）；FW_DATA 单帧净荷 = 64B 帧 - 4B offset。 */
#define BRIDGE_FW_MAX_LEN            (216u * 1024u)
#define BRIDGE_FW_CHUNK_MAX          60u

/* ---------------- 特性开关 ---------------- */
#define BRIDGE_FEATURE_NKRO_BLE      0    /* D3  禁用 NKRO over BLE */
#define BRIDGE_FEATURE_NKRO_2G4      1    /* D4  保留 NKRO over 2.4G */
#define BRIDGE_RF24_REPORT_HZ        1000 /* D5  射频上报率 1ms */
#define BRIDGE_FEATURE_TOUCHKEY      0    /* D10 无触摸 */
#define BRIDGE_FEATURE_EXT_FLASH     0    /* D8  无外挂 Flash */
#define BRIDGE_FEATURE_SRRC_TEST     1    /* D9  定频测试模式 */
#define BRIDGE_FEATURE_RF24_VENDOR   1    /* D11 2.4G 走厂商用例 */
/* 2.4G 跳频 —— 结论：**不启用**（2026-10 依据官方例程 + WCH 论坛确认）。
 *   EVT/EXAM/BLE/RF_PHY_Hop 的 RF_FrequencyHoppingTx/Rx 是**非对称**用法：
 *   TX 端每个周期在 ChannelMap 上把所有频点各发一次，RX 端每个周期只监听
 *   一个频点（见 CH58xBLE_LIB.h：Tx="sending HOP_TX pdu"、Rx="wait for
 *   receiving HOP_TX pdu"，均无应用载荷参数）。本桥是**双向**数据面
 *   （键盘↔Dongle 都要发），无法套用该单向骨架。
 *   WCH 官方（论坛 thread-132278）明确："RF_PHY 例程可实现一跳多并跳频，
 *   但绑定/跳频/透传**需要自己写算法**"，且"跳频通道由上一包数据携带，
 *   通讯失败则暂缓跳频"。故本层保持固定信道 + LLE_MODE_AUTO 自动 ACK/重传；
 *   若后续要做同步跳频，须在 rf24_* 内实现自定义跳频协议（core 无需改动）。
 *   置 1 仅作为将来接入自定义跳频时的预留开关。 */
#define BRIDGE_FEATURE_RF24_HOP      0    /* 2.4G 跳频（自定义算法未实现，见上） */
#define BRIDGE_FEATURE_OTA_SINGLE    1    /* D12：单一应用区 APP_A + Stage 暂存，由常驻 BOOT 搬运；非 A/B 双应用区，可回退 */
#define BRIDGE_MAX_BLE_PROFILE       4

/* ---------------- printf 日志（D13） ---------------- */
#define BRIDGE_LOG_ENABLE            BRIDGE_BOARD_LOG_ENABLE
#define BRIDGE_LOG_LEVEL             BRIDGE_BOARD_LOG_LEVEL
#define BRIDGE_LOG_BAUD              921600u
#define BRIDGE_LOG_BUF_SIZE          256

/* ---------------- 缓冲 ---------------- */
#define BRIDGE_RX_RING_SIZE          300
#define BRIDGE_TX_RING_SIZE          96

/* ---------------- 应用 NV 全槽分配（port 层 4KB = 64 槽 × 64B） ----------------
 *   1..3    配置 / 名称 / 2.4G 绑定记录（bridge_cfg / bridge_rf24pair）
 *   4       多主机槽索引表（bridge_ble_multihost.h）
 *   5..24   预留：原多主机整段 SNV 存档槽区（现改由 SDK 库原生多绑定，不再使用）；
 *           其中槽 8..11（绝对 0x77200~0x772FF）为 OTA 标志页（bridge_ota_layout.h）
 *   25..47  用户存储：给 SK32 用（VIA keymap 备份等），32B 为单元对外寻址
 *   48..63  BLE SNV 区（绝对地址 0x77C00，4×256B，SDK GAPBondMgr 占用，HAL/config.h），
 *           应用侧禁止使用
 * 用户存储容量：(48-25) × 2 = 46 个单元 = 1472B。 */
#define BRIDGE_NV_SLOT_SIZE          64u
#define BRIDGE_NV_SLOT_MAX           64u
#define BRIDGE_NV_USER_BASE          25u
#define BRIDGE_NV_SNV_LO             48u   /* BLE SNV 占用槽（0x77C00 起 4×256B = 16 槽） */
#define BRIDGE_NV_UNIT               32u
#define BRIDGE_NV_UNITS_PER_SLOT     (BRIDGE_NV_SLOT_SIZE / BRIDGE_NV_UNIT)
#define BRIDGE_NV_USER_UNITS         ((BRIDGE_NV_SNV_LO - BRIDGE_NV_USER_BASE) * BRIDGE_NV_UNITS_PER_SLOT)

/* ---------------- 可改写名称 / USB 标识（CMD_SET_NAME / CMD_SET_USB_ID） ----------------
 * 名称单独占一个 NV 槽（见 bridge_cfg.c）。
 * 上限取 21：BLE 侧受 GAP_DEVICE_NAME_LEN(=21) 约束（GGS 设备名与广播内
 * 完整本地名同源），且广播包内 flags(3)+appearance(4)+名结构(2+N) 不超过
 * 31B 的 legacy 上限。USB 侧字符串描述符容量远大于此，取同一上限便于统一。 */
#define BRIDGE_NAME_MAX              21u
#define BRIDGE_NV_ID_CFG             0x0001u
#define BRIDGE_NV_ID_NAMES           0x0002u
#define BRIDGE_NV_ID_RF24_BOND       0x0003u   /* 2.4G 绑定记录（密钥 + 对端 ID，见 bridge_rf24pair.h） */

/* ---------------- 2.4G 配对 / 绑定（bridge_rf24pair.h） ---------------- */
#define BRIDGE_RF24_LINK_ID_NONE     0x0000u   /* 未绑定：数据帧链路 ID */
#define BRIDGE_RF24_LINK_ID_PAIR     0xFFFFu   /* 配对帧链路 ID：任何状态下可收发 */
#define BRIDGE_RF24_PAIR_SEND_MS     200u      /* 键盘侧配对请求重发周期 */

#ifndef BRIDGE_BLE_NAME_DEFAULT
#define BRIDGE_BLE_NAME_DEFAULT      "SK32 Keyboard"
#endif
#ifndef BRIDGE_USB_NAME_DEFAULT
#define BRIDGE_USB_NAME_DEFAULT      "SK32 Wireless"
#endif
/* USB 标识默认值：Dongle 板级可覆盖（VIA 按 (VID,PID) 匹配键盘定义） */
#ifndef BRIDGE_USB_VID
#define BRIDGE_USB_VID               0x0194u
#endif
#ifndef BRIDGE_USB_PID
#define BRIDGE_USB_PID               0x0463u
#endif

/* ---------------- 编译期自检 ---------------- */
#if BRIDGE_MAX_PAYLOAD > 64
#error "BRIDGE_MAX_PAYLOAD 不得超过 64"
#endif
#if BRIDGE_FRAME_MIN_GAP_MS < 5
#error "D14：两帧最小间隔不得小于 5ms（921600 下 16B 帧占线约 174µs）"
#endif
#if defined(BRIDGE_LOG_ENABLE) && BRIDGE_LOG_ENABLE && (BRIDGE_LOG_LEVEL > 4)
#error "BRIDGE_LOG_LEVEL 取值 0..4"
#endif

#endif /* BRIDGE_CONFIG_H */
