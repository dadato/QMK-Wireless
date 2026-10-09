/* 2.4G 射频接入 —— 厂商 RF_PHY 例程的薄封装（清单 D11）
 *
 * 参考例程（本工程 submodule openwch/ch583 @ bd508ad7）：
 *   EVT/EXAM/BLE/RF_PHY/APP/RF_PHY.c   RF_Init() 的 rfConfig 构造与收发骨架
 *   EVT/EXAM/BLE/RF_PHY/APP/RF_main.c  TMOS 任务 + RF_2G4StatusCallBack 事件化
 *   EVT/EXAM/BLE/RF_PHY_Hop/           跳频变体（RF_FrequencyHoppingTx/Rx），
 *                                     需与对端严格同步，本层暂未启用（见文末说明）
 *
 * 设计约束（与 core 的分工）：
 *   1. 本文件只做「HID/透传报文 ↔ RF 原语」的搬运与链路状态翻译，
 *      **不改射频时序/跳频算法**；协议语义（配对、去重、ACK、上报）都在 core。
 *   2. RF_Tx/RF_Rx/RF_Shut/RF_Config 必须在**任务上下文**调用（厂商例程明确要求
 *      不得在 RF 回调里直接调用），故所有 RF 动作都经 TMOS 事件投递到
 *      rf24_process_event()，回调只搬数据 + 置事件。
 *   3. 数据面帧格式 = [type][link_id LE(2)][payload]：
 *      - link_id = CRC16(绑定密钥)，由 core 的 bridge_rf24pair 派生；本层在
 *        收包侧校验（不符即丢弃、不刷新心跳），发包侧自动附加
 *      - 配对帧（BRIDGE_RADIO_PKT_PAIR）link_id 恒 0xFFFF，任何状态可收发
 *      收包校验通过后剥去 link_id，交 sdk_rf24_on_recv() 分流（见 callbacks）。
 *
 * 本文件包含 WCH SDK 头，**不参与** test_host 构建（见 test_host/Makefile）。
 */
#include "CONFIG.h"            /* BLE 配置 + 协议栈头 + 外设头 */
#include "HAL.h"               /* HAL_Init / 系统服务 */
#include "bridge_ch58x_sdk.h"
#include "bridge_ch58x.h"
#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_rf24pair.h"

/* ------------------------------ 参数 ------------------------------ */
/* rfConfig：与 RF_PHY 例程同源，字段含义见 CH58xBLE_LIB.h 的 rfConfig_t */
#define RF24_ACCESS_ADDRESS   0x71764129u   /* 禁止 0x55555555 / 0xAAAAAAAA */
#define RF24_CRC_INIT         0x555555u
#define RF24_CHANNEL          8u            /* 仅在未置 EX_CHANNEL 位时生效（0-39 → 2404+n*2 MHz） */
#define RF24_FREQUENCY        2480000u      /* 2480MHz：置 LLE_MODE_EX_CHANNEL 后以此为准 */
#define RF24_RX_MAXLEN        251u
#define RF24_TX_MAXLEN        251u
/* ★根因（真机定位“收帧错误率 100%、几乎零有效帧”）：
 *   此前 LLEMode 只置 LLE_MODE_AUTO，未置 LLE_MODE_EX_CHANNEL(bit6)。
 *   头文件明确：bit6=0 时射频用 **Channel(0-39)** 选频，bit6=1 才用 **Frequency**。
 *   故 cfg.Frequency=2480000 从未生效，实际频点 = Channel 8 → 2420MHz。
 *   2420MHz 正落在 **WiFi 信道 1（2401~2423MHz）** 内，被环境 Wi-Fi 持续占满：
 *   两端都能“收到噪声”（RX_MODE_RX_DATA 命中）但 **rsr bit0=1（CRC 全错）**，
 *   硬件不解码、不回 ACK → 对端 TX 全部超时，链路停在 PAIRING（实测
 *   rx_data==rx_data_rsr、tx_timeout≈tx_ok、bound=0）。
 *   厂商唯一实发路径（RF_PHY/APP/RF_PHY.c 的 RF_Init）用
 *     LLE_MODE_BASIC|LLE_MODE_EX_CHANNEL + Frequency=2480000（2480MHz，高于 WiFi ch13）。
 *   本层对齐：置 LLE_MODE_EX_CHANNEL，让 Frequency=2480MHz 生效，避开 Wi-Fi 频段；
 *   仍保留 AUTO 自动 ACK/重传以获得链路级可靠性。
 *   其余字段（ChannelMap / HopPeriod / HopIndex / HeartPeriod）保持 0（非跳频）。 */
#define RF24_NONHOP_MODE      LLE_MODE_EX_CHANNEL
#define RF24_CHANNEL_MAP      0u            /* 0 = 不激活跳频数据连接 */
#define RF24_HOP_PERIOD       0u            /* 非跳频：保持 0 */
#define RF24_HOP_INDEX        0u            /* 非跳频：保持 0 */
#define RF24_HEART_PERIOD     0u            /* 非跳频例程不设置该字段 */

/* 发送队列（修复「单邮箱静默覆盖」丢帧）：
 *   旧实现只有 1 个邮箱：s_tx_pending 未消费时新帧直接覆写且仍返回 OK；
 *   且 rf24_send_mail() 在 RF_Tx 后立刻清 pending，下一帧的 RF_Shut() 会
 *   打断**在飞 TX**（LLE_MODE_AUTO 的自动 ACK/重传），→ 丢 HID 报文
 *   （主机侧表现为丢键 / 松开帧丢失导致连发）。
 *   现改为环形队列 + 在飞标志 s_tx_busy：入队不覆盖，只有 TX_FINISH/TX_FAIL
 *   回调才推进队列，绝不在在飞期间 RF_Shut。
 *   槽位 = 3B 帧头（type + link_id LE） + payload（≤ BRIDGE_MAX_PAYLOAD=64）+ 余量。 */
#define RF24_TX_QUEUE_DEPTH   4u            /* 队列深度（覆盖 RF_Tx 往返时延） */
#define RF24_TX_SLOT          68u           /* = [len] + type + link_id(2) + payload(64) */
#define RF24_REPORT_PERIOD_MS 1u            /* 默认 1ms（D5）；SET_CONFIG REPORT_PERIOD 可覆盖 */

/* ------------------------------ 事件 ------------------------------ */
#define RF24_EVT_TX     0x0001u   /* 有待发报文：RF_Shut + RF_Tx */
#define RF24_EVT_RX     0x0002u   /* 进入接收：RF_Shut + RF_Rx */
#define RF24_EVT_DATA   0x0004u   /* 收到数据（任务上下文投递） */
#define RF24_EVT_HB     0x0008u   /* 1s 心跳 / 断链判定 */
#define RF24_EVT_PAIR   0x0010u   /* 配对窗口：键盘侧周期重发 PAIR_REQ */
#define RF24_EVT_TXDONE 0x0020u   /* TX_FINISH/TX_FAIL：推进发送队列 / 回接收态 */

#define RF24_LINK_LOST_CNT  3u    /* 连续 3 次心跳无收 → 断链（规格 6.2） */
#define RF24_HB_PERIOD_MS   1000u

/* ------------------------------ 状态 ------------------------------ */
static uint8_t  s_task;
static bool     s_task_registered;  /* TMOS 任务只注册一次（见 sdk_rf24_init） */
static bool     s_up;
static bool     s_conn;
static bool     s_role_inited;
static uint16_t s_report_period_ms = RF24_REPORT_PERIOD_MS;

static uint8_t  s_tx_q[RF24_TX_QUEUE_DEPTH][RF24_TX_SLOT];
static uint8_t  s_tx_q_head;                 /* 队首（正在发 / 待发） */
static uint8_t  s_tx_q_cnt;
static bool     s_tx_busy;                   /* RF_Tx 已提交、等 ACK/超时（TX 时序在飞） */
static uint32_t s_tx_drop;                   /* 队列满 / 提交失败导致的丢帧计数（观测） */
/* RX_MODE_RX_DATA(校验通过) → RX_MODE_TX_FINISH/FAIL 期间，硬件正在自动回 ACK。
 * 这是**非终态**：此窗口内任何 RF_Shut 都会打断 ACK，使对端 TX 收不到应答。
 * 置位期间禁止 pump_tx / arm_rx。 */
static bool     s_rx_acking;

static uint8_t  s_rx_mail[RF24_RX_MAXLEN];
static uint8_t  s_rx_len;
static bool     s_rx_pending;

static uint8_t  s_hb_miss;

/* ------------------------------ 建链取证计数 ------------------------------
 * 2.4G 配对/建链失败时，串口日志不可用（键盘侧 COM10 只跑桥协议、Dongle 无日志
 * 出口），故在此累计射频层各关键路径的命中次数，双端用同一份：
 *   键盘：CMD_GET_DIAG（UART1）读出；Dongle：HID OTA GET_INFO 尾部读出。
 * 全部饱和到 0xFFFF，避免回绕歧义。 */
static uint16_t s_diag_tx_submit_ok;   /* RF_Tx 提交成功 */
static uint16_t s_diag_tx_submit_fail; /* RF_Tx 提交失败（丢本帧） */
static uint16_t s_diag_tx_done_ok;     /* TX_FINISH（对端 ACK 或广播完成） */
static uint16_t s_diag_tx_done_fail;   /* TX_FAIL（重传耗尽） */
static uint16_t s_diag_rx_any;         /* 射频层收到任何一包（rsr==0） */
static uint16_t s_diag_rx_pair;        /* 收到的配对帧数 */
static uint16_t s_diag_rx_data_bad;    /* 数据帧被丢弃：未绑定 / link_id 不符 */

static void diag_inc(uint16_t *c)
{
    if (*c < 0xFFFFu) {
        (*c)++;
    }
}

static uint16_t rf24_process_event(uint8_t task_id, uint16_t events);

/* ------------------------------ 收发原语（仅任务上下文） ------------------------------ */

static void rf24_arm_rx(void)
{
    /* LLE_MODE_AUTO 下 RF_Rx 的 txBuf/txLen = 「收到包后硬件自动回给对方的数据(ACK)载荷」，
     * 不是本端接收缓冲。若误传本端接收缓冲/RF24_RX_MAXLEN，则每收一包都会把陈旧的接收
     * 缓冲当作 251B 载荷回射，对端按数据帧解析 → link_id 校验失败 → rx_data_bad 高企。
     * 这里只回 1 字节哨兵：保留 auto 模式回包(ACK)的时序，同时因长度 < 3 会被对端
     * rf24_deliver() 的帧头检查静默丢弃。 */
    static uint8_t s_ack_dummy = 0u;

    (void)RF_Shut();
    (void)RF_Rx(&s_ack_dummy, 1u, 0xFFu, 0xFFu);
}

static void rf24_tx_reset(void)
{
    s_tx_q_head = 0u;
    s_tx_q_cnt  = 0u;
    s_tx_busy   = false;
    s_rx_acking = false;
    s_tx_drop   = 0u;
}

/* 提交队首帧到射频。只有 TX_FINISH/TX_FAIL 回调（RF24_EVT_TXDONE）才清除
 * s_tx_busy 并出队，因此**在飞期间不会被后续 RF_Shut() 打断**。
 * 提交失败（射频忙/地址错）时丢本帧并计数，避免队列卡死。 */
static void rf24_pump_tx(void)
{
    uint8_t len;

    if (!s_up || s_tx_busy || s_rx_acking || s_tx_q_cnt == 0u) {
        return;
    }

    len = s_tx_q[s_tx_q_head][0];
    if (len == 0u) {
        s_tx_q_head = (uint8_t)((s_tx_q_head + 1u) % RF24_TX_QUEUE_DEPTH);
        s_tx_q_cnt--;
        return;
    }

    (void)RF_Shut();
    /* LLE_MODE_AUTO：发完自动等待对端 ACK 并回到接收态（由回调驱动） */
    if (RF_Tx(&s_tx_q[s_tx_q_head][1], len, 0xFFu, 0xFFu) != SUCCESS) {
        s_tx_q_head = (uint8_t)((s_tx_q_head + 1u) % RF24_TX_QUEUE_DEPTH);
        s_tx_q_cnt--;
        s_tx_drop++;
        diag_inc(&s_diag_tx_submit_fail);
        return;
    }
    s_tx_busy = true;
    diag_inc(&s_diag_tx_submit_ok);
}

static void rf24_deliver(void)
{
    uint8_t  type;
    uint16_t fid;
    uint8_t  tmp[RF24_RX_MAXLEN];
    uint8_t  plen;

    if (!s_rx_pending) {
        return;
    }
    s_rx_pending = false;
    if (s_rx_len < 3u) {
        return;                     /* 帧头（type + link_id）不完整 */
    }

    type = s_rx_mail[0];
    fid  = (uint16_t)((uint16_t)s_rx_mail[1] | ((uint16_t)s_rx_mail[2] << 8));

    if (type == BRIDGE_RADIO_PKT_PAIR) {
        /* 配对帧：任何状态下可收发（link_id 恒 0xFFFF，不做密钥校验） */
        bool was_bound = bridge_rf24pair_is_bound();

        diag_inc(&s_diag_rx_pair);
        plen  = (uint8_t)(s_rx_len - 3u);
        tmp[0] = type;
        for (uint8_t i = 0u; i < plen; i++) {
            tmp[1u + i] = s_rx_mail[3u + i];
        }
        s_hb_miss = 0u;
        sdk_rf24_on_recv(tmp, (uint8_t)(1u + plen));
        /* 仅当**本帧恰好完成绑定**（未绑定→绑定）才建链：
         * 建链事件由 callbacks 的 PAIRED 上报，这里只置 RF 层连接态。 */
        if (!was_bound && bridge_rf24pair_is_bound()) {
            s_conn = true;
        }
        return;
    }

    /* 数据帧：须已绑定且链路 ID 匹配，否则丢弃（防同环境多套串扰）。
     * 未绑定 / 密钥不符的帧**不刷新心跳**，避免陌生射频撑活本端链路。 */
    if (!bridge_rf24pair_is_bound()) {
        diag_inc(&s_diag_rx_data_bad);
        return;
    }
    if (fid != bridge_rf24pair_link_id()) {
        diag_inc(&s_diag_rx_data_bad);
        return;
    }

    plen  = (uint8_t)(s_rx_len - 3u);
    tmp[0] = type;
    for (uint8_t i = 0u; i < plen; i++) {
        tmp[1u + i] = s_rx_mail[3u + i];
    }
    if (!s_conn) {
        s_conn = true;
        sdk_rf24_on_link(true);
    }
    s_hb_miss = 0u;
    sdk_rf24_on_recv(tmp, (uint8_t)(1u + plen));
}

/* 投递待发报文（仅入队 + 置事件，不碰寄存器）。
 * 帧格式 = [type][link_id LE(2)][payload]；link_id 由绑定密钥派生：
 *   数据帧 = CRC16(绑定密钥)，未绑定 = 0xFFFF；配对帧恒 = 0xFFFF。
 * 队列满时丢**最旧**一帧（HID 绝对报文语义：保最新状态），并计数 s_tx_drop。 */
static int rf24_post(uint8_t type, const uint8_t *d, uint8_t len)
{
    uint8_t  slot;
    uint16_t lid;

    if (!s_up) {
        return BRIDGE_ERR_STATE;
    }
    if ((d == 0) && (len > 0u)) {
        return BRIDGE_ERR_PARAM;
    }
    if ((uint16_t)len + 4u > (uint16_t)RF24_TX_SLOT) {
        return BRIDGE_ERR_PARAM;
    }

    if (type == BRIDGE_RADIO_PKT_PAIR) {
        lid = BRIDGE_RF24_LINK_ID_PAIR;
    } else if (bridge_rf24pair_is_bound()) {
        lid = bridge_rf24pair_link_id();
    } else {
        lid = BRIDGE_RF24_LINK_ID_PAIR;   /* 未绑定：仅配对/心跳可发，均不承载数据 */
    }

    if (s_tx_q_cnt >= RF24_TX_QUEUE_DEPTH) {
        s_tx_q_head = (uint8_t)((s_tx_q_head + 1u) % RF24_TX_QUEUE_DEPTH);
        s_tx_q_cnt--;
        s_tx_drop++;
    }
    slot = (uint8_t)((s_tx_q_head + s_tx_q_cnt) % RF24_TX_QUEUE_DEPTH);

    /* 线上帧 = [type][link_id LE(2)][payload...]（RF_Tx 自 &slot[1] 起发 len+3 字节）：
     *   [0] 只存线上长度（不发送），[1]=type、[2..3]=link_id、[4..]=payload。
     *   接收侧 rf24_deliver() 按 s_rx_mail[0]=type、[1..2]=link_id、[3..]=payload
     *   解析，故此处**必须**把 type 放进 [1]；漏写会导致对端把 link_id 当 type，
     *   所有帧（含配对 PAIR_REQ/RESP）被直接丢弃。 */
    s_tx_q[slot][0] = (uint8_t)(len + 3u);
    s_tx_q[slot][1] = type;
    s_tx_q[slot][2] = (uint8_t)(lid & 0xFFu);
    s_tx_q[slot][3] = (uint8_t)(lid >> 8);
    for (uint8_t i = 0u; i < len; i++) {
        s_tx_q[slot][4u + i] = d[i];
    }
    s_tx_q_cnt++;

    tmos_set_event(s_task, RF24_EVT_TX);
    return BRIDGE_OK;
}

/* ------------------------------ RF 状态回调（中断上下文） ------------------------------ */
/* 只搬数据 + 置事件，绝不直接调用 RF_* API（厂商例程硬性要求） */
static void rf24_status_cb(uint8_t sta, uint8_t rsr, uint8_t *rxBuf)
{
    switch (sta) {
    case TX_MODE_TX_FINISH:
        /* auto 模式：数据已发出，硬件**自动转入「等对端 ACK」**（SDK 原文：
         * "if it is in auto mode, it will wait for receiving"）。这是**非终态**，
         * 绝不能在此 RF_Shut —— 否则打断本端 ACK 等待，每包都退化为
         * TX_MODE_RX_TIMEOUT（实测 tx_timeout ≈ tx_ok）。终态是随后的
         * TX_MODE_RX_DATA / TX_MODE_RX_TIMEOUT。 */
        diag_inc(&s_diag_tx_done_ok);
        break;

    case TX_MODE_RX_DATA:
        /* 收到对端 ACK，硬件已回 idle（SDK："receive data(ack) and enter idle state"）。
         * 这是链路层应答，不是数据帧：不投递、不计数，仅推进发送队列。 */
        tmos_set_event(s_task, RF24_EVT_TXDONE);
        break;

    case TX_MODE_RX_TIMEOUT:
        /* 等 ACK 超时（约 200us），硬件已回 idle */
        diag_inc(&s_diag_tx_done_fail);
        tmos_set_event(s_task, RF24_EVT_TXDONE);
        break;

    case TX_MODE_TX_FAIL:
        /* 发送失败，硬件已回 idle */
        diag_inc(&s_diag_tx_done_fail);
        tmos_set_event(s_task, RF24_EVT_TXDONE);
        break;

    case RX_MODE_RX_DATA:
        if (rsr != 0u) {
            /* CRC 错（bit0）/ 类型不匹配（bit1）：硬件不会回 ACK，可安全重新武装 */
            tmos_set_event(s_task, RF24_EVT_RX);
            break;
        }
        if (rxBuf != 0) {
            uint8_t n = rxBuf[1];
            if (n > (uint8_t)sizeof(s_rx_mail)) {
                n = (uint8_t)sizeof(s_rx_mail);
            }
            for (uint8_t i = 0u; i < n; i++) {
                s_rx_mail[i] = rxBuf[i + 2u];   /* rxBuf[0]=rssi, [1]=len, [2..]=data */
            }
            s_rx_len     = n;
            s_rx_pending = true;
            diag_inc(&s_diag_rx_any);
            /* 链路活性在 rf24_deliver() 里**校验通过后**才刷新：
             * 否则陌生设备（未绑定/密钥不符）的射频包也会撑活心跳。 */
            tmos_set_event(s_task, RF24_EVT_DATA);
        }
        /* 校验通过 → 硬件随后会**自动回 ACK**（SDK："it will send data(ack)"），
         * 这是非终态。标记占位，禁止在 ACK 发出前 RF_Shut；等 RX_MODE_TX_FINISH。 */
        s_rx_acking = true;
        break;

    case RX_MODE_TX_FINISH:
        /* 自动回 ACK 成功，硬件已回 idle（SDK："enters idle state"）→ 重新武装 */
        diag_inc(&s_diag_tx_done_ok);
        s_rx_acking = false;
        tmos_set_event(s_task, RF24_EVT_RX);
        break;

    case RX_MODE_TX_FAIL:
        /* 自动回 ACK 失败，硬件已回 idle → 重新武装 */
        diag_inc(&s_diag_tx_done_fail);
        s_rx_acking = false;
        tmos_set_event(s_task, RF24_EVT_RX);
        break;

    default:
        break;
    }
}

/* ------------------------------ TMOS 任务 ------------------------------ */

static uint16_t rf24_process_event(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg = tmos_msg_receive(task_id);
        if (pMsg != 0) {
            tmos_msg_deallocate(pMsg);
        }
        return (uint16_t)(events ^ SYS_EVENT_MSG);
    }
    if (events & RF24_EVT_DATA) {
        rf24_deliver();
        return (uint16_t)(events ^ RF24_EVT_DATA);
    }
    if (events & RF24_EVT_TX) {
        rf24_pump_tx();
        return (uint16_t)(events ^ RF24_EVT_TX);
    }
    if (events & RF24_EVT_TXDONE) {
        /* 在飞 TX 结束：出队，然后续发下一帧或回接收态 */
        if (s_tx_busy) {
            s_tx_busy = false;
            s_tx_q_head = (uint8_t)((s_tx_q_head + 1u) % RF24_TX_QUEUE_DEPTH);
            s_tx_q_cnt--;
        }
        if (s_up) {
            if (s_tx_q_cnt > 0u) {
                rf24_pump_tx();
            } else {
                rf24_arm_rx();
            }
        }
        return (uint16_t)(events ^ RF24_EVT_TXDONE);
    }
    if (events & RF24_EVT_RX) {
        /* 重新武装/续发入口。仅在射频时序已回 idle（无在飞 TX、无在发 ACK）时执行，
         * 否则 RF_Shut 会打断对端 ACK 或本端自动回包。 */
        if (s_up && !s_tx_busy && !s_rx_acking) {
            if (s_tx_q_cnt > 0u) {
                rf24_pump_tx();
            } else {
                rf24_arm_rx();
            }
        }
        return (uint16_t)(events ^ RF24_EVT_RX);
    }
    if (events & RF24_EVT_HB) {
        if (s_up) {
            /* 心跳：维持链路活性 + 断链判定（规格 6.2） */
            if (s_hb_miss < 0xFFu) {
                s_hb_miss++;
            }
            if (s_conn && (s_hb_miss >= RF24_LINK_LOST_CNT)) {
                s_conn    = false;
                s_hb_miss = 0u;
                sdk_rf24_on_link(false);
            }
            (void)rf24_post(BRIDGE_RADIO_PKT_HEARTBEAT, 0, 0u);
            tmos_start_task(s_task, RF24_EVT_HB, (uint16_t)RF24_HB_PERIOD_MS);
        }
        return (uint16_t)(events ^ RF24_EVT_HB);
    }
    if (events & RF24_EVT_PAIR) {
        /* 配对窗口：键盘侧周期重发 PAIR_REQ，直至绑定成功 */
        if (s_up && !bridge_rf24pair_is_bound()) {
            uint8_t req[16];
            uint8_t rlen = 0u;

            bridge_rf24pair_build_req(req, &rlen);
            if (rlen > 0u) {
                (void)rf24_post(BRIDGE_RADIO_PKT_PAIR, req, rlen);
            }
            tmos_start_task(s_task, RF24_EVT_PAIR, (uint16_t)BRIDGE_RF24_PAIR_SEND_MS);
        }
        return (uint16_t)(events ^ RF24_EVT_PAIR);
    }
    return 0u;
}

/* ------------------------------ 对外接口 ------------------------------ */

int sdk_rf24_init(void)
{
    rfConfig_t cfg;

    if (s_up) {
        return BRIDGE_OK;       /* 幂等 */
    }

    /* RF_RoleInit 建立 PHY/BB 角色。**不能只做一次**：深睡（CH58X_LowPower）后
     * 射频角色状态丢失——实测“深睡唤醒 → 2.4G 重新配对”必失败，直至 MCU 复位
     * 重建角色（真机定位）。故每次 init（含唤醒后的模式事务）都重建。 */
    if (RF_RoleInit() != SUCCESS) {
        return BRIDGE_ERR_IO;
    }
    s_role_inited = true;

    /* 发射功率：厂商 2.4G PER 例程（Direct_Test_Mode/APP/RF_dtm.c）用
     * LL_SetTxPowerLevel() 选档，芯片默认 0dBm。实测 VIA 往返丢包以「同频干扰
     * 瞬时爆发（突发簇）」为主（见 SKILL 基线 8.68%），提高到 +6dBm 提升信噪比，
     * 使被干扰时更可能解码成功、减少 ACK 超时导致的 tx_done_fail 丢包。
     * 需在 BLE 栈初始化之后调用（LL_* 属协议栈 API），本函数在模式事务中执行时
     * 已满足。失败仅忽略（保持默认档）。 */
    (void)LL_SetTxPowerLevel(LL_TX_POWEER_6_DBM);

    /* TMOS 任务**只注册一次**：本函数在每次模式事务（SLEEP / BLE↔2G4 往返）都被
     * 调用，若每次都注册会持续占用 TMOS 任务槽，直至 TMOS_ProcessEventRegister
     * 返回 0xFF（无效 ID）。此后所有 tmos_set_event/start_task 静默丢事件，射频
     * 事件泵停摆（真机表现：GET_DIAG 计数完全冻结、up 仍为 1、仅 MCU 复位可恢复）。
     * 厂商 RF_PHY 例程同样只在 RF_Init()（main 中调用一次）注册。 */
    if (!s_task_registered) {
        s_task = TMOS_ProcessEventRegister(rf24_process_event);
        if (s_task == 0xFFu) {
            return BRIDGE_ERR_IO;
        }
        s_task_registered = true;
    }

    tmos_memset(&cfg, 0, sizeof(cfg));
    cfg.accessAddress = RF24_ACCESS_ADDRESS;
    cfg.CRCInit       = RF24_CRC_INIT;
    cfg.Channel       = RF24_CHANNEL;
    cfg.Frequency     = RF24_FREQUENCY;
    cfg.ChannelMap    = RF24_CHANNEL_MAP;
    /* 显式锁定 1M PHY（LLE_MODE_PHY_1M 值=0，与厂商 RF_PHY / PER 例程默认一致）。
     * 2M PHY 速率翻倍但接收灵敏度低、更易丢包，本项目不采用。 */
    cfg.LLEMode       = LLE_MODE_AUTO | RF24_NONHOP_MODE | LLE_MODE_PHY_1M;
    cfg.rfStatusCB    = rf24_status_cb;
    cfg.HopPeriod     = RF24_HOP_PERIOD;
    cfg.HopIndex      = RF24_HOP_INDEX;
    cfg.HeartPeriod   = RF24_HEART_PERIOD;
    cfg.RxMaxlen      = RF24_RX_MAXLEN;
    cfg.TxMaxlen      = RF24_TX_MAXLEN;

    if (RF_Config(&cfg) != SUCCESS) {
        return BRIDGE_ERR_IO;
    }

    s_up         = true;
    s_conn       = false;
    s_hb_miss    = 0u;
    rf24_tx_reset();
    s_rx_pending = false;

    /* 进入接收态并启动心跳 */
    tmos_set_event(s_task, RF24_EVT_RX);
    tmos_start_task(s_task, RF24_EVT_HB, (uint16_t)RF24_HB_PERIOD_MS);
    return BRIDGE_OK;
}

int sdk_rf24_deinit(void)
{
    if (!s_up) {
        return BRIDGE_OK;
    }
    /* 先停「配对重发」任务：配对窗口用 tmos_start_task 自续期、并用
     * tmos_set_event 立即触发首包，两者都须清，否则 deinit→init 之间
     * 悬挂的 RF24_EVT_PAIR 会在射频重新 up 后继续周期重发 PAIR_REQ
     * （真机表现为退出配对窗口后仍在配对）。 */
    (void)tmos_stop_task(s_task, RF24_EVT_PAIR);
    (void)tmos_clear_event(s_task, RF24_EVT_PAIR);
    (void)RF_Shut();
    s_up         = false;
    s_conn       = false;
    rf24_tx_reset();
    s_rx_pending = false;
    return BRIDGE_OK;
}

int sdk_rf24_send(const uint8_t *d, uint8_t len)
{
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    return rf24_post(BRIDGE_RADIO_PKT_HID, d, len);
}

int sdk_rf24_send_tunnel(const uint8_t *d, uint8_t len)
{
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    return rf24_post(BRIDGE_RADIO_PKT_TUNNEL, d, len);
}

int sdk_rf24_send_cfg(const uint8_t *d, uint8_t len)
{
    /* 键盘侧配置中继：载荷 = [CMD][payload...]，Dongle 侧按同一类型前缀识别并本地生效 */
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    return rf24_post(BRIDGE_RADIO_PKT_CFG, d, len);
}

int sdk_rf24_send_pair(const uint8_t *d, uint8_t len)
{
    /* 配对应答帧（Dongle → 键盘）：载荷 = [sub][kbd_id 4][rand 4][key 4] */
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    return rf24_post(BRIDGE_RADIO_PKT_PAIR, d, len);
}

int sdk_rf24_send_host(uint8_t state)
{
    /* 主机电源状态（Dongle → 键盘）：载荷 = [state]，state 取 BRIDGE_HOST_* */
    if (state != BRIDGE_HOST_AWAKE && state != BRIDGE_HOST_SUSPEND) {
        return BRIDGE_ERR_PARAM;
    }
    return rf24_post(BRIDGE_RADIO_PKT_HOST, &state, 1u);
}

bool sdk_rf24_is_connected(void)
{
    return s_up && s_conn;
}

int sdk_rf24_pair_start(void)
{
    if (!s_up) {
        return BRIDGE_ERR_STATE;
    }
    /* 进入配对窗口：清链路、生成新随机数。
     * 键盘侧由 RF24_EVT_PAIR 周期发 PAIR_REQ；Dongle 侧只监听（配对键按下）。
     * 绑定成功（core 配对状态机）后 rf24_deliver() 置 s_conn 建链。 */
    s_conn    = false;
    s_hb_miss = 0u;
    bridge_rf24pair_begin_pairing();
    tmos_set_event(s_task, RF24_EVT_RX);
#if !BRIDGE_BOARD_IS_DONGLE
    tmos_set_event(s_task, RF24_EVT_PAIR);
#endif
    return BRIDGE_OK;
}

int sdk_rf24_pair_stop(void)
{
    if (!s_up) {
        return BRIDGE_ERR_STATE;
    }
    /* 关闭配对窗口：既清 tmos_start_task 排定的重发，也清 tmos_set_event
     * 已置位、尚未处理的立即事件，二者缺一都会让配对循环继续。 */
    (void)tmos_stop_task(s_task, RF24_EVT_PAIR);
    (void)tmos_clear_event(s_task, RF24_EVT_PAIR);
    return BRIDGE_OK;
}

int sdk_rf24_unpair(void)
{
    s_conn    = false;
    s_hb_miss = 0u;
    bridge_rf24pair_forget();       /* 清除掉电记忆的绑定密钥 */
    return s_up ? BRIDGE_OK : BRIDGE_ERR_STATE;
}

int sdk_rf24_set_report_period_ms(uint16_t ms)
{
    if (ms == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    s_report_period_ms = ms;
    return BRIDGE_OK;
}

uint8_t sdk_rf24_protocol_ver(void)
{
    return 0x02u;      /* 厂商 2.4G 链路版本（core 的 EVT_STATE 用） */
}

/* 建链取证快照（24B，布局见 bridge_ch58x_sdk.h）：
 * 单字节状态在前，其后 8 个 LE16 计数；无副作用，可在任务上下文随时调用。 */
int sdk_rf24_diag(uint8_t *out, uint8_t *out_len)
{
    uint16_t c[8];

    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }

    /* c[0..7] = tx_submit_ok/tx_submit_fail/tx_done_ok/tx_done_fail/
     *           tx_drop/rx_any/rx_pair/rx_data_bad（饱和 0xFFFF） */
    c[0] = s_diag_tx_submit_ok;
    c[1] = s_diag_tx_submit_fail;
    c[2] = s_diag_tx_done_ok;
    c[3] = s_diag_tx_done_fail;
    c[4] = (s_tx_drop > 0xFFFFu) ? 0xFFFFu : (uint16_t)s_tx_drop;
    c[5] = s_diag_rx_any;
    c[6] = s_diag_rx_pair;
    c[7] = s_diag_rx_data_bad;

    out[0] = s_up ? 1u : 0u;
    out[1] = s_conn ? 1u : 0u;
    out[2] = bridge_rf24pair_is_bound() ? 1u : 0u;
    out[3] = s_tx_busy ? 1u : 0u;
    out[4] = s_tx_q_cnt;
    out[5] = s_hb_miss;
    out[6] = (uint8_t)(s_report_period_ms & 0xFFu);
    out[7] = (uint8_t)(s_report_period_ms >> 8);
    for (uint8_t i = 0u; i < 8u; i++) {
        out[8u + i * 2u] = (uint8_t)(c[i] & 0xFFu);
        out[9u + i * 2u] = (uint8_t)(c[i] >> 8);
    }
    *out_len = 24u;
    return BRIDGE_OK;
}

/* ===========================================================================
 * 跳频（RF_PHY_Hop）—— 结论：不接入，保持固定信道（见 bridge_config.h 同名开关注释）
 *   例程的 RF_FrequencyHoppingTx(resendCount) / RF_FrequencyHoppingRx(timeoutMS)
 *   是非对称单向骨架：TX 端每周期遍历 ChannelMap 发 HOP_TX PDU，RX 端每周期
 *   只监听单频点；两个 API 都不带应用载荷参数，故不能承载本桥的双向 HID/透传。
 *   WCH 官方亦确认：一跳多的绑定/跳频/透传需自行实现算法，跳频通道由上一包
 *   数据携带、通讯失败则暂缓跳频。
 *   因此本层用固定信道（RF24_CHANNEL）+ LLE_MODE_AUTO 的自动 ACK/重传，
 *   保证链路可用；若将来要实现同步跳频，只需改 rf24_arm_rx()/rf24_pump_tx()
 *   与 rf24_status_cb()（加入带通道号的自定义帧与失败暂缓逻辑），
 *   其余分层（类型前缀 / 翻译 / core 语义）无需改动。
 * =========================================================================== */
