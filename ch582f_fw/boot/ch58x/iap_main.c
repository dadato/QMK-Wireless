/* 常驻 BOOT（0x0，8K）—— 键盘侧：移植自 WCH EVT/EXAM/BLE/BackupUpgrade_IAP
 *
 * 启动链：复位 → 0x0 本程序（每次开机都会经过）：
 *   flag == IAP(0x03)：按标志记录的长度把 Stage(0x38000) 搬运到 APP_A(0x2000)
 *                       → 标志置 A → 擦除 Stage → 跳 App；
 *   App 无效（空片/坏镜像）：驻留 **UART 收包模式**（见下）；
 *   其余标志：直接跳 App（正常开机，零开销）。
 *
 * UART 收包模式（本次新增，对应「APP 为空则就在 BOOT」）：
 *   App 有效时的 OTA 由 App 自己经 UART 收 CMD_FW_BEGIN/DATA/END 写 Stage，
 *   复位后由本 BOOT 搬运；但 App 无效时空片无处可收，故本 BOOT 自带一份最小
 *   UART OTA 接收（轮询式，无中断、无 BLE/TMOS 依赖），发送方协议与 App 侧
 *   **逐字节一致**（同一套桥帧格式与 CMD_FW_* 命令/应答），可复用同一上位机。
 *
 * 与旧 WCH BackupUpgrade 三镜像（JumpIAP@0x0 + ImageA@0x1000 + ImageB@0x37000 +
 * ImageIAP@0x6D000）相比：BOOT 常驻 0x0 一条 hex 即可，工厂只需烧合并镜像。
 *
 * 共享原语（标志读写 / CRC / 擦除 / 搬运 / app_valid）在 iap_common.c，
 * 与 Dongle 的 USB BOOT（iap_usb_main.c）共用同一份实现，避免两份漂移。
 *
 * 注意：本文件编译为独立镜像（make images），不参与 keyboard/dongle 主构建。
 */
#include <string.h>

#include "iap_common.h"
#include "bridge_ota_layout.h"
#include "bridge_types.h"      /* CMD_FW_* / BRIDGE_FLAG_* / BRIDGE_ECODE_* */
#include "CH58x_common.h"

/* ============================ 帧常量 ============================
 * 与 core/bridge_proto.h 一致（BOOT 不链接 core，故本地定义）：
 *   AA 55 | LEN | CMD | SEQ | FLAGS | PAYLOAD(LEN) | CRC16(LE)
 * CRC-16/CCITT-FALSE（init 0xFFFF / poly 0x1021）覆盖 LEN..PAYLOAD。 */
#define IAP_SOF0           0xAAu
#define IAP_SOF1           0x55u
#define IAP_HDR_LEN        4u      /* LEN + CMD + SEQ + FLAGS */
#define IAP_CRC_LEN        2u
#define IAP_FRAME_OVERHEAD (2u + IAP_HDR_LEN + IAP_CRC_LEN)
#define IAP_MAX_PAYLOAD    64u
#define IAP_FW_CHUNK_MAX   60u     /* 与 BRIDGE_FW_CHUNK_MAX 一致 */

#define IAP_UART_BAUD      921600u /* 与桥数据面一致（PA8=RXD1 / PA9=TXD1，默认映射） */

/* BOOT 阶段的模式值（对应 core/bridge_mode.h MODE_OTA=5；BOOT 不链接 core 故本地取值） */
#define IAP_OTA_MODE       5u

/* 空闲超时（进入 UART 收包模式后长时间无任何帧）：
 * 50us 轮询 × OTA_IDLE_TICKS 即约 OTA_IDLE_TICKS×50us。取 60s 兜底：
 * 覆盖两类场景——① 升级中断（关页/拔线）；② 上次 OTA 遗留 flag=UART_IAP 但主机
 * 未再来（拔线/关页）。App 有效 → 写 flag=A 复位回 App（ImageA 未动，回原固件）；
 * App 无效（空片，工厂只烧 BOOT）→ 留在收包模式继续等主机，不做无谓复位。 */
#define IAP_OTA_IDLE_TICKS  1200000u   /* 60s @50us */

/* ============================ OTA 接收状态 ============================ */

static bool     s_recv;
static bool     s_io_err;
static uint32_t s_total;
static uint32_t s_written;
static uint32_t s_erased;
static uint16_t s_crc_exp;
static uint16_t s_crc_run;

/* FLASH_ROM_WRITE 要求 RAM 且 4 字节对齐的中转缓冲（同 iap_common.c 教训） */
static __attribute__((aligned(4))) uint8_t s_wbuf[IAP_FW_CHUNK_MAX];

/* ============================ RX 状态机 ============================ */

typedef enum {
    RX_SOF0 = 0,
    RX_SOF1,
    RX_HDR,
    RX_PAYLOAD,
    RX_CRC
} rx_st_t;

typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t flags;
    uint8_t len;
    uint8_t payload[IAP_MAX_PAYLOAD];
} iap_frame_t;

static rx_st_t  s_st;
static uint8_t  s_hdr[IAP_HDR_LEN];
static uint8_t  s_hdr_idx;
static uint8_t  s_pay[IAP_MAX_PAYLOAD];
static uint8_t  s_pay_idx;
static uint8_t  s_crc_bytes[IAP_CRC_LEN];
static uint8_t  s_crc_idx;
static uint16_t s_crc_calc;

/* ============================ UART 低层（轮询） ============================ */

static void iap_uart_init(void)
{
    /* PA9 = TXD1（先置高避免起始位毛刺），PA8 = RXD1。
     * PA8/PA9 是 UART1 的**默认**映射脚，无需设置 R16_PIN_ALTERNATE；
     * 切勿误置 RB_PIN_UART1（会切到 PB12/PB13，本板为 MODE_SEL）。 */
    GPIOA_SetBits(GPIO_Pin_9);
    GPIOA_ModeCfg(GPIO_Pin_8, GPIO_ModeIN_PU);
    GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeOut_PP_5mA);

    UART1_DefInit();                    /* FIFO 开、8N1 */
    UART1_BaudRateCfg(IAP_UART_BAUD);

    /* 纯轮询：不使能任何 UART1 中断（本 BOOT 不链 bridge_ch58x_sdk.c 的 ISR）。 */
}

static void iap_uart_tx(const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        while (R8_UART1_TFC == UART_FIFO_SIZE) {
            /* 等 TX FIFO 空位 */
        }
        R8_UART1_THR = d[i];
    }
}

/* 等 TX FIFO 排空并让最后一个字节移出移位寄存器（@921600 单字节 ≈10.9us）。
 * 用于复位/跳转前确保应答帧真正上线路，避免被截断。 */
static void iap_uart_flush(void)
{
    while (R8_UART1_TFC != 0u) {
    }
    DelayUs(300u);
}

/* ============================ 应答发送 ============================ */

static void iap_frame_send(uint8_t cmd, uint8_t seq, uint8_t flags,
                           const uint8_t *payload, uint8_t len)
{
    uint8_t  raw[IAP_FRAME_OVERHEAD + IAP_MAX_PAYLOAD];
    uint16_t i = 0u;
    uint16_t crc;

    raw[i++] = IAP_SOF0;
    raw[i++] = IAP_SOF1;
    raw[i++] = len;
    raw[i++] = cmd;
    raw[i++] = seq;
    raw[i++] = flags;
    if (len > 0u && payload != 0) {
        memcpy(&raw[i], payload, len);
        i += len;
    }
    crc = iap_crc16_update(0xFFFFu, &raw[2], (uint32_t)(IAP_HDR_LEN + len));
    raw[i++] = (uint8_t)(crc & 0xFFu);
    raw[i++] = (uint8_t)(crc >> 8);

    iap_uart_tx(raw, i);
}

static void iap_reply_ack(uint8_t seq, uint8_t cmd)
{
    iap_frame_send(cmd, seq, BRIDGE_FLAG_ACK, 0, 0u);
}

static void iap_reply_data(uint8_t seq, uint8_t cmd, const uint8_t *p, uint8_t n)
{
    iap_frame_send(cmd, seq, BRIDGE_FLAG_ACK, p, n);
}

static void iap_reply_err(uint8_t seq, uint8_t cmd, uint8_t ecode)
{
    iap_frame_send(cmd, seq, BRIDGE_FLAG_ERR, &ecode, 1u);
}

/* ============================ Stage 顺序写入 ============================ */

static uint32_t iap_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============================ OTA 命令处理 ============================
 * 严格镜像 App 侧 core/bridge_ota.c + bridge_proto.c handle_fw_* 的校验语义，
 * 使同一上位机对 App / BOOT 两条路径通用。 */

static void iap_ota_begin(const uint8_t *p, uint8_t len, uint8_t seq)
{
    uint32_t total;
    uint16_t crc;
    uint8_t  cm = IAP_FW_CHUNK_MAX;

    if (len != 6u) {
        iap_reply_err(seq, CMD_FW_BEGIN, BRIDGE_ECODE_PARAM);
        return;
    }
    total = iap_rd_u32(p);
    crc   = (uint16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
    /* 长度：非 0、不超暂存区、4 对齐（与 App 完全一致） */
    if (total == 0u || total > BRIDGE_OTA_STAGE_MAX || (total & 3u) != 0u) {
        iap_reply_err(seq, CMD_FW_BEGIN, BRIDGE_ECODE_PARAM);
        return;
    }

    s_total    = total;
    s_written  = 0u;
    s_erased   = 0u;
    s_crc_run  = 0xFFFFu;
    s_crc_exp  = crc;
    s_recv     = true;
    s_io_err   = false;

    iap_reply_data(seq, CMD_FW_BEGIN, &cm, 1u);   /* 应答 [chunk_max u8] */
}

static void iap_ota_data(const uint8_t *p, uint8_t len, uint8_t seq)
{
    uint32_t off;
    uint8_t  dlen;

    if (!s_recv) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_STATE);
        return;
    }
    if (s_io_err) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_STATE);   /* 之前落盘失败：不再继续 */
        return;
    }
    if (len < 5u) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_PARAM);
        return;
    }
    off  = iap_rd_u32(p);
    dlen = (uint8_t)(len - 4u);
    if (dlen == 0u || dlen > IAP_FW_CHUNK_MAX || (dlen & 3u) != 0u || (off & 3u) != 0u) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_PARAM);
        return;
    }
    if (off + (uint32_t)dlen <= s_written) {
        iap_reply_ack(seq, CMD_FW_DATA);        /* 重复帧（ACK 丢失重传）：幂等确认 */
        return;
    }
    if (off != s_written) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_PARAM);   /* 乱序一律拒绝 */
        return;
    }
    if ((uint32_t)dlen > (s_total - s_written)) {
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_PARAM);   /* 超出声明总长 */
        return;
    }

    memcpy(s_wbuf, &p[4], dlen);
    if (iap_stage_write(&s_erased, s_wbuf, off, dlen) != 0u) {
        s_io_err = true;
        iap_reply_err(seq, CMD_FW_DATA, BRIDGE_ECODE_STATE);
        return;
    }
    s_crc_run  = iap_crc16_update(s_crc_run, &p[4], dlen);
    s_written += dlen;
    iap_reply_ack(seq, CMD_FW_DATA);
}

static void iap_ota_end(uint8_t seq, uint8_t len)
{
    if (!s_recv) {
        iap_reply_err(seq, CMD_FW_END, BRIDGE_ECODE_STATE);
        return;
    }
    if (len != 0u) {
        iap_reply_err(seq, CMD_FW_END, BRIDGE_ECODE_PARAM);   /* 与 App 一致：FW_END 无载荷 */
        return;
    }
    if (s_io_err) {
        iap_reply_err(seq, CMD_FW_END, BRIDGE_ECODE_STATE);
        return;
    }
    if (s_written != s_total) {
        iap_reply_err(seq, CMD_FW_END, BRIDGE_ECODE_STATE);    /* 数据不完整 */
        return;
    }
    if (s_crc_run != s_crc_exp) {
        iap_reply_err(seq, CMD_FW_END, BRIDGE_ECODE_CRC);      /* 留在收包模式，允许整包重传 */
        return;
    }

    /* 先应答并等其真正发出，再进入不可中断的搬运/复位 */
    iap_reply_ack(seq, CMD_FW_END);
    iap_uart_flush();

    /* 先落「暂存就绪」标志：搬运过程中掉电，下次开机走 flag==IAP 的搬运恢复路径 */
    iap_flag_write(BRIDGE_OTA_FLAG_IAP, s_total, s_crc_run);

    if (iap_copy_stage_to_app()) {
        SYS_ResetExecute();     /* 搬运成功：复位跳新 App（不复返） */
    }
    /* 搬运失败（暂存 CRC 复验不符等）：保持驻留，允许重新下发整包。
     * 标志保留 IAP，下次开机仍会自动重试搬运，无变砖风险。 */
    s_recv = false;
}

static void iap_ota_abort(uint8_t seq)
{
    iap_reply_ack(seq, CMD_FW_ABORT);
    s_recv = false;
    /* 放弃升级（用户取消/上位机中止）：写 flag=A 复位回原 App。
     * AB 双区安全：APP_A 自始至终未被触碰，取消即可继续跑原固件。
     * （旧注释“App 无效才驻留”，仅空片场景适用——但空片收包同样走本分支，
     *   届时 flag=A 复位后 App 仍无效，会再次回到驻留收包模式，语义依旧成立。） */
    iap_flag_enter_reset(BRIDGE_OTA_FLAG_A);   /* 不复返 */
}

/* ============================ 阶段查询（对齐 App handle_get_version/status） ============================
 * 上位机在 App/BOOT 两条路径复用同一套查询命令；BOOT 不链接 core，故本地给出等价应答：
 *   版本固定 {0,1,0}；状态 [mode][connected][profile][battery][sleep_policy]，其中 BOOT 阶段
 *   mode=OTA(5)，射频/电量等字段无意义一律置 0。 */
static void iap_ota_get_version(uint8_t seq)
{
    static const uint8_t ver[3] = { 0u, 1u, 0u };

    iap_reply_data(seq, CMD_GET_VERSION, ver, (uint8_t)sizeof(ver));
}

static void iap_ota_get_status(uint8_t seq)
{
    uint8_t st[5];

    st[0] = IAP_OTA_MODE;   /* 处于 OTA/BOOT 阶段 */
    st[1] = 0u;             /* connected：BOOT 无射频连接 */
    st[2] = 0u;             /* profile */
    st[3] = 0u;             /* battery% */
    st[4] = 0u;             /* sleep_policy */
    iap_reply_data(seq, CMD_GET_STATUS, st, (uint8_t)sizeof(st));
}

static void iap_dispatch(const iap_frame_t *f)
{
    if ((f->flags & BRIDGE_FLAG_ACK) != 0u) {
        return;     /* 应答帧：忽略 */
    }
    switch (f->cmd) {
    case CMD_GET_VERSION: iap_ota_get_version(f->seq);            break;
    case CMD_GET_STATUS:  iap_ota_get_status(f->seq);             break;
    case CMD_FW_BEGIN: iap_ota_begin(f->payload, f->len, f->seq); break;
    case CMD_FW_DATA:  iap_ota_data(f->payload, f->len, f->seq);  break;
    case CMD_FW_END:   iap_ota_end(f->seq, f->len);               break;
    case CMD_FW_ABORT: iap_ota_abort(f->seq);                     break;
    case CMD_PING:     iap_reply_ack(f->seq, EVT_PONG);           break;  /* 便于对端链路探测 */
    default:
        /* 其余命令 BOOT 不支持：回 ERR（对齐 App bridge_proto_dispatch 的 default） */
        iap_reply_err(f->seq, f->cmd, BRIDGE_ECODE_PARAM);
        break;
    }
}

/* ============================ 帧解析（逐字节轮询） ============================ */

static bool iap_uart_get_frame(iap_frame_t *out)
{
    while (R8_UART1_RFC > 0u) {
        uint8_t b = R8_UART1_RBR;

        /* ★ 必须读 LSR 清「线路状态」错误位（溢出/帧错），否则持续收包后
         * 会形成错误位不清的恶性循环（对照 bridge_port_ch58x.c 真机教训）。 */
        (void)R8_UART1_LSR;

        switch (s_st) {
        case RX_SOF0:
            if (b == IAP_SOF0) {
                s_st = RX_SOF1;
            }
            break;

        case RX_SOF1:
            if (b == IAP_SOF1) {
                s_st      = RX_HDR;
                s_hdr_idx = 0u;
            } else if (b != IAP_SOF0) {
                s_st = RX_SOF0;     /* 兼容 AA AA 55 */
            }
            break;

        case RX_HDR:
            s_hdr[s_hdr_idx++] = b;
            if (s_hdr_idx == IAP_HDR_LEN) {
                if (s_hdr[0] > IAP_MAX_PAYLOAD) {
                    s_st = RX_SOF0;             /* 超长：丢弃重同步 */
                    break;
                }
                s_crc_calc = iap_crc16_update(0xFFFFu, s_hdr, IAP_HDR_LEN);
                s_pay_idx  = 0u;
                s_crc_idx  = 0u;
                s_st       = (s_hdr[0] == 0u) ? RX_CRC : RX_PAYLOAD;
            }
            break;

        case RX_PAYLOAD:
            s_pay[s_pay_idx++] = b;
            s_crc_calc = iap_crc16_update(s_crc_calc, &b, 1u);
            if (s_pay_idx == s_hdr[0]) {
                s_st = RX_CRC;
            }
            break;

        case RX_CRC:
            s_crc_bytes[s_crc_idx++] = b;
            if (s_crc_idx == IAP_CRC_LEN) {
                uint16_t rx = (uint16_t)((uint16_t)s_crc_bytes[0] |
                                         ((uint16_t)s_crc_bytes[1] << 8));
                if (rx == s_crc_calc) {
                    out->len   = s_hdr[0];
                    out->cmd   = s_hdr[1];
                    out->seq   = s_hdr[2];
                    out->flags = s_hdr[3];
                    if (out->len > 0u) {
                        memcpy(out->payload, s_pay, out->len);
                    }
                    s_st = RX_SOF0;
                    return true;
                }
                s_st = RX_SOF0;     /* CRC 不符：丢弃重同步 */
            }
            break;

        default:
            s_st = RX_SOF0;
            break;
        }
    }
    return false;
}

/* ============================ 收包主循环（不复返） ============================ */

static void iap_uart_ota_run(void)
{
    static iap_frame_t f;       /* 静态：IAP .stack 仅 512B，勿放栈上 */
    uint32_t idle = 0u;

    iap_uart_init();
    s_st      = RX_SOF0;
    s_hdr_idx = 0u;
    s_pay_idx = 0u;
    s_crc_idx = 0u;
    s_recv    = false;
    s_io_err  = false;

    for (;;) {
        bool got = iap_uart_get_frame(&f);
        if (got) {
            idle = 0u;
            iap_dispatch(&f);
        }

        /* 长时间无任何帧（升级中断 / 上次遗留 UART_IAP 但主机未再来）：
         * App 有效 → 写 flag=A 复位回原 App（APP_A 未动，回旧固件）；
         * App 无效（空片）→ 留在收包模式继续等主机，不做无谓复位。
         * 50us 轮询间隔 > 921600 下单字节 10.9us，但每次轮询都排空整个
         * UART1 FIFO（深度 8），且 OTA 为停等（每帧等 ACK），
         * 帧间/主从往返远大于 FIFO 溢出时间，不会丢字节；同时提供节流。
         * 传输中（s_recv，已收到 FW_BEGIN 尚未 FW_END）不触发：整包（最大 216K，
         * 受停等 ACK 往返节流）可能耗时数十秒，若中途复位会白白丢弃已写入的 Stage。
         * 代价：主机下发 BEGIN 后消失且不再回来时不会自动回退，需重新上电或重发指令。 */
        if (!s_recv && ++idle >= IAP_OTA_IDLE_TICKS) {
            idle = 0u;
            if (iap_app_valid()) {
                iap_flag_enter_reset(BRIDGE_OTA_FLAG_A);   /* 不复返 */
            }
        }
        DelayUs(50u);
    }
}

/* ============================ 入口 ============================ */

int main(void)
{
    uint8_t flag = 0u;

    SetSysClock(CLK_SOURCE_PLL_60MHz);
    iap_flag_read(&flag, 0, 0);

    /* App 请求（flag=UART_IAP）→ 驻留 UART 收包模式，**不复返**。
     * 收满 → 写 IAP 搬运标志 → 复位走下方 IAP 分支搬运；abort/空闲超时 → 回原 App。
     * 仿 Dongle 的 USB_IAP：让 App 升级流程「先跳 BOOT，BOOT 自收包」，绕开 App 侧
     * 无线栈/睡眠产生的 EVT_STATE 心跳噪音。 */
    if (flag == BRIDGE_OTA_FLAG_UART_IAP) {
        iap_uart_ota_run();
    }

    if (flag == BRIDGE_OTA_FLAG_IAP) {
        /* 复验暂存 CRC → 搬运 Stage→APP_A → flag=A → 擦 Stage；失败（记录异常/CRC
         * 不符）则不碰 APP_A，清标志回旧 App（与 Dongle BOOT 同策略），可重新下发。 */
        if (!iap_copy_stage_to_app()) {
            iap_flag_write(BRIDGE_OTA_FLAG_A, 0u, 0u);
        }
    }

    /* 空片 / 坏镜像（工厂只烧 BOOT、App 区尚未写入，或 App 损坏）：驻留 UART 收包模式，
     * 接收 CMD_FW_BEGIN/DATA/END 完成首次刷写——这是「工厂只烧一个 hex」的关键，
     * 也是「APP 为空则就在 BOOT」的落点。收包成功搬运后会复位跳新 App。
     * 循环内不重设时钟、不关调试口，保持与正常 App 相同的 60MHz 运行态，
     * 这样 DM 能稳定同步，OpenOCD 可随时重新烧录，避免"失联→反复救援"。 */
    if (!iap_app_valid()) {
        iap_uart_ota_run();     /* 不复返 */
    }

    iap_jump_app();
    return 0;   /* 不会到达 */
}
