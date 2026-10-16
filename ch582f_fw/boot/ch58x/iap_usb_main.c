/* Dongle 侧常驻 BOOT（0x0，8K）—— 自带 USB HID BootLoader + 暂存搬运
 *
 * 与键盘 BOOT（iap_main.c）的区别（SPEC §3.2）：
 *   键盘：App 收包写入 Stage → 复位 → BOOT 只做 Stage→APP_A 搬运；
 *   Dongle：App 仅触发（写 flag=USB_IAP 后复位）→ **本程序自带 USB HID 设备栈收包**，
 *           写 Stage → CRC 复验 → 搬运 Stage→APP_A → flag=A → 擦 Stage → 复位跳新 App。
 *
 * 分区 / 标志记录与键盘侧完全一致（bridge_ota_layout.h），共享搬运原语在 iap_common.c。
 *
 * 启动：复位 → 0x0 本程序：
 *   flag == USB_IAP(0x04)：进入 USB HID 接收模式（轮询式 USB，同 WCH EVM/USB_IAP）；
 *   flag == IAP(0x03)    ：暂存就绪，直接搬运（OTA 搬运中掉电的重启恢复路径）；
 *   App 无效             ：进入 USB HID 接收模式（工厂只烧 BOOT 的空片也能 OTA）；
 *   其余                 ：直接跳 App。
 *
 * USB 设备形态（决策 #3/#4）：设备/配置/报表描述符与 App **逐字节一致**（复合 3 接口，
 * Raw HID 在 IF2 / usage page 0xFF60 / usage 0x61 / Report ID 0 / 32B IN+OUT）——
 * 上位机据此静默重连（用户无感），仅由 GET_INFO.mode 区分阶段。
 *
 * 注意：本文件编译为独立镜像（make images），不参与 keyboard/dongle 主构建；
 * 也**不**链接 CH58x_usbdev.c（自带端点缓冲与轮询处理，见文件内说明）。
 */
#include <string.h>
#include "CH58x_common.h"
#include "iap_common.h"
#include "bridge_ota_layout.h"
#include "bridge_config.h"    /* BRIDGE_FW_REV / BRIDGE_USB_VID / BRIDGE_USB_PID */
#include "bridge_types.h"     /* BRIDGE_ERR_* */

/* ============================ OTA 子命令（SPEC §5.4） ============================ */
#define OTA_MAGIC0        0x4Fu   /* 'O' */
#define OTA_MAGIC1        0x54u   /* 'T' */
#define OTA_MAGIC2        0x41u   /* 'A' */

#define OTA_SUB_ENTER     0x01u
#define OTA_SUB_FW_BEGIN  0x02u
#define OTA_SUB_FW_DATA   0x03u
#define OTA_SUB_FW_END    0x04u
#define OTA_SUB_FW_ABORT  0x05u
#define OTA_SUB_GET_INFO  0x06u

#define OTA_REPORT_LEN    32u    /* EP3 单包（WebHID reportId=0） */
#define OTA_ARGS_MAX      28u    /* 32 - 3(magic) - 1(sub) */
#define OTA_CHUNK_MAX     24u    /* FW_DATA 有效数据上限（32 - magic/sub - offset(4)） */
#define OTA_MODE_IAP      1u

/* 空闲回退：USB 进入接收模式后，若长时间无任何报文则写 flag=A 复位回 App，
 * 避免主机侧异常（关页/拔线）导致设备永久停在 IAP。任何 USB 传输都会清零计数。
 * 约 60s（每轮 ≈20us，与 WCH EVM/USB_IAP 同量级；无需精确）。 */
#define OTA_IDLE_TICKS    3000000u

/* ============================ USB 端点缓冲 ============================
 * 不复用 CH58x_usbdev.c 的 pEPn_RAM_Addr 机制（那会引入对驱动 .c 的链接依赖），
 * 直接自带缓冲并用 R16_UEPn_DMA 指定，与 WCH EVM/IAP/USB_IAP 同款。 */
__attribute__((aligned(4))) static uint8_t iap_ep0_buf[64 + 64 + 64];
__attribute__((aligned(4))) static uint8_t iap_ep1_buf[64];
__attribute__((aligned(4))) static uint8_t iap_ep2_buf[64];
__attribute__((aligned(4))) static uint8_t iap_ep3_buf[64 + 64];

/* SDK 把 pSetupReqPak 定义为基于 pEP0_RAM_Addr 的宏；此处改用本文件的 EP0 缓冲 */
#undef pSetupReqPak
#define pSetupReqPak ((PUSB_SETUP_REQ)iap_ep0_buf)

/* ============================ 描述符 ============================
 * 关键：本 BOOT 的设备/配置/报表描述符必须与 App（port/ch58x/bridge_usb_ch58x.c）
 * **逐字节一致**。Chrome WebHID 的授权按「设备实例」记忆（iSerial 为空时以接口
 * 路径 mi_XX 区分）；若 App 为复合 3 接口（Raw HID=IF2）而 BOOT 为单接口
 * （Raw HID=IF0），二者在 Windows 上是不同实例 → 首次授权无法跨枚举延续 →
 * navigator.hid.getDevices() 静默重连失败，表现为「要连两次」「升完回 App 检测
 * 超时误报失败」。保持描述符一致后 App↔BOOT 重枚举路径相同，重连用户无感。
 */

/* 设备描述符（与 App 一致：VID/PID、bcdDevice、iSerial=NULL 均相同） */
static const uint8_t s_dev_desc[] = {
    0x12, 0x01,             /* bLength, DEVICE */
    0x10, 0x01,             /* bcdUSB 1.10 */
    0x00, 0x00, 0x00,       /* class/subclass/protocol：按接口声明 */
    0x40,                   /* bMaxPacketSize0 = 64 */
    (uint8_t)(BRIDGE_USB_VID & 0xFFu), (uint8_t)(BRIDGE_USB_VID >> 8),
    (uint8_t)(BRIDGE_USB_PID & 0xFFu), (uint8_t)(BRIDGE_USB_PID >> 8),
    0x00, 0x01,             /* bcdDevice 1.00 */
    0x01, 0x02, 0x00, 0x01  /* iManufacturer,iProduct,iSerial=NULL,1 config */
};

/* IF1 的 Report ID（与 s_media_report 的声明一致） */
#define OTA_RID_MOUSE    0x01u
#define OTA_RID_SYSTEM   0x02u
#define OTA_RID_CONSUMER 0x03u
#define OTA_RID_NKRO     0x04u

/* IF0：Boot 键盘报表 —— 与 App 的 s_kbd_report 逐字节一致（本 BOOT 不使用该接口，
 * 仅用于让 Windows 生成与 App 相同的设备实例/接口布局） */
static const uint8_t s_kbd_report[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x91, 0x02, 0x95, 0x05, 0x75, 0x01, 0x91,
    0x01, 0x95, 0x06, 0x75, 0x08, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0x91, 0x81,
    0x00, 0xC0
};

/* IF1：鼠标/系统/多媒体/NKRO —— 与 App 的 s_media_report 逐字节一致（同上，占位用） */
static const uint8_t s_media_report[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, OTA_RID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,
    0xC0, 0xC0,

    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, OTA_RID_SYSTEM,
    0x19, 0x01, 0x29, 0xB7, 0x15, 0x01, 0x25, 0xB7, 0x95, 0x01, 0x75, 0x08, 0x81, 0x00,
    0xC0,

    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, OTA_RID_CONSUMER,
    0x19, 0x01, 0x2A, 0xA0, 0x02, 0x15, 0x01, 0x26, 0xA0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00,
    0xC0,

    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, OTA_RID_NKRO,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x77, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x78, 0x81, 0x02,
    0xC0
};

/* IF2：VIA Raw HID —— 与 App 的 IF2 逐字节一致（32B 双向、无 Report ID）。
 * 上位机 filters:[{usagePage:0xFF60}] 与 sendReport(0,32B) 依赖此接口。 */
static const uint8_t s_raw_report[] = {
    0x06, 0x60, 0xFF,        /* Usage Page (Vendor Defined 0xFF60) */
    0x09, 0x61,              /* Usage (0x61) */
    0xA1, 0x01,              /* Collection (Application) */
    0x09, 0x62,              /*   Usage (0x62)：Data to host */
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x20,
    0x81, 0x02,              /*   Input (Data,Var,Abs) 32B */
    0x09, 0x63,              /*   Usage (0x63)：Data from host */
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x20,
    0x91, 0x02,              /*   Output (Data,Var,Abs) 32B */
    0xC0
};

/* 配置描述符：复合 3 接口，与 App 逐字节一致（wTotalLength = 91）。
 * 仅 IF2 的 EP3 被本 BOOT 实际启用（见 iap_usb_init）；IF0/IF1 的 EP1/EP2
 * 只在描述符中声明、硬件不使能（主机轮询得到 NAK，不影响枚举）。 */
static const uint8_t s_cfg_desc[] = {
    0x09, 0x02, 0x5B, 0x00, 0x03, 0x01, 0x00, 0xA0, 0x32,

    /* ---- IF0：键盘（Boot，无 Report ID）---- */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_kbd_report), 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,        /* EP1 IN, 8B, 10ms */

    /* ---- IF1：多媒体/系统/鼠标/NKRO（Report ID）---- */
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_media_report), 0x00,
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x0A,        /* EP2 IN, 32B, 10ms */

    /* ---- IF2：VIA Raw HID（EP3 IN/OUT，OTA 走此接口）---- */
    0x09, 0x04, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_raw_report), 0x00,
    0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x01,        /* EP3 IN, 32B, 1ms */
    0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x01         /* EP3 OUT, 32B, 1ms */
};

typedef char iap_usb_cfg_len_check[(sizeof(s_cfg_desc) == 91u) ? 1 : -1];

static const uint8_t s_lang_desc[] = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t s_manu_desc[] = { 0x0A, 0x03, 'S', 0, 'K', 0, '3', 0, '2', 0 };
static const uint8_t s_prod_desc[] = {
    0x1C, 0x03,
    'S', 0, 'K', 0, '3', 0, '2', 0, ' ', 0, 'W', 0, 'i', 0, 'r', 0, 'e', 0, 'l', 0, 'e', 0, 's', 0, 's', 0
};

/* ============================ 控制传输状态 ============================ */
static volatile uint8_t s_dev_config;
static uint8_t          s_setup_req_code;
static uint16_t         s_setup_req_len;
static const uint8_t   *s_descr;
static uint8_t          s_idle_value;

/* ============================ OTA 接收状态 ============================ */
static bool     s_recv;
static uint32_t s_total;
static uint32_t s_written;
static uint32_t s_erased;
static uint16_t s_crc_exp;
static uint16_t s_crc_run;
static volatile uint32_t s_idle_ticks;

/* FLASH_ROM_WRITE 要求 RAM 且 4 字节对齐的中转缓冲（同 App s_fw_wbuf 的教训） */
static __attribute__((aligned(4))) uint8_t s_wbuf[64];

/* 待落盘片：FW_DATA 只把负载拷入 s_wbuf 并**立即应答**，真正的擦/写推迟到主循环
 * （USB_DevTransProcess 之后）执行。FLASH_ROM_ERASE 单页可阻塞数十毫秒，若在 USB
 * 处理路径内同步执行，主机 OUT 会因长时间 NAK 而写超时（"Failed to write the report."）。 */
static bool         s_pend;
static uint32_t     s_pend_off;
static uint8_t      s_pend_len;
static bool         s_io_err;   /* 落盘失败：后续帧一律回 ERR_IO，FW_END 亦拒绝 */

/* ============================ EP3 IN 应答 ============================ */
/* 固定回 32B：byte0=status（0=OK，非 0 为 BRIDGE_ERR_* 的 uint8 形式），其后为 data */
static void iap_reply(uint8_t status, const uint8_t *data, uint8_t len)
{
    uint8_t *out = iap_ep3_buf + 64;

    /* 直接覆盖上一包 IN（不判 T_RES）：本 BOOT 是严格「一请求一应答」，上位机在超时/
     * 换设备时会放弃尚未取走的应答，随后还会重发同一子命令。若此处因 T_RES==ACK 拒绝
     * 覆盖，重发的请求将永远拿不到应答（实测表现为 GET_INFO/FW_BEGIN 无应答超时）。
     * 硬件同步位在「本次 IN 完成」时才前进，覆盖未取走的旧包不会造成同步位错位。 */
    memset(out, 0, OTA_REPORT_LEN);
    out[0] = status;
    if (data != 0 && len != 0u) {
        if (len > (uint8_t)(OTA_REPORT_LEN - 1u)) {
            len = (uint8_t)(OTA_REPORT_LEN - 1u);
        }
        memcpy(&out[1], data, len);
    }
    R8_UEP3_T_LEN = OTA_REPORT_LEN;
    R8_UEP3_CTRL  = (uint8_t)((R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK);
}

static void iap_reply_status(int rc)
{
    iap_reply((uint8_t)rc, 0, 0u);
}

/* ============================ Stage 顺序写入 ============================ */
/* 把 s_wbuf 中已就绪的一片写入 Stage（共享实现 iap_stage_write，按需擦 4K 页）。
 * 仅在主循环调用（见 s_pend）。 */

/* 主循环调用：把上一帧 FW_DATA 的负载落盘（失败置 s_io_err，后续帧/FW_END 报错）。 */
static void iap_flush_pending(void)
{
    if (!s_pend) {
        return;
    }
    s_pend = false;
    if (iap_stage_write(&s_erased, s_wbuf, s_pend_off, s_pend_len) != 0u) {
        s_io_err = true;
    }
}

static uint32_t ota_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============================ OTA 子命令处理 ============================ */

static void iap_ota_get_info(void)
{
    uint8_t r[7];

    r[0] = OTA_MODE_IAP;
    r[1] = (uint8_t)(BRIDGE_FW_REV & 0xFFu);
    r[2] = (uint8_t)(BRIDGE_FW_REV >> 8);
    r[3] = (uint8_t)(BRIDGE_USB_VID & 0xFFu);
    r[4] = (uint8_t)(BRIDGE_USB_VID >> 8);
    r[5] = (uint8_t)(BRIDGE_USB_PID & 0xFFu);
    r[6] = (uint8_t)(BRIDGE_USB_PID >> 8);
    iap_reply(0u, r, (uint8_t)sizeof(r));
}

static void iap_ota_fw_begin(const uint8_t *args, uint8_t alen)
{
    uint32_t total;
    uint16_t crc;
    uint8_t  cm = OTA_CHUNK_MAX;

    if (alen < 6u) {
        iap_reply_status(BRIDGE_ERR_PARAM);
        return;
    }
    total = ota_rd_u32(args);
    crc   = (uint16_t)((uint16_t)args[4] | ((uint16_t)args[5] << 8));
    if (total == 0u || total > BRIDGE_FW_MAX_LEN || (total & 3u) != 0u) {
        iap_reply_status(BRIDGE_ERR_PARAM);
        return;
    }

    s_total    = total;
    s_written  = 0u;
    s_erased   = 0u;
    s_crc_run  = 0xFFFFu;
    s_crc_exp  = crc;
    s_recv     = true;
    s_pend     = false;
    s_io_err   = false;
    iap_reply(0u, &cm, 1u);
}

static void iap_ota_fw_data(const uint8_t *args, uint8_t alen)
{
    uint32_t off;
    uint8_t  dlen;

    if (!s_recv) {
        iap_reply_status(BRIDGE_ERR_STATE);
        return;
    }
    if (s_io_err) {
        iap_reply_status(BRIDGE_ERR_IO);    /* 之前某片落盘失败：不再继续 */
        return;
    }
    if (alen < 5u) {
        iap_reply_status(BRIDGE_ERR_PARAM);
        return;
    }
    off  = ota_rd_u32(args);
    dlen = (uint8_t)(alen - 4u);
    if (dlen == 0u || dlen > OTA_CHUNK_MAX || (dlen & 3u) != 0u || (off & 3u) != 0u) {
        iap_reply_status(BRIDGE_ERR_PARAM);
        return;
    }
    if (off + (uint32_t)dlen <= s_written) {
        iap_reply_status(BRIDGE_OK);        /* 重复帧（ACK 丢失重传）：幂等确认 */
        return;
    }
    if (off != s_written) {
        iap_reply_status(BRIDGE_ERR_PARAM); /* 乱序一律拒绝 */
        return;
    }
    if ((uint32_t)dlen > (s_total - s_written)) {
        /* 末帧：HID OUT 报告定长 32B，发送方无法表达短包，故最后一片会带零填充
         * 被声明成 24B。超出声明总长的尾部一律按填充忽略，只写入剩余字节
         * （total/off 均 4 对齐 ⇒ 剩余必为 4 的倍数）。 */
        dlen = (uint8_t)(s_total - s_written);
        if (dlen == 0u) {
            iap_reply_status(BRIDGE_ERR_PARAM);
            return;
        }
    }
    /* 只缓冲 + 记账 + 立即应答；真正擦/写交给 iap_flush_pending()（主循环），
     * 避免在 USB 处理路径内阻塞导致主机 OUT 写超时。 */
    memcpy(s_wbuf, &args[4], dlen);
    s_pend_off = off;
    s_pend_len = dlen;
    s_pend     = true;
    s_crc_run  = iap_crc16_update(s_crc_run, &args[4], dlen);
    s_written += dlen;
    iap_reply_status(BRIDGE_OK);
}

static void iap_ota_fw_end(void)
{
    iap_flush_pending();        /* 收尾前确保最后一片已落盘 */

    if (!s_recv) {
        iap_reply_status(BRIDGE_ERR_STATE);
        return;
    }
    if (s_io_err) {
        iap_reply_status(BRIDGE_ERR_IO);
        return;
    }
    if (s_written != s_total) {
        iap_reply_status(BRIDGE_ERR_STATE);
        return;
    }
    if (s_crc_run != s_crc_exp) {
        iap_reply_status(BRIDGE_ERR_CRC);   /* 留在 IAP，允许整包重传 */
        return;
    }

    iap_reply_status(BRIDGE_OK);

    /* 先落「暂存就绪」标志：搬运过程中掉电，下次开机走 flag==IAP 的搬运恢复路径 */
    iap_flag_write(BRIDGE_OTA_FLAG_IAP, s_total, s_crc_run);

    /* 复验 + 搬运 Stage→APP_A（成功内部写 flag=A 并擦 Stage）后复位跳新 App。
     * 此处不等主机取走 IN 应答：上位机对 FW_END 无应答按成功处理（SPEC §5.6）。 */
    (void)iap_copy_stage_to_app();
    SYS_ResetExecute();
}

static void iap_ota_fw_abort(void)
{
    iap_reply_status(BRIDGE_OK);
    s_recv = false;
    iap_flag_enter_reset(BRIDGE_OTA_FLAG_A);   /* 回 App（不复返） */
}

/* EP3 OUT 报文入口：仅处理 "OTA"+SUB（IAP 阶段不关心 VIA 透传等其他报文） */
static void iap_ota_on_out(const uint8_t *buf, uint8_t len)
{
    uint8_t sub;
    uint8_t alen;

    if (len < 4u) {
        return;
    }
    if (buf[0] != OTA_MAGIC0 || buf[1] != OTA_MAGIC1 || buf[2] != OTA_MAGIC2) {
        return;
    }
    sub  = buf[3];
    alen = (uint8_t)((len - 4u > OTA_ARGS_MAX) ? OTA_ARGS_MAX : (len - 4u));

    switch (sub) {
    case OTA_SUB_GET_INFO:  iap_ota_get_info();                 break;
    case OTA_SUB_FW_BEGIN:  iap_ota_fw_begin(&buf[4], alen);    break;
    case OTA_SUB_FW_DATA:   iap_ota_fw_data(&buf[4], alen);     break;
    case OTA_SUB_FW_END:    iap_ota_fw_end();                   break;
    case OTA_SUB_FW_ABORT:  iap_ota_fw_abort();                 break;
    case OTA_SUB_ENTER:     iap_reply_status(BRIDGE_ERR_STATE); break;  /* App 专用 */
    default:                iap_reply_status(BRIDGE_ERR_UNSUPPORTED); break;
    }
}

/* ============================ USB 传输处理（轮询式） ============================
 * 描述符为与 App 一致的复合 3 接口，但本 BOOT 实际只启用 IF2 的 EP3（其余接口的
 * EP1/EP2 仅供枚举占位）。由主循环轮询调用（R8_USB_INT_EN=0），与 WCH EVM/IAP/USB_IAP 同款。 */
__attribute__((section(".highcode")))
void USB_DevTransProcess(void)
{
    uint8_t len, chtype;
    uint8_t intflag, errflag;

    intflag = R8_USB_INT_FG;
    if (intflag & RB_UIF_TRANSFER) {
        /* NAK 事件（主机轮询尚无数据的端点）不算“活动”：否则 s_idle_ticks 会被
         * 高频清零，空闲回退永不触发（实测设备因此永久停在 IAP）。与 SDK 例程
         * 一致，用 RB_U_IS_NAK 把 NAK 与真正的传输完成区分开。 */
        if ((R8_USB_INT_ST & RB_U_IS_NAK) == 0u) {
            s_idle_ticks = 0u;
        }
        if ((R8_USB_INT_ST & RB_U_IS_NAK) == 0u &&
            (R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN) {
            switch (R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
            case UIS_TOKEN_IN:              /* EP0 上传 */
                switch (s_setup_req_code) {
                case USB_GET_DESCRIPTOR:
                    len = (s_setup_req_len >= 64u) ? 64u : (uint8_t)s_setup_req_len;
                    memcpy(iap_ep0_buf, s_descr, len);
                    s_setup_req_len -= len;
                    s_descr += len;
                    R8_UEP0_T_LEN = len;
                    R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                    break;
                case USB_SET_ADDRESS:
                    R8_USB_DEV_AD = (uint8_t)((R8_USB_DEV_AD & RB_UDA_GP_BIT) | s_setup_req_len);
                    R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                default:
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                }
                break;

            case UIS_TOKEN_OUT:             /* EP0 下载（HID SET_REPORT 等，忽略数据） */
                break;

            case UIS_TOKEN_OUT | 3:         /* EP3 OUT：OTA 子命令 */
                /* 同步位由硬件 AUTO_TOG 管理，软件**不得**再翻转（否则双重翻转）。
                 * 与 SDK 已验证的 EVT/EXAM/IAP/USB_IAP 同款。 */
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    iap_ota_on_out(iap_ep3_buf, R8_USB_RX_LEN);
                }
                break;

            case UIS_TOKEN_IN | 3:          /* EP3 IN：应答已被主机取走 */
                R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                break;

            /* IF0/IF1 的 EP1/EP2：BOOT 不产生键盘/多媒体数据，端点保持 T_RES=NAK
             * 让主机持续轮询即可（端点已使能，见 iap_usb_init 注释）。 */
            case UIS_TOKEN_IN | 1:
            case UIS_TOKEN_IN | 2:
            case UIS_TOKEN_OUT | 1:
            case UIS_TOKEN_OUT | 2:
                break;

            default:
                break;
            }
        }

        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT) {
            R8_UEP0_CTRL     = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            s_setup_req_len  = pSetupReqPak->wLength;
            s_setup_req_code = pSetupReqPak->bRequest;
            chtype           = pSetupReqPak->bRequestType;

            len     = 0u;
            errflag = 0u;

            if ((pSetupReqPak->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD) {
                /* ---- HID 类请求（单接口）---- */
                if (pSetupReqPak->bRequestType & 0x20) {
                    switch (s_setup_req_code) {
                    case DEF_USB_SET_IDLE:
                        s_idle_value = (uint8_t)(pSetupReqPak->wValue >> 8);
                        break;
                    case DEF_USB_SET_REPORT:
                        break;      /* 数据在 OUT 阶段，忽略 */
                    case DEF_USB_GET_IDLE:
                        iap_ep0_buf[0] = s_idle_value;
                        len            = 1u;
                        break;
                    case DEF_USB_GET_PROTOCOL:
                        iap_ep0_buf[0] = 1u;
                        len            = 1u;
                        break;
                    case DEF_USB_SET_PROTOCOL:
                        break;
                    default:
                        errflag = 0xFFu;
                        break;
                    }
                } else {
                    errflag = 0xFFu;
                }
            } else {
                /* ---- 标准请求 ---- */
                switch (s_setup_req_code) {
                case USB_GET_DESCRIPTOR:
                    switch (pSetupReqPak->wValue >> 8) {
                    case USB_DESCR_TYP_DEVICE:
                        s_descr = s_dev_desc;
                        len     = s_dev_desc[0];
                        break;
                    case USB_DESCR_TYP_CONFIG:
                        s_descr = s_cfg_desc;
                        len     = s_cfg_desc[2];
                        break;
                    case USB_DESCR_TYP_HID:
                        /* 复合 3 接口：HID 描述符分别在配置描述符偏移 18/43/68 */
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case 0: s_descr = &s_cfg_desc[18]; len = 9u; break;
                        case 1: s_descr = &s_cfg_desc[43]; len = 9u; break;
                        case 2: s_descr = &s_cfg_desc[68]; len = 9u; break;
                        default: errflag = 0xFFu; break;
                        }
                        break;
                    case USB_DESCR_TYP_REPORT:
                        /* 按接口号返回对应报表描述符（主机对每个 HID 接口单独请求） */
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case 0: s_descr = s_kbd_report;   len = (uint8_t)sizeof(s_kbd_report);   break;
                        case 1: s_descr = s_media_report; len = (uint8_t)sizeof(s_media_report); break;
                        case 2: s_descr = s_raw_report;   len = (uint8_t)sizeof(s_raw_report);   break;
                        default: errflag = 0xFFu; break;
                        }
                        break;
                    case USB_DESCR_TYP_STRING:
                        switch (pSetupReqPak->wValue & 0xFFu) {
                        case 0: s_descr = s_lang_desc; len = s_lang_desc[0]; break;
                        case 1: s_descr = s_manu_desc; len = s_manu_desc[0]; break;
                        case 2: s_descr = s_prod_desc; len = s_prod_desc[0]; break;
                        default: errflag = 0xFFu; break;
                        }
                        break;
                    default:
                        errflag = 0xFFu;
                        break;
                    }
                    if (errflag != 0xFFu) {
                        if (s_setup_req_len > len) {
                            s_setup_req_len = len;
                        }
                        len = (s_setup_req_len >= 64u) ? 64u : (uint8_t)s_setup_req_len;
                        memcpy(iap_ep0_buf, s_descr, len);
                        s_descr += len;
                    }
                    break;

                case USB_SET_ADDRESS:
                    s_setup_req_len = pSetupReqPak->wValue & 0xFFu;
                    break;

                case USB_GET_CONFIGURATION:
                    iap_ep0_buf[0] = s_dev_config;
                    if (s_setup_req_len > 1u) {
                        s_setup_req_len = 1u;
                    }
                    break;

                case USB_SET_CONFIGURATION:
                    s_dev_config = (uint8_t)(pSetupReqPak->wValue & 0xFFu);
                    break;

                case USB_CLEAR_FEATURE:
                case USB_SET_FEATURE:
                    if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                        uint8_t is_set = (s_setup_req_code == USB_SET_FEATURE);
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case 0x83:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) |
                                                     (is_set ? UEP_T_RES_STALL : UEP_T_RES_NAK));
                            break;
                        case 0x03:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) |
                                                     (is_set ? UEP_R_RES_STALL : UEP_R_RES_ACK));
                            break;
                        case 0x81:      /* IF0 键盘 EP1：描述符声明但未使能，接受后忽略 */
                        case 0x01:
                        case 0x82:      /* IF1 辅助键 EP2：同上 */
                        case 0x02:
                            break;
                        default:
                            errflag = 0xFFu;
                            break;
                        }
                    } else {
                        errflag = 0xFFu;
                    }
                    break;

                case USB_GET_INTERFACE:
                    iap_ep0_buf[0] = 0u;
                    if (s_setup_req_len > 1u) {
                        s_setup_req_len = 1u;
                    }
                    break;

                case USB_SET_INTERFACE:
                    break;

                case USB_GET_STATUS:
                    iap_ep0_buf[0] = 0u;
                    iap_ep0_buf[1] = 0u;
                    if (s_setup_req_len > 2u) {
                        s_setup_req_len = 2u;
                    }
                    break;

                default:
                    errflag = 0xFFu;
                    break;
                }
            }

            if (errflag == 0xFFu) {
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            } else {
                if (chtype & 0x80) {
                    len = (s_setup_req_len > 64u) ? 64u : (uint8_t)s_setup_req_len;
                    s_setup_req_len -= len;
                } else {
                    len = 0u;
                }
                R8_UEP0_T_LEN = len;
                R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }
        }
        R8_USB_INT_FG = RB_UIF_TRANSFER;
    } else if (intflag & RB_UIF_BUS_RST) {
        s_dev_config = 0u;
        R8_USB_DEV_AD = 0x00;
        R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_UEP2_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        /* EP3 同步位交给硬件 AUTO_TOG（软件不再翻转），与 SDK USB_IAP 例程一致。
         * 这样即便 ISR 被 NAK/重复事件多触发一次，也只是把 T_RES 重设为 NAK，不会
         * 误翻同步位造成与主机不同步。 */
        R8_UEP3_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    } else if (intflag & RB_UIF_SUSPEND) {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    } else {
        R8_USB_INT_FG = intflag;
    }
}

/* ============================ USB 初始化（轮询式，手动配端点） ============================ */
static void iap_usb_init(void)
{
    R8_USB_CTRL = 0x00;     /* 取消 RB_UC_CLR_ALL 等设定模式 */

    /* 使能描述符中声明的**全部**端点（与 SDK USB_DeviceInit() 一致）。
     * IF0/IF1 的 EP1/EP2 虽不承载 OTA 数据，但必须使能：否则 Windows 的
     * kbd/mou 驱动对它们的读请求永远无人应答，主机会反复复位设备（实测 DEV_AD
     * 在 0x33/0x00 间跳动、Chrome sendReport 永久挂起、GET_INFO 无应答）。
     * 使能后端点以 NAK 正确应答“暂无数据”，主机不再复位。 */
    R8_UEP4_1_MOD = RB_UEP1_RX_EN | RB_UEP1_TX_EN;
    R8_UEP2_3_MOD = RB_UEP2_RX_EN | RB_UEP2_TX_EN | RB_UEP3_RX_EN | RB_UEP3_TX_EN;

    R16_UEP0_DMA = (uint16_t)(uint32_t)iap_ep0_buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)iap_ep1_buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)iap_ep2_buf;
    R16_UEP3_DMA = (uint16_t)(uint32_t)iap_ep3_buf;

    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;   /* 同步位硬件管理 */

    R8_USB_DEV_AD = 0x00;
    R8_USB_CTRL   = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
    R8_USB_INT_FG = 0xFF;
    R8_UDEV_CTRL  = RB_UD_PD_DIS | RB_UD_PORT_EN;
    R8_USB_INT_EN = 0x00;   /* 轮询式：不用 USB 中断 */

    /* USB 端口模拟部分（防止干扰模拟输入） */
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;

    s_dev_config = 0u;
}

/* ============================ 接收模式主循环（不复返） ============================ */
static void iap_usb_ota_run(void)
{
    iap_usb_init();
    s_recv       = false;
    s_pend       = false;
    s_io_err     = false;
    s_idle_ticks = 0u;

    for (;;) {
        USB_DevTransProcess();
        iap_flush_pending();    /* 把上一帧负载落盘（见 s_pend 说明） */

        if (!s_recv && s_idle_ticks > OTA_IDLE_TICKS) {
            /* 长时间无任何 USB 活动：放弃本次接收。
             * 覆盖两类场景：① 升级中断（关页/拔线）；② 上次 OTA 遗留 flag=USB_IAP
             * 但主机未再来（拔线/关页），若无此超时设备将**永久停在接收模式**。
             * App 有效 → 写 flag=A 复位回 App（不复返，ImageA 未动，旧 App 照常启动）；
             * App 无效（空片，工厂只烧 BOOT）→ 留在接收模式继续等主机，不做无谓复位。
             * 传输中（s_recv，已 FW_BEGIN 尚未 FW_END）不触发，避免大镜像传输中途被
             * 复位丢弃；代价：BEGIN 后主机消失且不再回来时不会自动回退，需重新上电。 */
            if (iap_app_valid()) {
                iap_flag_enter_reset(BRIDGE_OTA_FLAG_A);   /* 不复返 */
            }
            s_idle_ticks = 0u;
        }

        s_idle_ticks++;
        DelayUs(20u);
    }
}

/* ============================ 入口 ============================ */
int main(void)
{
    uint8_t flag = 0u;

    SetSysClock(CLK_SOURCE_PLL_60MHz);
    iap_flag_read(&flag, 0, 0);

    if (flag == BRIDGE_OTA_FLAG_USB_IAP) {
        iap_usb_ota_run();      /* 不复返：FW_END / FW_ABORT / 超时都会复位跳 App */
    }

    /* 暂存就绪：搬运 Stage→APP_A（含「搬运中掉电」的重启恢复）；失败则清标志回旧 App */
    if (flag == BRIDGE_OTA_FLAG_IAP) {
        if (!iap_copy_stage_to_app()) {
            iap_flag_write(BRIDGE_OTA_FLAG_A, 0u, 0u);
        }
    }

    /* 空片 / 坏镜像（工厂只烧 BOOT、App 区尚未写入）：直接进 USB HID 收包，
     * 让上位机可完成首次 OTA —— 这是「工厂只烧一个 hex」的关键。
     * FW_ABORT / 超时会写 flag=A 复位；届时 App 仍无效，会再次回到本收包模式。 */
    if (!iap_app_valid()) {
        iap_usb_ota_run();
    }

    iap_jump_app();
    return 0;   /* 不会到达 */
}
