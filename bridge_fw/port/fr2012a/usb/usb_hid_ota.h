#ifndef _USB_HID_OTA_H
#define _USB_HID_OTA_H

#include <stdint.h>
enum USB_HID_OTA_event_t {
    USB_HID_OTA_REC,
    USB_TASK_TEST,

};

extern uint16_t usb_hid_ota_task_id;


void usb_hid_ota_task_init(void);

void usb_hid_otas_recv_data(uint8_t *p_data);
#endif

