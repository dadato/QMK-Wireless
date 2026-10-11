#include "bridge_proto.h"
#include "bridge_uart.h"
#include "bridge_hid.h"
#include "bridge_mode.h"
#include "bridge_cfg.h"
#include "bridge_power.h"
#include "bridge_test.h"
#include "bridge_port.h"
#include "bridge_ota.h"
#include "bridge_log.h"
#include <string.h>

#define BRIDGE_MOD_PROTO "proto"

#define PENDING_Q_DEPTH  8u

typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t flags;
    uint8_t len;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];
} pending_t;

static pending_t s_q[PENDING_Q_DEPTH];
static uint8_t   s_q_head;
static uint8_t   s_q_tail;
static uint8_t   s_q_count;
static uint16_t  s_q_drop;

/* 需要应答的事件追踪（一次只跟踪一条，用于 EVT_STATE） */
static bool     s_ack_wait;
static uint8_t  s_ack_seq;
static uint32_t s_ack_sent_ms;
static uint8_t  s_ack_retry;
static uint8_t  s_evt_payload[BRIDGE_MAX_PAYLOAD];  /* 最近一条 EVT_STATE 的载荷，供重传 */
static uint8_t  s_evt_len;

void bridge_proto_init(void)
{
    s_q_head = s_q_tail = s_q_count = 0u;
    s_q_drop = 0u;
    s_ack_wait = false;
    s_ack_seq = 0u;
    s_ack_sent_ms = 0u;
    s_ack_retry = 0u;
    s_evt_len = 0u;
}

/* ---------------- 发送（带 GAP/BUSY 排队） ---------------- */

static int proto_tx(uint8_t cmd, uint8_t seq, uint8_t flags,
                    const uint8_t *payload, uint8_t len)
{
    int rc = bridge_uart_send(cmd, seq, flags, payload, len);
    if (rc == BRIDGE_OK) {
        return BRIDGE_OK;
    }
    if (rc != BRIDGE_ERR_GAP && rc != BRIDGE_ERR_BUSY) {
        return rc;
    }

    /* 排队，等下一个 poll 重试 */
    if (s_q_count >= PENDING_Q_DEPTH) {
        /* 控制/事件帧不允许静默丢失：丢最旧一条并计数 */
        s_q_tail = (uint8_t)((s_q_tail + 1u) % PENDING_Q_DEPTH);
        s_q_count--;
        s_q_drop++;
        LOG_W(BRIDGE_MOD_PROTO, "tx queue full, drop oldest (total %u)", s_q_drop);
    }

    pending_t *p = &s_q[s_q_head];
    p->cmd   = cmd;
    p->seq   = seq;
    p->flags = flags;
    p->len   = len;
    if (len > 0u && payload != 0) {
        memcpy(p->payload, payload, len);
    }
    s_q_head = (uint8_t)((s_q_head + 1u) % PENDING_Q_DEPTH);
    s_q_count++;
    return rc;
}

void bridge_proto_poll(void)
{
    while (s_q_count > 0u) {
        pending_t *p = &s_q[s_q_tail];
        int rc = bridge_uart_send(p->cmd, p->seq, p->flags, p->payload, p->len);
        if (rc == BRIDGE_ERR_GAP || rc == BRIDGE_ERR_BUSY) {
            break;   /* 串口仍不可用，下一轮再试 */
        }
        s_q_tail = (uint8_t)((s_q_tail + 1u) % PENDING_Q_DEPTH);
        s_q_count--;
    }

    /* ACK 超时重试（仅对 FLAG_ACK_REQ 的事件） */
    if (s_ack_wait &&
        (uint32_t)(port_millis() - s_ack_sent_ms) >= BRIDGE_ACK_TIMEOUT_MS) {
        if (s_ack_retry < BRIDGE_ACK_MAX_RETRY) {
            s_ack_retry++;
            s_ack_sent_ms = port_millis();
            /* 重传必须与原帧同形：带原载荷、且只置 ACK_REQ。
             * 若误置 ACK 位，对端会按"应答帧"直接丢弃（见本文件 dispatch 开头），
             * 重传等于白发；若载荷为空，对端拿不到 mode/link/profile/battery。 */
            (void)proto_tx(EVT_STATE, s_ack_seq, BRIDGE_FLAG_ACK_REQ,
                           s_evt_payload, s_evt_len);
            LOG_W(BRIDGE_MOD_PROTO, "evt_state no ack, retry %u", s_ack_retry);
        } else {
            s_ack_wait = false;
            LOG_E(BRIDGE_MOD_PROTO, "evt_state ack timeout");
            bridge_evt_error(BRIDGE_ECODE_ACK_TIMEOUT);
        }
    }
}

/* ---------------- 应答 / 事件 ---------------- */

void bridge_proto_reply(uint8_t seq, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    (void)proto_tx(cmd, seq, BRIDGE_FLAG_ACK, payload, len);
}

void bridge_proto_ack(uint8_t seq, uint8_t cmd)
{
    bridge_proto_reply(seq, cmd, 0, 0u);
}

void bridge_proto_error(uint8_t seq, uint8_t cmd, uint8_t code)
{
    /* 错误帧回带原始 CMD，SK32 才能按 (CMD,SEQ) 匹配到自己发出的命令 */
    (void)proto_tx(cmd, seq, BRIDGE_FLAG_ERR, &code, 1u);
}

int bridge_proto_send_event(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    return proto_tx(cmd, 0u, BRIDGE_FLAG_ACK_REQ, payload, len);
}

void bridge_evt_state(void)
{
    uint8_t p[4];
    p[0] = (uint8_t)bridge_mode_get();
    p[1] = bridge_mode_is_connected() ? 1u : 0u;
    p[2] = bridge_cfg_get()->ble_profile;
    p[3] = bridge_power_battery_percent();

    int rc = proto_tx(EVT_STATE, 0u, BRIDGE_FLAG_ACK_REQ, p, (uint8_t)sizeof(p));

    /* 存档本次载荷供重传使用 */
    memcpy(s_evt_payload, p, sizeof(p));
    s_evt_len = (uint8_t)sizeof(p);

    /* proto_tx 在 GAP/BUSY 时已把帧入队并返回该错误码，帧仍会由 bridge_proto_poll 发出；
     * 故这三种结果都算"已提交"，必须武装 ACK 跟踪，否则串口稍忙就会静默丢掉
     * EVT_STATE 的重传与 ACK_TIMEOUT 上报（规格 6.3：未应答重试 3 次后报 EVT_ERROR）。 */
    if (rc == BRIDGE_OK || rc == BRIDGE_ERR_GAP || rc == BRIDGE_ERR_BUSY) {
        s_ack_wait    = true;
        s_ack_seq     = 0u;
        s_ack_sent_ms = port_millis();
        s_ack_retry   = 0u;
    }
}

void bridge_evt_leds(uint8_t led_state)
{
    bridge_hid_set_leds(led_state);
    (void)bridge_proto_send_event(EVT_LEDS, &led_state, 1u);
}

void bridge_evt_battery(uint16_t mv, uint8_t percent)
{
    uint8_t p[3];
    p[0] = (uint8_t)(mv & 0xFFu);
    p[1] = (uint8_t)((mv >> 8) & 0xFFu);
    p[2] = percent;
    (void)bridge_proto_send_event(EVT_BATTERY, p, (uint8_t)sizeof(p));
}

void bridge_evt_pair_result(uint8_t result, uint8_t profile)
{
    uint8_t p[2] = { result, profile };
    (void)bridge_proto_send_event(EVT_PAIR_RESULT, p, 2u);
}

void bridge_evt_error(uint8_t code)
{
    (void)bridge_proto_send_event(EVT_ERROR, &code, 1u);
}

void bridge_evt_pong(uint8_t seq)
{
    (void)proto_tx(EVT_PONG, seq, BRIDGE_FLAG_ACK, 0, 0u);
}

void bridge_evt_tunnel(uint8_t kind, const uint8_t *payload, uint8_t len)
{
    uint8_t cmd;
    uint8_t cap;

    if (payload == 0 || len == 0u) {
        return;
    }
    if (kind == BRIDGE_TUNNEL_BLE) {
        cmd = EVT_TUNNEL_BLE;
        cap = BRIDGE_TUNNEL_BLE_LEN;
    } else {
        cmd = EVT_TUNNEL_USB;
        cap = BRIDGE_TUNNEL_USB_LEN;
    }
    if (len > cap) {
        return;
    }
    (void)bridge_proto_send_event(cmd, payload, len);
}

void bridge_evt_host_power(uint8_t state)
{
    (void)bridge_proto_send_event(EVT_HOST_POWER, &state, 1u);
}

/* ---------------- 分发 ---------------- */

static void handle_set_mode(const bridge_frame_t *f)
{
    uint8_t mode    = (f->len >= 1u) ? f->payload[0] : (uint8_t)MODE_BLE;
    uint8_t persist = (f->len >= 2u) ? f->payload[1] : 1u;

    /* 配对窗口不再经 SET_MODE 进入：一律走 CMD_PAIR_START。
     * 旧上位机若仍传配对模式，明确回 PARAM 错误（而非静默进入配对窗口）。 */
    if (mode == (uint8_t)MODE_BLE_PAIR || mode == (uint8_t)MODE_2G4_PAIR) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }

    int rc = bridge_mode_request(mode, persist != 0u);
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else {
        uint8_t code = (rc == BRIDGE_ERR_VBUS) ? BRIDGE_ECODE_STATE
                                               : BRIDGE_ECODE_PARAM;
        bridge_proto_error(f->seq, f->cmd, code);
    }
}

/* 模式状态查询（CMD_GET_MODE）：应答 [mode][connected][radio]。
 * 与 GET_STATUS 的区别：只回模式三要素，不含档位/电量/睡眠策略，便于高频轮询。 */
static void handle_get_mode(const bridge_frame_t *f)
{
    uint8_t p[3];
    p[0] = (uint8_t)bridge_mode_get();
    p[1] = bridge_mode_is_connected() ? 1u : 0u;
    p[2] = bridge_mode_radio_id();            /* BRIDGE_RADIO_* */
    bridge_proto_reply(f->seq, f->cmd, p, (uint8_t)sizeof(p));
}

/* 无线综合状态查询（CMD_GET_WIRELESS_STATE）：应答 [wl_state]（BRIDGE_WL_*）。
 * 把 [mode][connected][radio] 折叠成单字节，SK32 查表即得，无需组合判断。 */
static void handle_get_wireless_state(const bridge_frame_t *f)
{
    uint8_t s = bridge_mode_wireless_state();
    bridge_proto_reply(f->seq, f->cmd, &s, 1u);
}

/* BLE 多主机槽绑定状态（CMD_GET_BLE_SLOTS）：应答 [s1][s2][s3][s4][last_active]。
 *   s1..s4     = Profile 1..4 是否已存有效绑定（1 = 已绑定 / 0 = 空）
 *   last_active= 最近活跃（当前使用）的 Profile（1..4），0 = 无
 * 仅键盘侧支持；Dongle 无 BLE 主机绑定 → 回 ERR STATE。 */
static void handle_get_ble_slots(const bridge_frame_t *f)
{
    uint8_t p[5];

    if (port_ble_slots(p) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    bridge_proto_reply(f->seq, f->cmd, p, (uint8_t)sizeof(p));
}

/* 心跳周期开关（CMD_SET_HEARTBEAT）：payload [enable u8]。
 * 关闭后停止一切心跳相关功能（不发送 PING、不做失联判定、不回 PONG）。 */
static void handle_set_heartbeat(const bridge_frame_t *f)
{
    if (f->len < 1u || f->payload[0] > 1u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    bridge_uart_heartbeat_enable(f->payload[0] != 0u);
    LOG_I(BRIDGE_MOD_PROTO, "heartbeat %s", (f->payload[0] != 0u) ? "on" : "off");
    bridge_proto_ack(f->seq, f->cmd);
}

static void handle_set_profile(const bridge_frame_t *f)
{
    uint8_t profile = (f->len >= 1u) ? f->payload[0] : 1u;

    int rc = bridge_cfg_set_profile(profile);
    if (rc != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    bridge_hid_release_all();
    /* 切槽门控必须用「当前模式」而非 bridge_mode_radio_id()：后者是转发用谓词，
     * 在未连接(connected=0)时返回 BRIDGE_RADIO_NONE，会让断开态切槽被整体跳过
     * （core 已更新 profile，但射频层从未切槽/换地址，PC 无法回连）。 */
    {
        uint8_t m = bridge_mode_get();
        if (m == MODE_BLE || m == MODE_BLE_PAIR) {
            (void)port_radio_connect_profile(BRIDGE_RADIO_BLE, profile);
        }
    }
    bridge_proto_ack(f->seq, f->cmd);
    bridge_evt_state();
}

/* PAIR_START / UNPAIR 的射频意图：现行模式若是非无线模式（睡眠 / USB / OTA），
 * 取值退回记忆模式（cfg->last_mode，睡眠不落盘故保留最近一次无线模式）。
 * 直接读瞬时 bridge_mode_get() 会在 MODE_SLEEP 下把 2.4G 键盘误判成 BLE：
 * PAIR_START 落到 BLE_PAIR → 2.4G 永不配对；UNPAIR 清错射频 → 2.4G 绑定残留
 * 导致键盘因「已绑定」不再发 PAIR_REQ、配对窗口僵死（真机实测）。 */
static uint8_t proto_radio_intent(void)
{
    uint8_t mode = bridge_mode_get();

    if (mode == MODE_BLE || mode == MODE_BLE_PAIR ||
        mode == MODE_2G4 || mode == MODE_2G4_PAIR) {
        return mode;
    }

    mode = bridge_cfg_get()->last_mode;
    return (mode == (uint8_t)MODE_2G4 || mode == (uint8_t)MODE_2G4_PAIR)
               ? mode
               : (uint8_t)MODE_BLE;
}

static void handle_pair_start(const bridge_frame_t *f)
{
    uint8_t mode = proto_radio_intent();
    uint8_t target = (mode == MODE_2G4 || mode == MODE_2G4_PAIR) ? (uint8_t)MODE_2G4_PAIR
                                                                 : (uint8_t)MODE_BLE_PAIR;

    /* 可选持续时间参数 [duration_s LE16]：0 = 不自动关闭窗口；省略（无载荷）回默认值。
     * 其它长度视为参数错误。 */
    if (f->len == 0u) {
        bridge_mode_set_pair_timeout(BRIDGE_PAIR_TIMEOUT_S);
    } else if (f->len == 2u) {
        bridge_mode_set_pair_timeout((uint16_t)((uint16_t)f->payload[0] |
                                                ((uint16_t)f->payload[1] << 8)));
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }

    int rc = bridge_mode_request(target, true);
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else if (rc == BRIDGE_ERR_VBUS) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PAIR);
    }
}

static void handle_unpair(const bridge_frame_t *f)
{
    uint8_t profile = (f->len >= 1u) ? f->payload[0] : 0xFFu;
    uint8_t radio;
    uint8_t n = 0u;

    /* 按**当前模式**选射频，而不是 bridge_mode_radio_id()：
     * 后者只在“已连接”时返回射频号，配对窗口（2G4_PAIR/BLE_PAIR，未连接）会返回
     * NONE，曾导致 2.4G_PAIR 下的 UNPAIR 误落到 BLE → 2.4G 绑定没清 → 键盘因
     * “已绑定”不再发 PAIR_REQ，配对窗口僵死（真机实测）。
     * 睡眠/USB/OTA 下同样改用记忆模式，避免清错射频（见 proto_radio_intent）。 */
    switch (proto_radio_intent()) {
        case MODE_2G4:
        case MODE_2G4_PAIR:
            radio = BRIDGE_RADIO_2G4;
            break;
        default:
            radio = BRIDGE_RADIO_BLE;
            break;
    }

    if (profile == 0xFFu) {
        /* 全清必须把 0xFF 透传到 port 层：BLE 侧只有 sdk_ble_unpair(0xFF) 才会调
         * GAPBOND_ERASE_ALLBONDS 清库内 LTK 绑定记录；2G4 侧忽略 profile 直接清。
         * 旧实现循环 1..4 只清本工程多主机槽（bridge_mh_erase），库内绑定残留，
         * 导致主机解绑后重连加密失败 → 反复建链/断链（真机实测）。 */
        (void)port_radio_unpair(radio, 0xFFu);
        n = 1u;
    } else if (profile >= 1u && profile <= (uint8_t)BRIDGE_MAX_BLE_PROFILE) {
        (void)port_radio_unpair(radio, profile);
        n++;
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }

    LOG_I(BRIDGE_MOD_PROTO, "unpair profile=0x%02X count=%u", profile, n);
    bridge_proto_ack(f->seq, f->cmd);
    bridge_evt_state();
}

static void handle_get_mac(const bridge_frame_t *f)
{
    uint8_t mac[6];
    memset(mac, 0, sizeof(mac));
    if (port_chip_get_mac(mac) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    bridge_proto_reply(f->seq, f->cmd, mac, 6u);
}

static void handle_set_config(const bridge_frame_t *f)
{
    if (f->len < 1u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    uint8_t key = f->payload[0];
    int rc = bridge_cfg_apply(key, &f->payload[1], (uint8_t)(f->len - 1u));
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
    }
}

static void handle_get_status(const bridge_frame_t *f)
{
    uint8_t p[5];
    p[0] = (uint8_t)bridge_mode_get();
    p[1] = bridge_mode_is_connected() ? 1u : 0u;
    p[2] = bridge_cfg_get()->ble_profile;
    p[3] = bridge_power_battery_percent();
    p[4] = bridge_cfg_get()->sleep_policy;   /* 0=AUTO 1=MANUAL（指令控制） */
    bridge_proto_reply(f->seq, f->cmd, p, (uint8_t)sizeof(p));
}

static void handle_get_version(const bridge_frame_t *f)
{
    uint8_t ver[3];
    ver[0] = 0u;
    ver[1] = 1u;
    ver[2] = 0u;
    bridge_proto_reply(f->seq, f->cmd, ver, (uint8_t)sizeof(ver));
}

/* 诊断快照（排障用）：转发 2.4G 射频层的运行时计数，见 port_radio_diag() */
static void handle_get_diag(const bridge_frame_t *f)
{
    uint8_t p[64];
    uint8_t n = 0u;

    if (port_radio_diag(p, &n) != BRIDGE_OK || n == 0u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    bridge_proto_reply(f->seq, f->cmd, p, n);
}

/* BLE HID 服务注册取证（排障用）：见 port_hid_diag() */
static void handle_hid_diag(const bridge_frame_t *f)
{
    uint8_t p[32];            /* 14B 基础句柄 + 3×uint32 出站 notify 计数 + 6B 断链取证 */
    uint8_t n = 0u;

    if (port_hid_diag(p, &n) != BRIDGE_OK || n == 0u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    bridge_proto_reply(f->seq, f->cmd, p, n);
}

static void handle_test_mode(const bridge_frame_t *f)
{
    uint8_t mode    = (f->len >= 1u) ? f->payload[0] : 0u;
    uint8_t channel = (f->len >= 2u) ? f->payload[1] : 0u;
    uint8_t power   = (f->len >= 3u) ? f->payload[2] : 0u;

    int rc = bridge_test_set_mode(mode, channel, power);
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
    }
}

/* ---------------- 借鉴市售模组指令集新增命令（一、基本数据 / 二、控制命令） ---------------- */

/* 透传（0x88 / 0x89）：与 HID 同属低延迟路径，无 ACK、无日志 */
static void handle_tunnel(const bridge_frame_t *f)
{
    uint8_t cap = (f->cmd == CMD_TUNNEL_BLE) ? BRIDGE_TUNNEL_BLE_LEN : BRIDGE_TUNNEL_USB_LEN;

    if (f->len > cap) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    (void)port_tunnel_send((f->cmd == CMD_TUNNEL_BLE) ? BRIDGE_TUNNEL_BLE : BRIDGE_TUNNEL_USB,
                           f->payload, f->len);
}

/* 用户 NV 寻址：单元号 → (NV 槽 id, 槽内偏移)。
 * 市售模组按「地址编号 × 32B」寻址，本工程槽宽 64B，故每槽放 2 个单元。 */
static uint16_t nv_unit_slot(uint8_t addr)
{
    return (uint16_t)(BRIDGE_NV_USER_BASE + (addr / BRIDGE_NV_UNITS_PER_SLOT));
}

static uint8_t nv_unit_off(uint8_t addr)
{
    return (uint8_t)((addr % BRIDGE_NV_UNITS_PER_SLOT) * BRIDGE_NV_UNIT);
}

static void handle_nv_write(const bridge_frame_t *f)
{
    uint8_t  addr;
    uint8_t  blk[BRIDGE_NV_SLOT_SIZE];
    uint16_t id;

    if (f->len != (uint8_t)(1u + BRIDGE_NV_UNIT) || f->payload[0] >= BRIDGE_NV_USER_UNITS) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    addr = f->payload[0];
    id   = nv_unit_slot(addr);
    if (port_nv_read(id, blk, (uint16_t)sizeof(blk)) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    memcpy(&blk[nv_unit_off(addr)], &f->payload[1], BRIDGE_NV_UNIT);
    if (port_nv_write(id, blk, (uint16_t)sizeof(blk)) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    bridge_proto_ack(f->seq, f->cmd);
}

static void handle_nv_read(const bridge_frame_t *f)
{
    uint8_t  addr;
    uint8_t  blk[BRIDGE_NV_SLOT_SIZE];
    uint8_t  out[1u + BRIDGE_NV_UNIT];
    uint16_t id;

    if (f->len < 1u || f->payload[0] >= BRIDGE_NV_USER_UNITS) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    addr  = f->payload[0];
    id    = nv_unit_slot(addr);
    if (port_nv_read(id, blk, (uint16_t)sizeof(blk)) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    out[0] = addr;
    memcpy(&out[1], &blk[nv_unit_off(addr)], BRIDGE_NV_UNIT);
    bridge_proto_reply(f->seq, f->cmd, out, (uint8_t)sizeof(out));
}

/* 下发电量（0x87）：SK32 自算的 0..100，优先于模组本地 ADC */
static void handle_set_battery(const bridge_frame_t *f)
{
    if (f->len < 1u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    bridge_power_set_external(f->payload[0]);
    bridge_proto_ack(f->seq, f->cmd);
    bridge_evt_state();         /* 电量变化立即上报（对齐市售模组语义） */
}

/* 睡眠分级（0x4A/0x4B/0x4C）。
 *   IDLE = 浅睡（保持链路）：模式/射频不变，port 层主循环空闲时自动进行
 *          20ms 有界休眠（bridge_ch58x_idle，链路靠射频中断维持）；
 *   DEEP = 深睡：进 MODE_SLEEP（射频关闭 + 1s 窗口 + 看门狗挂起）。
 * 唤醒（WAKE）只需在深睡时需要显式退出。
 *
 * 策略配合（SET_CONFIG key 0x06）：AUTO 下 port/core 自行判闲休眠，本命令仍可
 * 显式控制；MANUAL 下不自动进睡，只有收到 IDLE/DEEP 才置位睡眠许可、WAKE 清除。
 * 无论何种策略，休眠均由 RXD1 边沿（起始位）唤醒（见 port_sleep_enter）。 */
static void handle_sleep(const bridge_frame_t *f)
{
    uint8_t level = (f->len >= 1u) ? f->payload[0] : BRIDGE_SLEEP_IDLE;

    if (level > BRIDGE_SLEEP_DEEP) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    if (level == BRIDGE_SLEEP_WAKE) {
        bridge_mode_t m = (bridge_mode_t)bridge_mode_restore_target(
            bridge_cfg_get()->last_mode);
        bridge_mode_sleep_permit(false);        /* 指令控制：解除休眠许可（完全清醒） */
        if (m == MODE_SLEEP || m == MODE_OTA) {
            m = MODE_BLE;
        }
        (void)bridge_mode_request((uint8_t)m, false);
    } else if (level == BRIDGE_SLEEP_DEEP) {
        bridge_mode_sleep_permit(true);         /* 指令控制：允许本次深睡 */
        (void)bridge_mode_request((uint8_t)MODE_SLEEP, false);
    } else {
        /* IDLE：置位睡眠许可，port 层随后自动进行有界浅睡（保持链路）。 */
        bridge_mode_sleep_permit(true);
    }
    bridge_proto_ack(f->seq, f->cmd);
}

/* 睡眠时间配置（0x25）：payload [idle_s LE16][deep_s LE16]，单位秒，各 2 字节。
 *   idle_s = AUTO 策略下距最近一次串口接收活动达该秒数后允许浅睡（IDLE），0 = 禁用自动浅睡；
 *   deep_s = AUTO 策略下未连接且空闲达该秒数后进深睡（DEEP），0 = 禁用自动深睡。
 * 落盘保存（bridge_cfg）；仅 AUTO 策略下生效。应答 ACK / ERR PARAM。 */
static void handle_sleep_cfg(const bridge_frame_t *f)
{
    if (f->len != 4u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    uint16_t idle_s = (uint16_t)((uint16_t)f->payload[0] |
                                 ((uint16_t)f->payload[1] << 8));
    uint16_t deep_s = (uint16_t)((uint16_t)f->payload[2] |
                                 ((uint16_t)f->payload[3] << 8));

    int rc = bridge_cfg_set_sleep_times(idle_s, deep_s);
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
    }
}

/* 电量上报配置（0x26）：payload [enable][on_change][period_s]，各 1 字节。
 *   enable    = EVT_BATTERY 总开关，0 = 关 / 1 = 开；
 *   on_change = 电量变化时立即上报，0 = 关 / 1 = 开；
 *   period_s  = 定时上报周期，单位 S，0 = 关闭定时上报。
 * 落盘保存（bridge_cfg）；EVT_BATTERY 仅在 battery_src = LOCAL（本地 ADC）时才会上报。
 * 应答 ACK / ERR PARAM。 */
static void handle_battery_cfg(const bridge_frame_t *f)
{
    if (f->len != 3u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    int rc = bridge_cfg_set_batt_report(f->payload[0], f->payload[1], f->payload[2]);
    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
    }
}

/* 按需采集 ADC（0x4D）：应答 2B 小端毫伏值（与市售模组 0x22 上报同格式） */
static void handle_get_adc(const bridge_frame_t *f)
{
    uint16_t mv = port_bat_read_mv();
    uint8_t  p[2];

    p[0] = (uint8_t)(mv & 0xFFu);
    p[1] = (uint8_t)((mv >> 8) & 0xFFu);
    bridge_proto_reply(f->seq, f->cmd, p, (uint8_t)sizeof(p));
}

/* ---------------- 名称 / USB 标识（CMD_SET_NAME 0x1F / CMD_SET_USB_ID 0x20） ----------------
 * 键盘侧（无 USB）：USB 名称与 VID/PID 本板无法生效，经 2.4G 中继给 Dongle；
 * BLE 广播名在键盘侧本地生效。Dongle 反之（无 BLE）。 */

static int parse_name_req(const uint8_t *p, uint8_t len, uint8_t *target, uint8_t *nlen)
{
    if (len < 2u) {
        return BRIDGE_ERR_PARAM;
    }
    if (p[0] != BRIDGE_NAME_TARGET_BLE && p[0] != BRIDGE_NAME_TARGET_USB) {
        return BRIDGE_ERR_PARAM;
    }
    if ((uint8_t)(len - 1u) > BRIDGE_NAME_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    *target = p[0];
    *nlen   = (uint8_t)(len - 1u);
    return BRIDGE_OK;
}

static void relay_cfg(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t pkt[BRIDGE_MAX_PAYLOAD + 1u];

    if (port_is_dongle() || len > BRIDGE_MAX_PAYLOAD) {
        return;                 /* Dongle 无下游接收方 */
    }
    pkt[0] = cmd;
    if (len > 0u && payload != 0) {
        memcpy(&pkt[1], payload, len);
    }
    (void)port_radio_cfg_send(pkt, (uint8_t)(len + 1u));
}

static void handle_set_name(const bridge_frame_t *f)
{
    uint8_t target;
    uint8_t nlen;

    if (parse_name_req(f->payload, f->len, &target, &nlen) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    if (bridge_cfg_set_name(target, &f->payload[1], nlen) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
        return;
    }
    if (port_set_name(target, &f->payload[1], nlen) == BRIDGE_ERR_UNSUPPORTED) {
        relay_cfg(f->cmd, f->payload, f->len);
    }
    bridge_proto_ack(f->seq, f->cmd);
}

static void handle_set_usb_id(const bridge_frame_t *f)
{
    uint16_t vid;
    uint16_t pid;

    if (f->len != 4u) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    vid = (uint16_t)((uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8));
    pid = (uint16_t)((uint16_t)f->payload[2] | ((uint16_t)f->payload[3] << 8));
    if (bridge_cfg_set_usb_ids(vid, pid) != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        return;
    }
    if (port_set_usb_ids(vid, pid) == BRIDGE_ERR_UNSUPPORTED) {
        relay_cfg(f->cmd, f->payload, f->len);
    }
    bridge_proto_ack(f->seq, f->cmd);
}

int bridge_proto_apply_remote(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t  target;
    uint8_t  nlen;
    uint16_t vid;
    uint16_t pid;

    if (payload == 0) {
        return BRIDGE_ERR_PARAM;
    }
    switch (cmd) {
        case CMD_SET_NAME:
            if (parse_name_req(payload, len, &target, &nlen) != BRIDGE_OK) {
                return BRIDGE_ERR_PARAM;
            }
            if (bridge_cfg_set_name(target, &payload[1], nlen) != BRIDGE_OK) {
                return BRIDGE_ERR_IO;
            }
            return port_set_name(target, &payload[1], nlen);

        case CMD_SET_USB_ID:
            if (len != 4u) {
                return BRIDGE_ERR_PARAM;
            }
            vid = (uint16_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
            pid = (uint16_t)((uint16_t)payload[2] | ((uint16_t)payload[3] << 8));
            if (bridge_cfg_set_usb_ids(vid, pid) != BRIDGE_OK) {
                return BRIDGE_ERR_PARAM;
            }
            return port_set_usb_ids(vid, pid);

        default:
            return BRIDGE_ERR_UNSUPPORTED;
    }
}

/* ---------------- 固件升级（键盘侧 UART OTA，WCH IAP 分区） ---------------- */

static void handle_fw_begin(const bridge_frame_t *f)
{
    int rc = bridge_ota_begin(f->payload, f->len);

    if (rc == BRIDGE_OK) {
        uint8_t cm = (uint8_t)BRIDGE_FW_CHUNK_MAX;

        /* 进 OTA 态：射频关闭（切模式事务内含 HID 释放），写 Flash 期间不转发 */
        (void)bridge_mode_request((uint8_t)MODE_OTA, false);
        bridge_proto_reply(f->seq, f->cmd, &cm, 1u);
    } else {
        bridge_proto_error(f->seq, f->cmd,
                           (rc == BRIDGE_ERR_PARAM) ? BRIDGE_ECODE_PARAM
                                                    : BRIDGE_ECODE_STATE);
    }
}

static void handle_fw_data(const bridge_frame_t *f)
{
    int rc = bridge_ota_data(f->payload, f->len);

    if (rc == BRIDGE_OK) {
        bridge_proto_ack(f->seq, f->cmd);
    } else if (rc == BRIDGE_ERR_PARAM) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
    } else if (rc == BRIDGE_ERR_CRC) {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_CRC);
    } else {
        bridge_proto_error(f->seq, f->cmd, BRIDGE_ECODE_STATE);
    }
}

static void handle_fw_end(const bridge_frame_t *f)
{
    int rc = bridge_ota_end(f->payload, f->len);

    if (rc != BRIDGE_OK) {
        bridge_proto_error(f->seq, f->cmd,
                           (rc == BRIDGE_ERR_CRC)   ? BRIDGE_ECODE_CRC :
                           (rc == BRIDGE_ERR_PARAM) ? BRIDGE_ECODE_PARAM :
                                                      BRIDGE_ECODE_STATE);
        return;
    }

    /* 先应答，并给串口 ~2 帧时间把 ACK 真正发出，再置标志复位（apply 不复返）。
     * 注意必须显式 port_uart_tx_flush()：bridge_proto_poll() 只把帧送进 port 的
     * 软件 TX 环，而环→硬件 FIFO 的排空只发生在主循环，port_delay_ms() 是阻塞
     * 忙等、不会排环，否则 ACK 在复位前可能始终未上线。 */
    bridge_proto_ack(f->seq, f->cmd);
    for (uint8_t i = 0u; i < 4u; i++) {
        bridge_proto_poll();
        port_uart_tx_flush();
        port_delay_ms(6u);
    }
    bridge_ota_apply();
}

static void handle_fw_abort(const bridge_frame_t *f)
{
    bridge_ota_abort();
    bridge_proto_ack(f->seq, f->cmd);
    (void)bridge_mode_request(bridge_mode_default(), false);   /* 恢复默认无线模式 */
}

int bridge_proto_dispatch(const bridge_frame_t *frame)
{
    if (frame == 0) {
        return BRIDGE_ERR_PARAM;
    }

    bridge_uart_on_rx_frame();
    bridge_mode_note_activity();

    /* 应答帧：只用于清 ACK 追踪，不再分发 */
    if ((frame->flags & BRIDGE_FLAG_ACK) != 0u) {
        if (s_ack_wait && frame->seq == s_ack_seq) {
            s_ack_wait  = false;
            s_ack_retry = 0u;
            LOG_D(BRIDGE_MOD_PROTO, "evt_state acked");
        }
        return BRIDGE_OK;
    }

    switch (frame->cmd) {
        case CMD_GET_VERSION:  handle_get_version(frame);      return BRIDGE_OK;
        case CMD_GET_STATUS:   handle_get_status(frame);       return BRIDGE_OK;
        case CMD_SET_MODE:     handle_set_mode(frame);         return BRIDGE_OK;
        case CMD_SET_PROFILE:  handle_set_profile(frame);      return BRIDGE_OK;
        case CMD_PAIR_START:   handle_pair_start(frame);       return BRIDGE_OK;
        case CMD_UNPAIR:       handle_unpair(frame);           return BRIDGE_OK;
        case CMD_SET_CONFIG:   handle_set_config(frame);       return BRIDGE_OK;
        case CMD_GET_MAC:      handle_get_mac(frame);          return BRIDGE_OK;
        case CMD_SET_TEST_MODE: handle_test_mode(frame);       return BRIDGE_OK;
        case CMD_GET_MODE:     handle_get_mode(frame);         return BRIDGE_OK;
        case CMD_GET_WIRELESS_STATE: handle_get_wireless_state(frame); return BRIDGE_OK;
        case CMD_GET_BLE_SLOTS: handle_get_ble_slots(frame);    return BRIDGE_OK;
        case CMD_SET_HEARTBEAT: handle_set_heartbeat(frame);   return BRIDGE_OK;

        /* 借鉴市售模组指令集新增（一/二节） */
        case CMD_TUNNEL_USB:
        case CMD_TUNNEL_BLE:   handle_tunnel(frame);           return BRIDGE_OK;
        case CMD_NV_WRITE:     handle_nv_write(frame);         return BRIDGE_OK;
        case CMD_NV_READ:      handle_nv_read(frame);          return BRIDGE_OK;
        case CMD_SET_BATTERY:  handle_set_battery(frame);      return BRIDGE_OK;
        case CMD_SLEEP:        handle_sleep(frame);            return BRIDGE_OK;
        case CMD_SLEEP_CFG:    handle_sleep_cfg(frame);        return BRIDGE_OK;
        case CMD_BATTERY_CFG:  handle_battery_cfg(frame);      return BRIDGE_OK;
        case CMD_GET_ADC:      handle_get_adc(frame);          return BRIDGE_OK;
        case CMD_SET_NAME:     handle_set_name(frame);         return BRIDGE_OK;
        case CMD_SET_USB_ID:   handle_set_usb_id(frame);       return BRIDGE_OK;

        /* 键盘侧 UART OTA（WCH IAP 分区） */
        case CMD_FW_BEGIN:     handle_fw_begin(frame);         return BRIDGE_OK;
        case CMD_FW_DATA:      handle_fw_data(frame);          return BRIDGE_OK;
        case CMD_FW_END:       handle_fw_end(frame);           return BRIDGE_OK;
        case CMD_FW_ABORT:     handle_fw_abort(frame);         return BRIDGE_OK;

        /* 诊断快照（排障用） */
        case CMD_GET_DIAG:     handle_get_diag(frame);         return BRIDGE_OK;
        case CMD_HID_DIAG:     handle_hid_diag(frame);         return BRIDGE_OK;

        case CMD_RESET:
            LOG_W(BRIDGE_MOD_PROTO, "reset requested");
            port_reset();
            return BRIDGE_OK;

        case CMD_ENTER_BOOT:
            LOG_W(BRIDGE_MOD_PROTO, "bootloader requested");
            /* 先回 ACK 并确保其真正上线，再复位进 BOOT：主机收到应答即停止密集
             * 连发，避免复位后仍在途的 ENTER_BOOT 落到 BOOT 固件（只认
             * GET_VERSION、GET_STATUS、FW_ 系列与 PING）而产生 ERR PARAM 风暴。 */
            bridge_proto_ack(frame->seq, frame->cmd);
            for (uint8_t i = 0u; i < 4u; i++) {
                bridge_proto_poll();
                port_uart_tx_flush();
                port_delay_ms(6u);
            }
            bridge_hid_release_all();
            port_enter_bootloader();
            return BRIDGE_OK;

        case CMD_HID_KEYBOARD:
        case CMD_HID_NKRO:
        case CMD_HID_CONSUMER:
        case CMD_HID_MOUSE:
        case CMD_HID_SYSTEM:
        case CMD_HID_RELEASE:
            /* 高频转发路径：无 ACK、无日志 */
            return bridge_hid_send(frame->cmd, frame->payload, frame->len);

        case CMD_PING:
            if (!bridge_uart_heartbeat_enabled()) {
                return BRIDGE_OK;   /* 心跳已关闭：不回 PONG（活性已由 dispatch 入口刷新） */
            }
            /* 对端 PING 的活性已在 dispatch 入口由 bridge_uart_on_rx_frame() 刷新，
             * 这里只需回 PONG 应答。 */
            bridge_evt_pong(frame->seq);
            return BRIDGE_OK;

        default:
            LOG_W(BRIDGE_MOD_PROTO, "unsupported cmd 0x%02X", frame->cmd);
            bridge_proto_error(frame->seq, frame->cmd, BRIDGE_ECODE_PARAM);
            return BRIDGE_ERR_UNSUPPORTED;
    }
}
