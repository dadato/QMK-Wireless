/* FR2012A 接收器（Dongle）USB HID 适配层
 *
 * 厂商 USB 设备栈（port/fr2012a/usb/）是复合 HID：
 *   EP1 鼠标 / EP2 键盘(NKRO 位图) / EP3 vendor 64B / EP4 透传 64B
 * 本层做三件事：
 *   1. 把桥链路收到的 HID 报文按报表 ID 打包送 EP2；
 *   2. 把主机下发的 LED / 挂起状态经桥链路回传给键盘模组；
 *   3. 运行时替换 USB 标识（VID/PID + 产品名串）并重枚举。
 *
 * 标识来源：core 的 bridge_cfg（由 SK32 的 CMD_SET_USB_ID / CMD_SET_NAME 写入 NV）。
 * 厂商 USB 栈用 usbdev_get_dev_desc()/usbdev_get_string_Product() 注册描述符指针，
 * 因此这里只需换成自己的可写缓冲，再拉低/拉高 D+ 上拉触发重新枚举。
 */
#ifndef BRIDGE_USB_FR2012A_H
#define BRIDGE_USB_FR2012A_H

#include <stdint.h>

void bridge_usb_fr2012a_init(void);
void bridge_usb_fr2012a_poll(void);

/* 键盘模组上报的 HID 报文 → USB 主机 */
int  bridge_usb_fr2012a_send(const uint8_t *payload, uint8_t len);

/* 运行时改 USB 标识（NV 已由 core 落盘）：立即生效并重新枚举 */
int  bridge_usb_fr2012a_set_ids(uint16_t vid, uint16_t pid);
int  bridge_usb_fr2012a_set_name(const uint8_t *name, uint8_t len);

#endif /* BRIDGE_USB_FR2012A_H */
