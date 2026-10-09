/* Dongle USB 复合设备 —— HID（键盘/多媒体/系统/鼠标/NKRO）+ VIA Raw HID
 *
 * 参考例程（本工程 submodule openwch/ch583 @ bd508ad7）：
 *   EVT/EXAM/USB/Device/CompoundDev/src/Main.c   USB1 复合 HID 蓝本
 *     （USB_DeviceInit + 端点缓冲 + USB_DevTransProcess + DevEPn_OUT_Deal）
 *   EVT/EXAM/BLE/BLE_USB/APP/app_usb.c           BLE 与 USB1 共存时的
 *     初始化顺序（pEPx_RAM_Addr → USB_DeviceInit → PFIC_EnableIRQ(USB_IRQn)）
 *     与 USB_IRQHandler 写法（证明厂商 BLE 库不占用 USB 中断向量）
 *   tmk_core/protocol/usb_descriptor.c           QMK 报表描述符口径 —— 本工程
 *     对端 SK32 跑 QMK，故各 HID 报文的长度/语义以此为准
 *
 * 接口布局（USB 复合设备，3 接口 / 4 端点中的 EP1~EP3）：
 *   IF0 键盘     ：Boot 键盘 8B 输入（EP1 IN），LED 1B 输出经 EP0 SET_REPORT 下发
 *   IF1 辅助键   ：多媒体 2B / 系统 1B / 鼠标 4B / NKRO 16B，Report ID 区分（EP2 IN）
 *   IF2 VIA Raw  ：usage page 0xFF60 / usage 0x61，32B IN+OUT（EP3 IN/OUT）
 *
 * 数据方向（与 core 的约定）：
 *   键盘 → 主机：2.4G 收到 HID/透传 → sdk_usb_hid_send() / sdk_usb_tunnel_send()
 *   主机 → 键盘：EP3 OUT（Raw HID）/ EP0 SET_REPORT（LED）
 *               → sdk_rf24_send_tunnel() / sdk_rf24_send()
 *   PC OTA     ：EP3 OUT 首 4 字节 = "OTA!" → 释放按键并复位进 ISP BootLoader
 *
 * 本文件包含 WCH SDK 头，**不参与** test_host 构建（见 test_host/Makefile）。
 */
#include "CONFIG.h"            /* BLE 配置 + 协议栈头 + 外设头（含 CH58x_usbdev.h） */
#include "HAL.h"
#include "bridge_ch58x_sdk.h"
#include "bridge_ch58x.h"
#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_hid.h"
#include "bridge_log.h"
#include "bridge_port.h"
#include "bridge_ota.h"          /* App 侧 OTA 状态机（Dongle 经 USB HID 收包，见下） */
#include "bridge_ota_layout.h"

#if BRIDGE_BOARD_HAS_USB

#define BRIDGE_MOD_USB "usb"

/* USB 设备标识：VIA 按 (VID,PID) 匹配键盘定义，故必须与目标键盘 via.json 一致。
 * 默认取 KB17；配套 LD7_OLED 时在 board/dongle/bridge_board.h 覆盖为 0x0464。 */
#ifndef BRIDGE_USB_VID
#define BRIDGE_USB_VID 0x0194u
#endif
#ifndef BRIDGE_USB_PID
#define BRIDGE_USB_PID 0x0463u
#endif

#define USB_DEV_EP0_SIZE   0x40u
#define USB_IF_CNT         3u
#define USB_IF_KBD         0u
#define USB_IF_MEDIA       1u
#define USB_IF_RAW         2u

/* IF1 的 Report ID（与 s_media_report 内声明一一对应） */
#define USB_RID_MOUSE      0x01u
#define USB_RID_SYSTEM     0x02u
#define USB_RID_CONSUMER   0x03u
#define USB_RID_NKRO       0x04u

/* ============================ 报表描述符 ============================ */

/* IF0：Boot 键盘（mods + reserved + 6 keys = 8B）+ LED 输出（1B）。
 * 与 CompoundDev 的 KeyRepDesc 同源（厂家验证过的 Boot 键盘报表）。 */
static const uint8_t s_kbd_report[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x91, 0x02, 0x95, 0x05, 0x75, 0x01, 0x91,
    0x01, 0x95, 0x06, 0x75, 0x08, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0x91, 0x81,
    0x00, 0xC0
};

/* IF1：鼠标(4B) + 系统(1B) + 多媒体(2B) + NKRO(16B)，以 Report ID 区分。
 * 各报文长度与 bridge_types.h 的 BRIDGE_REPORT_*_LEN 严格一致。 */
static const uint8_t s_media_report[] = {
    /* Report ID 1：鼠标 4B = 按键(1B) + X/Y/Wheel(3B) */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, USB_RID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,
    0xC0, 0xC0,

    /* Report ID 2：系统控制 1B */
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, USB_RID_SYSTEM,
    0x19, 0x01, 0x29, 0xB7, 0x15, 0x01, 0x25, 0xB7, 0x95, 0x01, 0x75, 0x08, 0x81, 0x00,
    0xC0,

    /* Report ID 3：多媒体 2B（16bit usage） */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, USB_RID_CONSUMER,
    0x19, 0x01, 0x2A, 0xA0, 0x02, 0x15, 0x01, 0x26, 0xA0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00,
    0xC0,

    /* Report ID 4：NKRO 16B = 修饰键(1B) + 键位位图(120bit = 15B) */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, USB_RID_NKRO,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x77, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x78, 0x81, 0x02,
    0xC0
};

/* IF2：VIA Raw HID，usage page 0xFF60 / usage 0x61，32B 双向（QMK RAW_EPSIZE） */
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

/* ============================ 设备 / 配置 / 字符串描述符 ============================ */

/* 设备描述符：VID/PID 可运行期改写（CMD_SET_USB_ID），故非常量 */
static uint8_t s_dev_desc[] = {
    0x12, 0x01,             /* bLength, DEVICE */
    0x10, 0x01,             /* bcdUSB 1.10（FS） */
    0x00, 0x00, 0x00,       /* class/subclass/protocol：按接口声明 */
    USB_DEV_EP0_SIZE,
    (uint8_t)(BRIDGE_USB_VID & 0xFFu), (uint8_t)(BRIDGE_USB_VID >> 8),
    (uint8_t)(BRIDGE_USB_PID & 0xFFu), (uint8_t)(BRIDGE_USB_PID >> 8),
    0x00, 0x01,             /* bcdDevice 1.00 */
    0x01, 0x02, 0x00, 0x01  /* iManufacturer, iProduct, iSerial=NULL, 1 config */
};

/* 配置描述符：3 接口，HID 描述符分别在偏移 18 / 43 / 68 */
static const uint8_t s_cfg_desc[] = {
    /* 配置描述符：wTotalLength 见下方编译期长度校验 */
    0x09, 0x02, 0x5B, 0x00, USB_IF_CNT, 0x01, 0x00, 0xA0, 0x32,

    /* ---- IF0：键盘（Boot，无 Report ID）---- */
    0x09, 0x04, USB_IF_KBD, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_kbd_report), 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,        /* EP1 IN, 8B, 10ms */

    /* ---- IF1：多媒体/系统/鼠标/NKRO（Report ID）---- */
    0x09, 0x04, USB_IF_MEDIA, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_media_report), 0x00,
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x0A,        /* EP2 IN, 32B, 10ms */

    /* ---- IF2：VIA Raw HID ---- */
    0x09, 0x04, USB_IF_RAW, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)sizeof(s_raw_report), 0x00,
    0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x01,        /* EP3 IN, 32B, 1ms */
    0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x01         /* EP3 OUT, 32B, 1ms */
};

/* 编译期校验：wTotalLength 字段必须等于配置描述符实际长度，否则枚举行失败 */
typedef char bridge_usb_cfg_len_check[(sizeof(s_cfg_desc) == 91u) ? 1 : -1];

static const uint8_t s_lang_desc[] = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t s_manu_desc[] = { 0x0A, 0x03, 'S', 0, 'K', 0, '3', 0, '2', 0 };
/* 产品名字符串描述符：[bLength][0x03][UTF-16LE...]，可由 CMD_SET_NAME 重建（故非常量）。
 * 容量按 BRIDGE_NAME_MAX（21 字符）预留：2 + 21*2 = 44B。默认 "SK32 Wireless"（13 字符）。 */
static uint8_t s_prod_desc[2u + BRIDGE_NAME_MAX * 2u] = {
    0x1C, 0x03,
    'S', 0, 'K', 0, '3', 0, '2', 0, ' ', 0, 'W', 0, 'i', 0, 'r', 0, 'e', 0, 'l', 0, 'e', 0, 's', 0, 's', 0
};

/* ============================ 端点缓冲 ============================ */
/* 布局与 USB_DeviceInit 的约定一致：EP0(64) + EP4out(64) + EP4in(64) */
__attribute__((aligned(4))) static uint8_t s_ep0_buf[64 + 64 + 64];
__attribute__((aligned(4))) static uint8_t s_ep1_buf[64 + 64];
__attribute__((aligned(4))) static uint8_t s_ep2_buf[64 + 64];
__attribute__((aligned(4))) static uint8_t s_ep3_buf[64 + 64];

/* ============================ HID 发送队列（EP1/EP2 背压） ============================
 * 旧实现直接覆写 pEPn_IN_DataBuf 并重挂 ACK：主机尚未取走上一条时报文即被覆盖，
 * 且与 USB 外设读取缓冲并发 → 报文撕裂。主机侧表现为「丢 press（丢键）」或
 * 「丢 release（OS 一直按住 → 连发）」。
 * 现改为每端点一个小环队：只有主机取走（IN 完成中断置 NAK）后由 ISR 排空下一帧；
 * 队满丢最旧（HID 绝对报文语义：保最新状态），并计数 s_hid_q_drop。
 * EP1：键盘 8B（无 Report ID）；EP2：辅助键（首字节 Report ID + 载荷）。 */
#define USB_HID_Q_DEPTH   4u
#define USB_EP2_SLOT      (1u + BRIDGE_REPORT_NKRO_LEN)

static uint8_t  s_ep1_q[USB_HID_Q_DEPTH][BRIDGE_REPORT_KEYBOARD_LEN];
static uint8_t  s_ep1_q_head;
static uint8_t  s_ep1_q_cnt;

static uint8_t  s_ep2_q[USB_HID_Q_DEPTH][USB_EP2_SLOT];
static uint8_t  s_ep2_q_len[USB_HID_Q_DEPTH];
static uint8_t  s_ep2_q_head;
static uint8_t  s_ep2_q_cnt;

static uint32_t s_hid_q_drop;      /* 队满丢帧计数（观测用；>0 说明主机取走速率低于上报速率） */

static bool usb_ep_in_busy(uint8_t ep)
{
    uint8_t ctrl = (ep == 1u) ? R8_UEP1_CTRL : R8_UEP2_CTRL;
    return ((ctrl & MASK_UEP_T_RES) == UEP_T_RES_ACK);
}

static void usb_ep1_drain(void)
{
    if (s_ep1_q_cnt == 0u || usb_ep_in_busy(1u)) {
        return;
    }
    memcpy(pEP1_IN_DataBuf, s_ep1_q[s_ep1_q_head], BRIDGE_REPORT_KEYBOARD_LEN);
    s_ep1_q_head = (uint8_t)((s_ep1_q_head + 1u) % USB_HID_Q_DEPTH);
    s_ep1_q_cnt--;
    DevEP1_IN_Deal(BRIDGE_REPORT_KEYBOARD_LEN);
}

static void usb_ep2_drain(void)
{
    uint8_t len;

    if (s_ep2_q_cnt == 0u || usb_ep_in_busy(2u)) {
        return;
    }
    len = s_ep2_q_len[s_ep2_q_head];
    memcpy(pEP2_IN_DataBuf, s_ep2_q[s_ep2_q_head], len);
    s_ep2_q_head = (uint8_t)((s_ep2_q_head + 1u) % USB_HID_Q_DEPTH);
    s_ep2_q_cnt--;
    DevEP2_IN_Deal(len);
}

static uint8_t usb_q_slot(uint8_t head, uint8_t cnt)
{
    return (uint8_t)((head + cnt) % USB_HID_Q_DEPTH);
}

static void usb_ep1_push(const uint8_t *d)
{
    if (s_ep1_q_cnt >= USB_HID_Q_DEPTH) {
        s_ep1_q_head = (uint8_t)((s_ep1_q_head + 1u) % USB_HID_Q_DEPTH);
        s_ep1_q_cnt--;
        s_hid_q_drop++;
    }
    memcpy(s_ep1_q[usb_q_slot(s_ep1_q_head, s_ep1_q_cnt)], d, BRIDGE_REPORT_KEYBOARD_LEN);
    s_ep1_q_cnt++;
}

/* EP2 帧 = [Report ID][payload]：Report ID 直接写进队列槽，省一次中转拷贝 */
static void usb_ep2_push(uint8_t rid, const uint8_t *d, uint8_t len)
{
    uint8_t slot;

    if (s_ep2_q_cnt >= USB_HID_Q_DEPTH) {
        s_ep2_q_head = (uint8_t)((s_ep2_q_head + 1u) % USB_HID_Q_DEPTH);
        s_ep2_q_cnt--;
        s_hid_q_drop++;
    }
    slot = usb_q_slot(s_ep2_q_head, s_ep2_q_cnt);
    s_ep2_q[slot][0] = rid;
    memcpy(&s_ep2_q[slot][1], d, len);
    s_ep2_q_len[slot] = (uint8_t)(len + 1u);
    s_ep2_q_cnt++;
}

static void usb_hid_q_reset(void)
{
    s_ep1_q_head = 0u;
    s_ep1_q_cnt  = 0u;
    s_ep2_q_head = 0u;
    s_ep2_q_cnt  = 0u;
    s_hid_q_drop = 0u;
}

/* ============================ 控制传输状态 ============================ */
static bool            s_usb_inited;
static volatile uint8_t s_dev_config;
static uint8_t         s_setup_req_code;
static uint16_t        s_setup_req_len;
static const uint8_t  *s_descr;
static uint8_t         s_idle_value[USB_IF_CNT];
static uint8_t         s_report_value[USB_IF_CNT];

/* ---- USB 远程唤醒状态 ---- */
/* 主机在允许设备唤醒前会下发 SET_FEATURE(DEVICE_REMOTE_WAKEUP)，拔出/复位时清除。
 * 该许可仅对"本次挂起周期"有效：唤醒一次后即消费掉，直到主机再次下发。 */
static volatile uint8_t s_usb_rmt_wake_armed;
/* 挂起期间触发唤醒的那一条 HID 报文：DevWakeup 会抖 D+ 上拉导致主机重新枚举
 * （BUS_RST 会清空 HID 队列），故先暂存，待重枚举完成由 sdk_usb_poll() 补发。 */
static uint8_t  s_wake_report[BRIDGE_REPORT_NKRO_LEN];
static uint8_t  s_wake_report_len;
static bool     s_wake_report_pending;

/* 主机挂起/恢复边沿检测（sdk_usb_poll 内），用于经 2.4G 通知键盘。
 * 初值 false=未挂起：上电未挂起时不产生冗余事件。 */
static bool     s_host_suspended_prev;

/* ============================ 传输处理（EP0 / 各端点） ============================ */

/* 该函数在 USB 中断上下文运行；**禁止**在此打印日志（规格 11.1） */
void USB_DevTransProcess(void)
{
    uint8_t len, chtype;
    uint8_t intflag, errflag = 0;

    intflag = R8_USB_INT_FG;
    if (intflag & RB_UIF_TRANSFER) {
        if ((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN) {
            switch (R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
            case UIS_TOKEN_IN:              /* EP0 上传 */
                switch (s_setup_req_code) {
                case USB_GET_DESCRIPTOR:
                    len = (s_setup_req_len >= USB_DEV_EP0_SIZE)
                              ? USB_DEV_EP0_SIZE
                              : (uint8_t)s_setup_req_len;
                    memcpy(pEP0_DataBuf, s_descr, len);
                    s_setup_req_len -= len;
                    s_descr += len;
                    R8_UEP0_T_LEN = len;
                    R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                    break;

                case USB_SET_ADDRESS:
                    R8_USB_DEV_AD = (uint8_t)((R8_USB_DEV_AD & RB_UDA_GP_BIT) | s_setup_req_len);
                    R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;

                case USB_SET_FEATURE:
                    break;

                default:
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                }
                break;

            case UIS_TOKEN_OUT:             /* EP0 下载（含 HID SET_REPORT → LED） */
                if (s_setup_req_code == DEF_USB_SET_REPORT &&
                    ((pSetupReqPak->wIndex & 0xFFu) == USB_IF_KBD)) {
                    /* 键盘接口无 Report ID，第 0 字节即 LED 状态 */
                    sdk_usb_on_led_report(pEP0_DataBuf[0]);
                }
                break;

            case UIS_TOKEN_OUT | 1:         /* EP1 OUT：键盘接口未声明 OUT，忽略 */
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    R8_UEP1_CTRL ^= RB_UEP_R_TOG;
                }
                break;

            case UIS_TOKEN_IN | 1:          /* EP1 IN：键盘 8B 已取走 → 排空下一帧 */
                R8_UEP1_CTRL ^= RB_UEP_T_TOG;
                R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                usb_ep1_drain();
                break;

            case UIS_TOKEN_OUT | 2:         /* EP2 OUT：辅助键接口未声明 OUT，忽略 */
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    R8_UEP2_CTRL ^= RB_UEP_R_TOG;
                }
                break;

            case UIS_TOKEN_IN | 2:          /* EP2 IN：辅助键报文已取走 → 排空下一帧 */
                R8_UEP2_CTRL ^= RB_UEP_T_TOG;
                R8_UEP2_CTRL = (uint8_t)((R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                usb_ep2_drain();
                break;

            case UIS_TOKEN_OUT | 3:         /* EP3 OUT：VIA Raw HID 下行 → 键盘 */
                if (R8_USB_INT_ST & RB_UIS_TOG_OK) {
                    R8_UEP3_CTRL ^= RB_UEP_R_TOG;
                    DevEP3_OUT_Deal(R8_USB_RX_LEN);
                }
                break;

            case UIS_TOKEN_IN | 3:          /* EP3 IN：Raw HID 上行已取走 */
                R8_UEP3_CTRL ^= RB_UEP_T_TOG;
                R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                break;

            default:
                break;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT) {
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            s_setup_req_len  = pSetupReqPak->wLength;
            s_setup_req_code = pSetupReqPak->bRequest;
            chtype           = pSetupReqPak->bRequestType;

            len     = 0;
            errflag = 0;

            if ((pSetupReqPak->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD) {
                /* ---- HID 类请求 ---- */
                if (pSetupReqPak->bRequestType & 0x20) {
                    switch (s_setup_req_code) {
                    case DEF_USB_SET_IDLE:
                        s_idle_value[pSetupReqPak->wIndex & 0xFFu] =
                            (uint8_t)(pSetupReqPak->wValue >> 8);
                        break;
                    case DEF_USB_SET_REPORT:
                        break;      /* 数据在 OUT 阶段处理（LED） */
                    case DEF_USB_SET_PROTOCOL:
                        s_report_value[pSetupReqPak->wIndex & 0xFFu] =
                            (uint8_t)pSetupReqPak->wValue;
                        break;
                    case DEF_USB_GET_IDLE:
                        pEP0_DataBuf[0] = s_idle_value[pSetupReqPak->wIndex & 0xFFu];
                        len             = 1;
                        break;
                    case DEF_USB_GET_PROTOCOL:
                        pEP0_DataBuf[0] = s_report_value[pSetupReqPak->wIndex & 0xFFu];
                        len             = 1;
                        break;
                    default:
                        errflag = 0xFF;
                        break;
                    }
                } else {
                    errflag = 0xFF;     /* 厂商自定义请求不支持 */
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
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case USB_IF_KBD:
                            s_descr = &s_cfg_desc[18];
                            len     = 9;
                            break;
                        case USB_IF_MEDIA:
                            s_descr = &s_cfg_desc[43];
                            len     = 9;
                            break;
                        case USB_IF_RAW:
                            s_descr = &s_cfg_desc[68];
                            len     = 9;
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                        break;
                    case USB_DESCR_TYP_REPORT:
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case USB_IF_KBD:
                            s_descr = s_kbd_report;
                            len     = (uint8_t)sizeof(s_kbd_report);
                            break;
                        case USB_IF_MEDIA:
                            s_descr = s_media_report;
                            len     = (uint8_t)sizeof(s_media_report);
                            break;
                        case USB_IF_RAW:
                            s_descr = s_raw_report;
                            len     = (uint8_t)sizeof(s_raw_report);
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                        break;
                    case USB_DESCR_TYP_STRING:
                        switch (pSetupReqPak->wValue & 0xFFu) {
                        case 0:
                            s_descr = s_lang_desc;
                            len     = s_lang_desc[0];
                            break;
                        case 1:
                            s_descr = s_manu_desc;
                            len     = s_manu_desc[0];
                            break;
                        case 2:
                            s_descr = s_prod_desc;
                            len     = s_prod_desc[0];
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                        break;
                    default:
                        errflag = 0xFF;
                        break;
                    }
                    if (errflag != 0xFF) {
                        if (s_setup_req_len > len) {
                            s_setup_req_len = len;      /* 实际上传总长 */
                        }
                        len = (s_setup_req_len >= USB_DEV_EP0_SIZE)
                                  ? USB_DEV_EP0_SIZE
                                  : (uint8_t)s_setup_req_len;
                        memcpy(pEP0_DataBuf, s_descr, len);
                        s_descr += len;
                    }
                    break;

                case USB_SET_ADDRESS:
                    s_setup_req_len = pSetupReqPak->wValue & 0xFFu;
                    break;

                case USB_GET_CONFIGURATION:
                    pEP0_DataBuf[0] = s_dev_config;
                    if (s_setup_req_len > 1) {
                        s_setup_req_len = 1;
                    }
                    break;

                case USB_SET_CONFIGURATION:
                    s_dev_config = (uint8_t)(pSetupReqPak->wValue & 0xFFu);
                    break;

                case USB_CLEAR_FEATURE:
                    if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case 0x83:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK);
                            break;
                        case 0x03:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK);
                            break;
                        case 0x82:
                            R8_UEP2_CTRL = (uint8_t)((R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK);
                            break;
                        case 0x02:
                            R8_UEP2_CTRL = (uint8_t)((R8_UEP2_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK);
                            break;
                        case 0x81:
                            R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK);
                            break;
                        case 0x01:
                            R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK);
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                    } else if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE) {
                        /* CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)：撤销唤醒许可 */
                        if ((pSetupReqPak->wValue & 0xFFu) == 1u) {
                            s_usb_rmt_wake_armed = 0u;
                        }
                    }
                    break;

                case USB_SET_FEATURE:
                    if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                        switch (pSetupReqPak->wIndex & 0xFFu) {
                        case 0x83:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL);
                            break;
                        case 0x03:
                            R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_STALL);
                            break;
                        case 0x82:
                            R8_UEP2_CTRL = (uint8_t)((R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL);
                            break;
                        case 0x02:
                            R8_UEP2_CTRL = (uint8_t)((R8_UEP2_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_STALL);
                            break;
                        case 0x81:
                            R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL);
                            break;
                        case 0x01:
                            R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_STALL);
                            break;
                        default:
                            errflag = 0xFF;
                            break;
                        }
                    } else if ((pSetupReqPak->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE) {
                        /* SET_FEATURE(DEVICE_REMOTE_WAKEUP)：主机允许本设备唤醒它。
                         * 记录许可，供挂起期间收到 HID 报文时触发 resume。 */
                        if ((pSetupReqPak->wValue & 0xFFu) == 1u) {
                            s_usb_rmt_wake_armed = 1u;
                        }
                    }
                    break;

                case USB_GET_INTERFACE:
                    pEP0_DataBuf[0] = 0x00;
                    if (s_setup_req_len > 1) {
                        s_setup_req_len = 1;
                    }
                    break;

                case USB_SET_INTERFACE:
                    break;

                case USB_GET_STATUS:
                    pEP0_DataBuf[0] = 0x00;
                    pEP0_DataBuf[1] = 0x00;
                    if (s_setup_req_len > 2) {
                        s_setup_req_len = 2;
                    }
                    break;

                default:
                    errflag = 0xFF;
                    break;
                }
            }

            if (errflag == 0xFF) {
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            } else {
                if (chtype & 0x80) {
                    len = (s_setup_req_len > USB_DEV_EP0_SIZE)
                              ? USB_DEV_EP0_SIZE
                              : (uint8_t)s_setup_req_len;
                    s_setup_req_len -= len;
                } else {
                    len = 0;
                }
                R8_UEP0_T_LEN = len;
                R8_UEP0_CTRL  = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }

            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    } else if (intflag & RB_UIF_BUS_RST) {
        s_dev_config  = 0u;
        s_usb_rmt_wake_armed = 0u;      /* 总线复位清除远程唤醒许可（规格 9.4） */
        usb_hid_q_reset();
        R8_USB_DEV_AD = 0x00;
        R8_UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
        /* 手动翻转同步位（不置 RB_UEP_AUTO_TOG），与 ISR 中的 ^= RB_UEP?_TOG 配对。
         * 若同时置 AUTO_TOG，硬件与软件各翻一次，同步位不前进，主机会丢弃重复包。 */
        R8_UEP1_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP2_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP3_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    } else if (intflag & RB_UIF_SUSPEND) {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    } else {
        R8_USB_INT_FG = intflag;
    }
}

/* ============================ 厂商例程回调（端点 OUT） ============================ */

/* EP3 OUT：主机下发的 VIA Raw HID（32B）→ 经 2.4G 送键盘的 SK32。
 * 其中首 4 字节为 "OTA!" 的报文是 PC OTA 软件的「进 BOOT」指令（规格 10.1/10.2）：
 * Dongle 无 SK32，无法走 UART 的 CMD_ENTER_BOOT，故由 USB 侧直接触发。
 * 魔数取 ASCII "OTA!"：VIA 隧道报文首字节是 Report ID（常为 0x00），不会误命中。 */
#define BRIDGE_OTA_MAGIC_LEN   4u
#define BRIDGE_OTA_MAGIC0      0x4Fu   /* 'O' */
#define BRIDGE_OTA_MAGIC1      0x54u   /* 'T' */
#define BRIDGE_OTA_MAGIC2      0x41u   /* 'A' */
#define BRIDGE_OTA_MAGIC3      0x21u   /* '!'：保留的「进 ROM ISP」旧工具魔数 */

/* ---- HID OTA 子命令（SPEC §5.4，WebHID：OUT [O T A][SUB][ARGS≤28]）----
 * 方案A：**收包全程留在 App**，不中途复位进 IAP。原因：Chrome WebHID 在设备 USB
 * 复位重枚举后不再把设备交回 navigator.hid.getDevices()（实测长期返回 0），
 * 导致「等待设备进入 IAP 模式超时」。改为 App 自己收 FW_BEGIN/DATA/END 写 Stage，
 * FW_END 校验通过后置 IAP 标志并复位；常驻 BOOT 只做 Stage→APP_A 搬运。
 * 全程 USB 无断连，getDevices() 一直有效。
 *
 * 中断/主循环分工：本文件的 USB ISR 只**登记**请求（无 Flash 擦写的校验/应答可
 * 就地完成），真正的 4K 页擦/写与应答放在主循环 usb_ota_poll()——在 ISR 里擦
 * Flash 会长时间关中断，让主机 USB 传输超时。 */
#define BRIDGE_OTA_SUB_ENTER     0x01u
#define BRIDGE_OTA_SUB_FW_BEGIN  0x02u
#define BRIDGE_OTA_SUB_FW_DATA   0x03u
#define BRIDGE_OTA_SUB_FW_END    0x04u
#define BRIDGE_OTA_SUB_FW_ABORT  0x05u
#define BRIDGE_OTA_SUB_GET_INFO  0x06u
#define BRIDGE_OTA_MODE_APP      0x00u   /* GET_INFO.mode：0=App / 1=IAP */
#define BRIDGE_OTA_HID_CHUNK     24u     /* FW_DATA 单帧有效数据：32 − magic3 − sub1 − off4 */

/* IN 应答：[status u8][data...]，固定 32B（不足补 0，由 sdk_usb_tunnel_send 处理）。
 * status 为 0 表示 OK，非 0 为 BRIDGE_ERR_* 的 uint8 形式（0xF7=CRC）。 */
static void usb_ota_reply(uint8_t status, const uint8_t *data, uint8_t len)
{
    uint8_t buf[32];
    uint8_t n = 1u;

    memset(buf, 0, sizeof(buf));
    buf[0] = status;
    if (data != 0 && len != 0u) {
        if (len > (uint8_t)(sizeof(buf) - 1u)) {
            len = (uint8_t)(sizeof(buf) - 1u);
        }
        memcpy(&buf[1], data, len);
        n = (uint8_t)(1u + len);
    }
    (void)sdk_usb_tunnel_send(buf, n);
}

static void usb_ota_get_info(void)
{
    /* 前 7B 为既有布局（上位机按固定偏移解析），其后追加 2.4G 建链取证快照，
     * 供 2.4G 配对排障读取（Dongle 无独立日志出口）。总长 7+24=31 ≤ 32−1。 */
    uint8_t r[7 + 24];
    uint8_t n = 0u;

    r[0] = BRIDGE_OTA_MODE_APP;
    r[1] = (uint8_t)(BRIDGE_FW_REV & 0xFFu);
    r[2] = (uint8_t)(BRIDGE_FW_REV >> 8);
    r[3] = (uint8_t)(BRIDGE_USB_VID & 0xFFu);
    r[4] = (uint8_t)(BRIDGE_USB_VID >> 8);
    r[5] = (uint8_t)(BRIDGE_USB_PID & 0xFFu);
    r[6] = (uint8_t)(BRIDGE_USB_PID >> 8);
    if (sdk_rf24_diag(&r[7], &n) != BRIDGE_OK) {
        n = 0u;
    }
    usb_ota_reply(0u, r, (uint8_t)(7u + n));
}

/* 待落盘的 OTA 作业：ISR 登记 → 主循环 usb_ota_poll() 执行。
 * 上位机严格「发一帧等一应答」，故任一时刻至多一个作业在飞。 */
typedef enum { OTA_JOB_NONE = 0u, OTA_JOB_DATA, OTA_JOB_END, OTA_JOB_ABORT } ota_job_t;

static volatile uint8_t s_ota_job     = (uint8_t)OTA_JOB_NONE;
static uint8_t          s_ota_job_len = 0u;
static uint8_t          s_ota_job_buf[28];   /* FW_DATA 的 ARGS：offset(4) + data(≤24) */

/* 处理 "OTA"+SUB：返回 true 表示已识别并处理（不再当 VIA 隧道报文）。 */
static bool usb_ota_handle_sub(uint8_t sub, uint8_t l)
{
    switch (sub) {
    case BRIDGE_OTA_SUB_ENTER:
        /* 方案A：收包在 App 完成，无需复位进 IAP。仅松开残留按键。
         * **不应答**：上位机不等待 ENTER 的应答（见 hid_ota.js enterOta），若这里
         * 发一条 IN 报告，它会在主机侧滞留，被随后的 FW_BEGIN 请求当成应答读走
         * （chunk_max=0 → 误判 FW_BEGIN 被拒）。 */
        (void)bridge_hid_release_all();
        return true;

    case BRIDGE_OTA_SUB_GET_INFO:
        usb_ota_get_info();
        return true;

    case BRIDGE_OTA_SUB_FW_BEGIN: {
        int rc;

        /* 注意：HID OUT 报告固定 32B，l 恒为报告长度（USB_RX_LEN），不是「有效载荷
         * 长度」。FW_BEGIN 的参数就是前 6B（total4 + crc2），必须按 6 传入——
         * bridge_ota_begin 要求 len==6，用 l-4（=28）会被判 PARAM。 */
        if (l < 10u) {                      /* magic3 + sub1 + total4 + crc2 */
            usb_ota_reply((uint8_t)BRIDGE_ERR_PARAM, 0, 0u);
            return true;
        }
        /* 仅校验长度/CRC 格式并准备暂存区，无 Flash 擦写，可在 ISR 中完成 */
        rc = bridge_ota_begin(&pEP3_OUT_DataBuf[4], 6u);
        if (rc != BRIDGE_OK) {
            usb_ota_reply((uint8_t)rc, 0, 0u);   /* 负错误码 → uint8 补码，与上位机错误名表一致 */
            return true;
        }
        {
            uint8_t cm = (uint8_t)BRIDGE_OTA_HID_CHUNK;
            usb_ota_reply(0u, &cm, 1u);
        }
        return true;
    }

    case BRIDGE_OTA_SUB_FW_DATA:
        if (s_ota_job != (uint8_t)OTA_JOB_NONE) {
            usb_ota_reply((uint8_t)BRIDGE_ERR_BUSY, 0, 0u);   /* 上一帧尚未落盘：令上位机重试 */
            return true;
        }
        if (l < 8u) {                       /* off4 + data≥4 */
            usb_ota_reply((uint8_t)BRIDGE_ERR_PARAM, 0, 0u);
            return true;
        }
        memcpy(s_ota_job_buf, &pEP3_OUT_DataBuf[4], (uint8_t)(l - 4u));
        s_ota_job_len = (uint8_t)(l - 4u);
        s_ota_job     = (uint8_t)OTA_JOB_DATA;   /* 最后写：主循环见非 NONE 才读 buf/len */
        return true;                             /* 应答由 usb_ota_poll 在落盘后发出 */

    case BRIDGE_OTA_SUB_FW_END:
        if (s_ota_job != (uint8_t)OTA_JOB_NONE) {
            usb_ota_reply((uint8_t)BRIDGE_ERR_BUSY, 0, 0u);
            return true;
        }
        s_ota_job = (uint8_t)OTA_JOB_END;
        return true;

    case BRIDGE_OTA_SUB_FW_ABORT:
        if (s_ota_job != (uint8_t)OTA_JOB_NONE) {
            usb_ota_reply((uint8_t)BRIDGE_ERR_BUSY, 0, 0u);
            return true;
        }
        s_ota_job = (uint8_t)OTA_JOB_ABORT;
        return true;

    default:
        return false;
    }
}

/* ============================ App 侧 OTA 落盘（主循环调用） ============================
 * ISR 只登记作业，这里执行 Flash 擦写与应答。FW_END 校验通过后先应答、等主机真正
 * 取走，再 bridge_ota_apply()（写 IAP 标志 + 复位，不复返）；复位后常驻 BOOT 把
 * Stage 搬到 APP_A。 */
void usb_ota_poll(void)
{
    uint8_t job = s_ota_job;
    int     rc;

    if (job == (uint8_t)OTA_JOB_NONE) {
        return;
    }

    if (job == (uint8_t)OTA_JOB_DATA) {
        rc = bridge_ota_data(s_ota_job_buf, s_ota_job_len);
        s_ota_job = (uint8_t)OTA_JOB_NONE;
        if (rc == BRIDGE_OK) {
            usb_ota_reply(0u, 0, 0u);
        } else if (rc == BRIDGE_ERR_PARAM) {
            usb_ota_reply((uint8_t)BRIDGE_ERR_PARAM, 0, 0u);
        } else {
            usb_ota_reply((uint8_t)BRIDGE_ERR_STATE, 0, 0u);
        }
        return;
    }

    if (job == (uint8_t)OTA_JOB_ABORT) {
        bridge_ota_abort();
        s_ota_job = (uint8_t)OTA_JOB_NONE;
        usb_ota_reply(0u, 0, 0u);
        return;
    }

    /* OTA_JOB_END */
    rc = bridge_ota_end(0, 0u);
    if (rc != BRIDGE_OK) {
        s_ota_job = (uint8_t)OTA_JOB_NONE;
        usb_ota_reply((uint8_t)((rc == BRIDGE_ERR_CRC) ? BRIDGE_ERR_CRC : BRIDGE_ERR_STATE), 0, 0u);
        return;
    }

    usb_ota_reply(0u, 0, 0u);
    /* 等主机真正取走 EP3 IN 应答再复位，否则应答会被复位打断（上位机误判失败）。 */
    for (uint8_t i = 0u; i < 500u; i++) {
        if ((R8_UEP3_CTRL & MASK_UEP_T_RES) != UEP_T_RES_ACK) {
            break;                          /* 已被取走（ISR 置回 NAK） */
        }
        sdk_delay_ms(1u);
    }
    sdk_delay_ms(5u);
    bridge_ota_apply();     /* port_fw_finish → 写 IAP 标志 + 复位（不复返） */
}

void DevEP3_OUT_Deal(uint8_t l)
{
    if (l == 0u) {
        return;
    }

    if (l >= BRIDGE_OTA_MAGIC_LEN &&
        pEP3_OUT_DataBuf[0] == BRIDGE_OTA_MAGIC0 &&
        pEP3_OUT_DataBuf[1] == BRIDGE_OTA_MAGIC1 &&
        pEP3_OUT_DataBuf[2] == BRIDGE_OTA_MAGIC2) {
        uint8_t sub = pEP3_OUT_DataBuf[3];

        if (sub == BRIDGE_OTA_MAGIC3) {
            /* 旧 PC OTA 工具魔数 "OTA!"：释放按键 + 进 ROM ISP（不复返回）。 */
            (void)bridge_hid_release_all();
            port_enter_bootloader();
            return;
        }
        if (usb_ota_handle_sub(sub, l)) {
            return;
        }
        /* 未知 SUB：丢弃，避免半截命令被当 VIA 隧道报文误发到 2.4G */
        return;
    }

    (void)sdk_rf24_send_tunnel(pEP3_OUT_DataBuf, l);
}

void DevEP1_OUT_Deal(uint8_t l)
{
    (void)l;    /* 键盘接口未声明 OUT 端点 */
}

void DevEP2_OUT_Deal(uint8_t l)
{
    (void)l;    /* 辅助键接口未声明 OUT 端点 */
}

void DevEP4_OUT_Deal(uint8_t l)
{
    (void)l;    /* EP4 未使用 */
}

/* ============================ USB 中断向量 ============================ */
/* 厂商 BLE 库（LIBCH58xBLE.a）不占用 USB 向量，可安全定义（见 BLE_USB 例程） */
__INTERRUPT
__HIGH_CODE
void USB_IRQHandler(void)
{
    USB_DevTransProcess();
}

/* ============================ 对外接口 ============================ */

int sdk_usb_init(void)
{
    if (s_usb_inited) {
        return BRIDGE_OK;       /* 幂等 */
    }

    pEP0_RAM_Addr = s_ep0_buf;
    pEP1_RAM_Addr = s_ep1_buf;
    pEP2_RAM_Addr = s_ep2_buf;
    pEP3_RAM_Addr = s_ep3_buf;

    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);

    usb_hid_q_reset();
    s_dev_config          = 0u;
    s_usb_rmt_wake_armed  = 0u;
    s_wake_report_pending = false;
    s_usb_inited          = true;
    LOG_I(BRIDGE_MOD_USB, "usb device up vid=%04X pid=%04X", (unsigned)BRIDGE_USB_VID,
          (unsigned)BRIDGE_USB_PID);
    return BRIDGE_OK;
}

/* 按报文长度分流提交：键盘走 IF0/EP1（无 Report ID），其余走 IF1/EP2（带 Report ID）。
 * 长度取值见 bridge_types.h 的 BRIDGE_REPORT_*_LEN。
 * 一律**入队**（不直接覆写端点缓冲），由 usb_ep*_drain() 在端点空闲时提交，
 * 避免主机未取走时覆盖在飞报文。 */
static int usb_hid_push(const uint8_t *d, uint8_t len)
{
    switch (len) {
    case BRIDGE_REPORT_KEYBOARD_LEN:        /* 8B */
        usb_ep1_push(d);
        usb_ep1_drain();
        return BRIDGE_OK;

    case BRIDGE_REPORT_MOUSE_LEN:           /* 4B */
        usb_ep2_push(USB_RID_MOUSE, d, BRIDGE_REPORT_MOUSE_LEN);
        usb_ep2_drain();
        return BRIDGE_OK;

    case BRIDGE_REPORT_SYSTEM_LEN:          /* 1B */
        usb_ep2_push(USB_RID_SYSTEM, d, BRIDGE_REPORT_SYSTEM_LEN);
        usb_ep2_drain();
        return BRIDGE_OK;

    case BRIDGE_REPORT_CONSUMER_LEN:        /* 2B */
        usb_ep2_push(USB_RID_CONSUMER, d, BRIDGE_REPORT_CONSUMER_LEN);
        usb_ep2_drain();
        return BRIDGE_OK;

    case BRIDGE_REPORT_NKRO_LEN:            /* 16B */
        usb_ep2_push(USB_RID_NKRO, d, BRIDGE_REPORT_NKRO_LEN);
        usb_ep2_drain();
        return BRIDGE_OK;

    default:
        return BRIDGE_ERR_PARAM;
    }
}

/* 主机重新枚举完成后，补发唤醒前暂存的那条报文（保持"先按后放"的按键顺序）。 */
static void usb_wake_report_flush(void)
{
    if (!s_wake_report_pending) {
        return;
    }
    if (s_dev_config == 0u || sdk_usb_is_suspended()) {
        return;                     /* 仍在重枚举 / 挂起：继续等 */
    }
    s_wake_report_pending = false;
    (void)usb_hid_push(s_wake_report, s_wake_report_len);
}

int sdk_usb_hid_send(const uint8_t *d, uint8_t len)
{
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }

    /* 挂起期间不得发送报文（USB-CV）。若主机已允许远程唤醒，则触发 resume，
     * 并把这条报文暂存，等主机重新枚举完成后由 sdk_usb_poll() 补发。 */
    if (sdk_usb_is_suspended()) {
        if (s_usb_rmt_wake_armed != 0u) {
            if (len <= (uint8_t)sizeof(s_wake_report)) {
                memcpy(s_wake_report, d, len);
                s_wake_report_len     = len;
                s_wake_report_pending = true;
            }
            s_usb_rmt_wake_armed = 0u;      /* 一次挂起周期只唤醒一次 */
            (void)sdk_usb_remote_wakeup();
            LOG_I(BRIDGE_MOD_USB, "remote wakeup (len=%u)", (unsigned)len);
            return BRIDGE_OK;
        }
        return BRIDGE_ERR_STATE;    /* 未获唤醒许可：挂起期间一律不发 */
    }

    if (s_dev_config == 0u) {
        return BRIDGE_ERR_STATE;    /* 未枚举完成 */
    }

    usb_wake_report_flush();        /* 先补发唤醒前报文，保证按键顺序 */
    return usb_hid_push(d, len);
}

bool sdk_usb_is_configured(void)
{
    return (s_dev_config != 0u);
}

bool sdk_usb_is_suspended(void)
{
    return (R8_USB_MIS_ST & RB_UMS_SUSPEND) != 0u;
}

bool sdk_usb_remote_wakeup_enabled(void)
{
    return (s_usb_rmt_wake_armed != 0u);
}

/* USB 设备模式远程唤醒（CH58x 全速 USB，与厂商 HID_CompliantDev 例程 DevWakeup 一致）：
 * 短暂断开并按低速重挂 D+ 上拉，主机据此退出挂起并重新枚举本设备。
 * 仅在总线确实处于挂起态时调用；进入后阻塞约 2ms（须在任务上下文调用）。 */
int sdk_usb_remote_wakeup(void)
{
    if (!sdk_usb_is_suspended()) {
        return BRIDGE_ERR_STATE;
    }
    R16_PIN_ANALOG_IE = (uint16_t)(R16_PIN_ANALOG_IE & ~RB_PIN_USB_DP_PU);
    R8_UDEV_CTRL |= RB_UD_LOW_SPEED;
    sdk_delay_ms(2u);
    R8_UDEV_CTRL = (uint8_t)(R8_UDEV_CTRL & ~RB_UD_LOW_SPEED);
    R16_PIN_ANALOG_IE |= RB_PIN_USB_DP_PU;
    return BRIDGE_OK;
}

/* 主循环每轮调用：把唤醒时暂存的报文在重枚举完成后补发；并执行 App 侧 OTA 落盘。 */
void sdk_usb_poll(void)
{
    usb_wake_report_flush();
    usb_ota_poll();

    /* 主机挂起/恢复边沿 → 经 2.4G 下发键盘（见 BRIDGE_RADIO_PKT_HOST），
     * 键盘侧据此更新主机电源状态并上报 SK32。 */
    bool susp = sdk_usb_is_suspended();
    if (susp != s_host_suspended_prev) {
        s_host_suspended_prev = susp;
        sdk_usb_on_host_power(susp);
    }
}

int sdk_usb_tunnel_send(const uint8_t *d, uint8_t len)
{
    /* VIA Raw HID 报文固定 32B（QMK RAW_EPSIZE），不足补 0 */
    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_dev_config == 0u) {
        return BRIDGE_ERR_STATE;
    }
    if (len > 32u) {
        return BRIDGE_ERR_PARAM;    /* 超出单包容量：VIA 不使用 >32B */
    }
    if ((R8_UEP3_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) {
        return BRIDGE_ERR_IO;       /* 上一包尚未被主机取走 */
    }

    memset(pEP3_IN_DataBuf, 0, 32u);
    memcpy(pEP3_IN_DataBuf, d, len);
    DevEP3_IN_Deal(32u);
    return BRIDGE_OK;
}

/* ============================ 运行时改 USB 标识（CMD_SET_NAME / CMD_SET_USB_ID） ============================ */

/* 按 USB 字符串描述符格式重建产品名：ASCII → UTF-16LE */
static void usb_build_prod_desc(const uint8_t *name, uint8_t len)
{
    uint8_t i;

    if (len > BRIDGE_NAME_MAX) {
        len = BRIDGE_NAME_MAX;
    }
    s_prod_desc[0] = (uint8_t)(2u + ((uint16_t)len * 2u));
    s_prod_desc[1] = 0x03u;                 /* STRING 描述符 */
    for (i = 0u; i < len; i++) {
        s_prod_desc[2u + ((uint16_t)i * 2u)]      = name[i];
        s_prod_desc[2u + ((uint16_t)i * 2u) + 1u] = 0x00u;
    }
}

/* 触发 USB 重新枚举：先断开 D+ 上拉让主机检测到"拔除"，延时后重新挂载。
 * 描述符改动必须重枚举才对主机生效（主机缓存了旧描述符）。
 * 本函数阻塞约 200ms，仅在配置类命令 / 上电应用时调用，不在 HID 数据面路径。 */
static void usb_reenumerate(void)
{
    R8_USB_CTRL      &= (uint8_t)~RB_UC_DEV_PU_EN;
    R16_PIN_ANALOG_IE = (uint16_t)(R16_PIN_ANALOG_IE & ~RB_PIN_USB_DP_PU);
    usb_hid_q_reset();
    s_dev_config          = 0u;
    s_wake_report_pending = false;          /* 主动重枚举：作废旧唤醒报文 */
    sdk_delay_ms(200u);
    USB_DeviceInit();                       /* 重新挂载并复位端点 / 中断标志 */
}

int sdk_usb_set_product_name(const uint8_t *name, uint8_t len)
{
    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    usb_build_prod_desc(name, len);
    usb_reenumerate();
    LOG_I(BRIDGE_MOD_USB, "product name updated (%u B)", (unsigned)len);
    return BRIDGE_OK;
}

int sdk_usb_set_ids(uint16_t vid, uint16_t pid)
{
    s_dev_desc[8]  = (uint8_t)(vid & 0xFFu);
    s_dev_desc[9]  = (uint8_t)(vid >> 8);
    s_dev_desc[10] = (uint8_t)(pid & 0xFFu);
    s_dev_desc[11] = (uint8_t)(pid >> 8);
    usb_reenumerate();
    LOG_I(BRIDGE_MOD_USB, "ids updated vid=%04X pid=%04X", (unsigned)vid, (unsigned)pid);
    return BRIDGE_OK;
}

#else  /* !BRIDGE_BOARD_HAS_USB —— 键盘侧不使用 CH58x 的 USB */

int sdk_usb_init(void)
{
    return BRIDGE_OK;
}

int sdk_usb_hid_send(const uint8_t *d, uint8_t len)
{
    (void)d;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

bool sdk_usb_is_configured(void)
{
    return false;
}

bool sdk_usb_is_suspended(void)
{
    return false;
}

bool sdk_usb_remote_wakeup_enabled(void)
{
    return false;
}

int sdk_usb_remote_wakeup(void)
{
    return BRIDGE_ERR_UNSUPPORTED;
}

void sdk_usb_poll(void)
{
}

int sdk_usb_tunnel_send(const uint8_t *d, uint8_t len)
{
    (void)d;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int sdk_usb_set_product_name(const uint8_t *name, uint8_t len)
{
    (void)name;
    (void)len;
    return BRIDGE_ERR_UNSUPPORTED;
}

int sdk_usb_set_ids(uint16_t vid, uint16_t pid)
{
    (void)vid;
    (void)pid;
    return BRIDGE_ERR_UNSUPPORTED;
}

#endif /* BRIDGE_BOARD_HAS_USB */
