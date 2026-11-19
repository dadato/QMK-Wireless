/* FR2012A 接收器 USB HID 适配层实现
 *
 * 报表映射（厂商描述符，见 usb/usb_hid_user.h）：
 *   8B 键盘(6KRO) → RID 8，展开成 15B NKRO 位图（EP2）
 *   2B 多媒体     → RID 2 + 2B（EP2）
 *   1B 系统控制   → RID 1 + 1B（EP2）
 *   4B 鼠标       → 未实现（BLE 侧无鼠标通路，与 CH58x 一致）
 */
#include <string.h>

#include "plf.h"
#include "driver_system.h"
#include "driver_pmu.h"
#include "driver_gpio.h"

#include "usb_core.h"
#include "usb_dev.h"
#include "usb_hid_user.h"

#include "bridge_config.h"
#include "bridge_cfg.h"
#include "bridge_types.h"
#include "bridge_log.h"

#include "bridge_usb_fr2012a.h"
#include "bridge_ble_fr2012a.h"
#include "bridge_fr2012a_sdk.h"

#define BRIDGE_MOD_USB "usb"

/* 厂商 usb_hid_user.c 里的定长发送接口（其头文件未声明） */
void usb_hid_send_keyboard_report1(uint8_t *report, uint8_t len);

/* 厂商默认设备描述符（usb_hid_user.c）；本层复制成可写缓冲后再注册回去 */
extern const uint8_t USB_HID_DeviceDesc[];

#define FR_USB_DEV_DESC_LEN   18u
#define FR_USB_NAME_MAX       BRIDGE_NAME_MAX   /* 21 字节 → 44B 字符串描述符 */

/* 设备描述符偏移：idVendor@8、idProduct@10（USB 2.0 定义） */
#define FR_USB_OFF_VID        8u
#define FR_USB_OFF_PID        10u

static uint8_t s_dev_desc[FR_USB_DEV_DESC_LEN];
static uint8_t s_product_desc[2u + (2u * FR_USB_NAME_MAX)];
static volatile bool s_usb_suspended;
static bool s_last_suspend;

/* 产品名串描述符：UTF-16LE，bLength = 2 + 2*len */
static void usb_set_product_name(const uint8_t *name, uint8_t len)
{
    if (len > FR_USB_NAME_MAX) {
        len = FR_USB_NAME_MAX;
    }
    s_product_desc[0] = (uint8_t)(2u + (len * 2u));
    s_product_desc[1] = 0x03;
    for (uint8_t i = 0u; i < len; i++) {
        s_product_desc[2u + (i * 2u)]      = name[i];
        s_product_desc[3u + (i * 2u)]      = 0x00;
    }
    usbdev_get_string_Product(s_product_desc);
}

static void usb_apply_ids(uint16_t vid, uint16_t pid)
{
    s_dev_desc[FR_USB_OFF_VID]     = (uint8_t)(vid & 0xFFu);
    s_dev_desc[FR_USB_OFF_VID + 1] = (uint8_t)((vid >> 8) & 0xFFu);
    s_dev_desc[FR_USB_OFF_PID]     = (uint8_t)(pid & 0xFFu);
    s_dev_desc[FR_USB_OFF_PID + 1] = (uint8_t)((pid >> 8) & 0xFFu);
    usbdev_get_dev_desc(s_dev_desc);
}

/* 拉低再拉高 D+ 上拉，让主机重新读取描述符（厂商 usb_core 提供原语） */
static void usb_reenumerate(void)
{
    usb_DP_Pullup_Disable();
    sdk_delay_ms(100u);
    usb_DP_Pullup_Enable();
}

/* 总线挂起/恢复：新 SDK 通过回调通知，不再有 f_Suspend 全局 */
static void usb_suspend_handler(void)
{
    s_usb_suspended = true;
}

static void usb_resume_handler(void)
{
    s_usb_suspended = false;
}

void bridge_usb_fr2012a_init(void)
{
    const bridge_cfg_t *cfg;

    __SYSTEM_USB_CLK_ENABLE();
    __SYSTEM_GPIO_CLK_ENABLE();

    NVIC_ClearPendingIRQ(USBMCU_IRQn);
    NVIC_SetPriority(USBMCU_IRQn, 1u);
    NVIC_EnableIRQ(USBMCU_IRQn);

    /* 复用 PA4/PA5 为 USB DP/DM，并打开 USB pad */
    pmu_usb_pad_ctrl(1);
    system_set_port_mux(GPIO_PORT_A, GPIO_BIT_4, PORTA4_FUNC_USB_DP);
    system_set_port_mux(GPIO_PORT_A, GPIO_BIT_5, PORTA5_FUNC_USB_DM);

    usb_device_init();
    usb_hid_init();             /* 先按厂商默认描述符完成注册 */

    /* 新 SDK 通过回调通知总线挂起/恢复 */
    USB_Suspend_Handler = usb_suspend_handler;
    USB_Resume_Handler  = usb_resume_handler;

    /* 换成可写副本，再套用 NV 中的标识（SK32 经 CMD_SET_USB_ID/CMD_SET_NAME 写入） */
    memcpy(s_dev_desc, USB_HID_DeviceDesc, sizeof(s_dev_desc));
    cfg = bridge_cfg_get();
    usb_apply_ids(cfg->usb_vid, cfg->usb_pid);
    usb_set_product_name((const uint8_t *)bridge_cfg_usb_name(),
                         bridge_cfg_usb_name_len());

    usb_DP_Pullup_Enable();

    LOG_I(BRIDGE_MOD_USB, "usb hid up vid=%04X pid=%04X",
          (unsigned)cfg->usb_vid, (unsigned)cfg->usb_pid);
}

int bridge_usb_fr2012a_set_ids(uint16_t vid, uint16_t pid)
{
    if ((vid == 0u) || (pid == 0u)) {
        return BRIDGE_ERR_PARAM;
    }
    usb_apply_ids(vid, pid);
    usb_reenumerate();
    LOG_I(BRIDGE_MOD_USB, "usb ids applied vid=%04X pid=%04X",
          (unsigned)vid, (unsigned)pid);
    return BRIDGE_OK;
}

int bridge_usb_fr2012a_set_name(const uint8_t *name, uint8_t len)
{
    if ((name == NULL) || (len == 0u)) {
        return BRIDGE_ERR_PARAM;
    }
    usb_set_product_name(name, len);
    usb_reenumerate();
    LOG_I(BRIDGE_MOD_USB, "usb product name applied (%uB)", (unsigned)len);
    return BRIDGE_OK;
}

/* 6KRO（mod, rsvd, key[6]）→ RID8 的 15B NKRO 位图 */
static void usb_pack_nkro(const uint8_t *in, uint8_t out[16])
{
    memset(out, 0, 16u);
    out[0] = (uint8_t)KB_RID;
    out[1] = in[0];                     /* 修饰键 0xE0..0xE7 → bit0..7 */

    for (uint8_t i = 2u; i < BRIDGE_REPORT_KEYBOARD_LEN; i++) {
        uint8_t kc = in[i];

        if ((kc >= 4u) && (kc <= 0x67u)) {
            uint8_t bit = (uint8_t)(8u + kc);   /* usage 0x00..0x67 → bit8..111 */

            out[1u + (bit / 8u)] |= (uint8_t)(1u << (bit % 8u));
        }
    }
}

int bridge_usb_fr2012a_send(const uint8_t *payload, uint8_t len)
{
    static uint8_t s_report[16];

    if (payload == NULL || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }

    switch (len) {
    case BRIDGE_REPORT_KEYBOARD_LEN:
        usb_pack_nkro(payload, s_report);
        usb_hid_send_keyboard_report1(s_report, 16u);
        return BRIDGE_OK;

    case BRIDGE_REPORT_CONSUMER_LEN:
        s_report[0] = (uint8_t)CONSUMER_RID;
        s_report[1] = payload[0];
        s_report[2] = payload[1];
        usb_hid_send_keyboard_report1(s_report, 3u);
        return BRIDGE_OK;

    case BRIDGE_REPORT_SYSTEM_LEN:
        s_report[0] = (uint8_t)ACPI_RID;
        s_report[1] = payload[0];
        usb_hid_send_keyboard_report1(s_report, 2u);
        return BRIDGE_OK;

    default:
        return BRIDGE_ERR_UNSUPPORTED;
    }
}

void bridge_usb_fr2012a_poll(void)
{
    if (usb_hid_take_led_change() != 0u) {
        (void)bridge_fr_ble_send_led(usb_hid_led_state());
    }

    if (s_last_suspend != s_usb_suspended) {
        s_last_suspend = s_usb_suspended;
        (void)bridge_fr_ble_send_host_power(s_usb_suspended ? 1u : 0u);
    }
}
