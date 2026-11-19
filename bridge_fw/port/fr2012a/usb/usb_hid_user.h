/*
  ******************************************************************************
  * @file    usb_hid.h
  * @author  FreqChip Firmware Team
  * @version V1.0.0
  * @date    2021
  * @brief   Header file for usb_hid.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 FreqChip.
  * All rights reserved.
  ******************************************************************************
*/
#ifndef __USB_HID_USER_H__
#define __USB_HID_USER_H__

#include "stdint.h"
#include "stdbool.h"
#include "string.h"

#include "usb_core.h"
#include "usb_dev.h"

#define KEY_LEFT_CONTROL    (0x1001)
#define KEY_LEFT_SHIFT      (0x1002)
#define KEY_LEFT_ALT        (0x1004)
#define KEY_LEFT_GUI        (0x1008)
#define KEY_RIGHT_CONTROL   (0x1010)
#define KEY_RIGHT_SHIFT     (0x1020)
#define KEY_RIGHT_ALT       (0x1040)
#define KEY_RIGHT_GUI       (0x1080)

/*------------------------------------------------------------------*/
/*  HID Class-Specific Requests                                     */
/*------------------------------------------------------------------*/
#define HID_GET_REPORT      (0x01)
#define HID_GET_IDLE        (0x02)
#define HID_GET_PROTOCOL    (0x03)
#define HID_SET_REPORT      (0x09)
#define HID_SET_IDLE        (0x0A)
#define HID_SET_PROTOCOL    (0x0B)

/* Exported inline functions --------------------------------------------------------*/


/* usb_hid_set_mouse_report */
void usb_hid_set_mouse_report(uint8_t fu8_Index, uint8_t fu8_Value);

/* usb_hid_set_keyboard_report */
void usb_hid_set_keyboard_report(uint16_t fu16_Key);

/* usb_hid_clear_keyboard_report */
void usb_hid_clear_keyboard_report(uint16_t fu16_Key);

/* usb_hid_send_mouse_report */
void usb_hid_send_mouse_report(void);

/* usb_hid_send_keyboard_report */
void usb_hid_send_keyboard_report(void);
void usb_hid_send_custom_report1(uint8_t *report);


/* usb_hid_init */ 
void usb_hid_init(void);

/* 主机 LED 状态（HID SET_REPORT，接收器转发到 BLE 侧） */
uint8_t usb_hid_led_state(void);
uint8_t usb_hid_take_led_change(void);

#define REPORT0_LENGTH    (8)
#define REPORT1_LENGTH    (8)
#define REPORT2_LENGTH    (0x40)
#define REPORT3_LENGTH    (5)
#define REPORT4_LENGTH    (5)

#define	MOUSE_RID			        7 
#define KB_RID					      8
#define ACPI_RID					    1
#define CONSUMER_RID					2
#define VENDOR_RID		        3

extern uint8_t HID_Mouse_Report[REPORT0_LENGTH];

#endif
