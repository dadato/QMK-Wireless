/*
  ******************************************************************************
  * @file    usb_hid.c
  * @author  FreqChip Firmware Team
  * @version V1.0.0
  * @date    2021
  * @brief   This file provides the high layer firmware functions to manage the 
  *          USB HID Device.(Human Interface Device)
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 FreqChip.
  * All rights reserved.
  * 
  ******************************************************************************
  * How to use, for example:
  * 
  *    int main(void)
  *    {
  *        NVIC_ClearPendingIRQ(USBMCU_IRQn);
  *        NVIC_SetPriority(USBMCU_IRQn, 0);
  *        NVIC_EnableIRQ(USBMCU_IRQn);
  *
  *        usb_device_init();
  *        usb_hid_init();
  *        
  *        // Wait for other initialization of the MCU
  * 
  *        usb_DP_Pullup_Enable();
  *
  *        while(1)
  *        {
  *            usb_hid_send_report();
  *        }
  *    }
  ******************************************************************************
*/
#include "usb_hid_user.h"
#include "usb_hid_ota.h"
#include "driver_uart.h"
#include "sys_utils.h"

#include "os_task.h"


uint8_t HID_Mouse_Report[REPORT0_LENGTH];
uint8_t HID_Keyboard_Report[REPORT1_LENGTH];

/* 主机 LED 状态（HID SET_REPORT，键盘 RID=8 的 1B 输出报表）。
 * 由接收器 poll 周期取走并通过 BLE 链路转发给键盘模组。 */
static volatile uint8_t s_led_state;
static volatile uint8_t s_led_changed;

uint8_t usb_hid_led_state(void)
{
    return s_led_state;
}

uint8_t usb_hid_take_led_change(void)
{
    uint8_t changed = s_led_changed;

    s_led_changed = 0u;
    return changed;
}

/* USB Standard Device Descriptor */
const uint8_t USB_HID_DeviceDesc[] =
{
    0x12,    /* bLength */
    0x01,    /* bDescriptorType */
    0x00,    /* bcdUSB */
    0x02,
    0x00,    /* bDeviceClass */
    0x00,    /* bDeviceSubClass */
    0x00,    /* bDeviceProtocol */
    0x40,    /* bMaxPacketSize */
    0x81,    /* idVendor */
    0x2F,    /* idVendor */
    0x08,    /* idProduct */
    0x21,    /* idProduct */
    0x00,    /* bcdDevice rel. 2.00 */
    0x20,
    0x01,    /* Index of manufacturer string */
    0x02,    /* Index of product string */
    0x00,    /* Index of serial number string */
    0x01,    /* bNumConfigurations */
};


/* HID device report Descriptor */
uint8_t USB_HID_Mouse_ReportDesc[] = 
{
//  36*2-1,          /* Length */
//	0x05,0x01,			// Usage Page (Generic Desktop Control)
//	0x09,0x02,			// Usage (Mouse)
//	0xA1,0x01,			// Collection (Application)
//	0x09,0x01,			// Usage (Pointer)
//	0xA1,0x00,			// Collection (Physical)
//	0x05,0x09,			// Usage Page (Button)
//	0x15,0x00,			// Logical Minimum (0)
//	0x25,0x01,			// Logical Maximum (1)
//	0x19,0x01,			// Usage Minimum (1)
//	0x29,0x05,			// Usage Maximum (5)
//	0x75,0x01,			// Report Size (1)
//	0x95,0x05,			// Report Count (5)
//	0x81,0x02,			// Input (Data, Variable, Absolute)
//	0x95,0x03,			// Report Count (3)
//	0x81,0x01,			// Input (Constant)
//	0x05,0x01,			// Usage Page (Generic Desktop Control)
//	0x16,0x00,0x81,		// Logical Minimum (-32768) LSB first
//	0x26,0xff,0x7f,		// Logical Maximum (32767)
//	0x09,0x30,			// Usage (X)
//	0x09,0x31,			// Usage (Y)
//	0x75,0x10,			// Report Size (16)	//4BYTE
//	0x95,0x02,			// Report Count (2)
//	0x81,0x06,			// Input (Data, Variable, Relative)
//	0x15,0x80,			// Logical Minimum (-128)
//	0x25,0x7F,			// Logical Maximum (127)
//	0x09,0x38,			// Usage (Wheel)
//	0x75,0x08,			// Report Size (8)
//	0x95,0x01,			// Report Count (1)
//	0x81,0x06,			// Input (Data, Variable, Relative)
//	//--------------------Horizontal wheel-----------------------//
//	0x05,0x0c,			// Usage Page (Consumer Devices)
//	0x0a,0x38,0x02,		// Usage (AC Pan)
//	0x95,0x01,			// Report Count (1)
//	0x81,0x06,			// Input (Data, Variable, Relative)
//	//--------------------Horizontal wheel-----------------------//	
//	0xC0,				// End Collection
//	0xC0,				// End Collection
	//	KEYBORAD REPORT DESCRIPTOR
	34*2-1,          /* Length */
		 0x05, 0x01, 		// USAGE_PAGE (Generic Desktop)
		 0x09, 0x06, 		// USAGE (Keyboard)
		 0xa1, 0x01, 		// COLLECTION (Application)
		 0x05, 0x07, 		//     USAGE_PAGE (Keyboard/Keypad)
		 0x19, 0xe0, 		//     USAGE_MINIMUM (L_Ctrl,L_Shift,L_Alt,L_Gui,R_Ctrl,R_Shift,R_Alt,R_Gui)
		 0x29, 0xe7, 		//     USAGE_MAXIMUM (Keyboard Right GUI)
		 0x15, 0x00, 		//     LOGICAL_MINIMUM (0)
		 0x25, 0x01, 		//     LOGICAL_MAXIMUM (1)
		 0x95, 0x08, 		//     REPORT_COUNT (8)
		 0x75, 0x01, 		//     REPORT_SIZE (1)
		 0x81, 0x02, 		//     INPUT (Data,Var,Abs)
		
		 0x95, 0x01, 		//     REPORT_COUNT (1)
		 0x75, 0x08, 		//     REPORT_SIZE (8)
		 0x81, 0x03, 		//     INPUT (Cnst,Var,Abs)
		 	 
		 0x95, 0x06, 		//   REPORT_COUNT (6)
		 0x75, 0x08, 		//   REPORT_SIZE (8)
		 0x15, 0x00, 		//   LOGICAL_MINIMUM (0)
		 0x26, 0xff, 0x00, 	//   LOGICAL_MAXIMUM (255)
		 0x05, 0x07, 		//   USAGE_PAGE (Keyboard/Keypad)
		 0x19, 0x00, 		//   USAGE_MINIMUM (Reserved (no event indicated))
		 0x2a, 0xff, 0x00, 	//   USAGE_MAXIMUM (Keyboard Application)
		 0x81, 0x00, 		//     INPUT (Data,Ary,Abs)
		 
		 0x25, 0x01, 		//   LOGICAL_MAXIMUM (1)
		 0x95, 0x05, 		//   REPORT_COUNT (5)
		 0x75, 0x01, 		//   REPORT_SIZE (1)
		 0x05, 0x08, 		//   USAGE_PAGE (LEDs)
		 0x19, 0x01,		//   USAGE_MINIMUM (Num Lock)
		 0x29, 0x05,		//   USAGE_MAXIMUM (Kana)
		 0x91, 0x02, 		//   OUTPUT (Data,Var,Abs)
		 0x95, 0x01, 		//   REPORT_COUNT (1)
		 0x75, 0x03, 		//   REPORT_SIZE (3)
		 0x91, 0x03, 		//   OUTPUT (Cnst,Var,Abs)
		         
		 0xc0        		//   END_COLLECTION	
};

/* HID device report Descriptor */
uint8_t USB_HID_Keyboard_ReportDesc[] = 
{
  88*2-1,          /* Length */
	
	0x05,0x01,			// Usage Page (Generic Desktop Control)
	0x09,0x02,			// Usage (Mouse)
	0xA1,0x01,			// Collection (Application)
  0x85,MOUSE_RID,     //DW
	0x09,0x01,			// Usage (Pointer)
	0xA1,0x00,			// Collection (Physical)
	0x05,0x09,			// Usage Page (Button)
	0x15,0x00,			// Logical Minimum (0)
	0x25,0x01,			// Logical Maximum (1)
	0x19,0x01,			// Usage Minimum (1)
	0x29,0x05,			// Usage Maximum (5)
	0x75,0x01,			// Report Size (1)
	0x95,0x05,			// Report Count (5)
	0x81,0x02,			// Input (Data, Variable, Absolute)
	0x95,0x03,			// Report Count (3)
	0x81,0x01,			// Input (Constant)
	0x05,0x01,			// Usage Page (Generic Desktop Control)
	0x16,0x00,0x81,		// Logical Minimum (-32768) LSB first
	0x26,0xff,0x7f,		// Logical Maximum (32767)
	0x09,0x30,			// Usage (X)
	0x09,0x31,			// Usage (Y)
	0x75,0x10,			// Report Size (16)	//4BYTE
	0x95,0x02,			// Report Count (2)
	0x81,0x06,			// Input (Data, Variable, Relative)

	0x15,0x80,			// Logical Minimum (-128)
	0x25,0x7F,			// Logical Maximum (127)
	0x09,0x38,			// Usage (Wheel)
	0x75,0x08,			// Report Size (8)
	0x95,0x01,			// Report Count (1)
	0x81,0x06,			// Input (Data, Variable, Relative)
//	
//	//--------------------Horizontal wheel-----------------------//
//	0x05,0x0c,			// Usage Page (Consumer Devices)
//	0x0a,0x38,0x02,		// Usage (AC Pan)
//	0x95,0x01,			// Report Count (1)
//	0x81,0x06,			// Input (Data, Variable, Relative)

	//--------------------Horizontal wheel-----------------------//	
	0xC0,				// End Collection
	0xC0,				// End Collection

//ȫ���޳�ͻ����
		0x05, 0x01, 
		0x09, 0x06,  	
		0xa1, 0x01, 
		0x85, KB_RID,  	 //ID        
		0x15,0x00,					//Log Min (0)
		0x25,0x01,					//Log Max (1)
		0x75,0x01,					//Report Size (1)
		0x95,0x70,					//Report Count (112) 
		0x05,0x07,					//Usage Pg (Key Codes)
		0x19,0xe0,					//Usage Minimum(0xE0 )
		0x29,0xe7,					//Usage Max (0xE7) 
		0x19,0x00,					//Usage Minimum(0x0 )
		0x29,0x67,					//Usage Maximum(0x67 ) 
		0x81,0x02,					//Input(Data, Variable, Absolute)  
		0x95,0x08,					//Report Count(0x08 ) 
		0x81,0x01,					//Input: (Data, Variable, Absolute)
		0xc0,
//   0x05,0x01,       //DW        00105H                 ;Usage Page (Generic Desktop)=05 01
//   0x09,0x06,       //DW				00609H                 ;Usage (Keyboard)=09 06
//   0xA1,0x01,       //DW        001A1H                 ;Collection (Application)=A1 01
//   0x85,KB_RID,     //DW        (KEYB_REPORT_ID SHL 8) + 85H     ;Report ID=85 01
//   //;-------- modify key
//   0x05,0x07,       //DW        00705H                 ;Usage Page (Key Codes)=05 07
//   0x19,0xE0,       //DW        0E019H                 ;Usage Minimum (224)=19 E0
//   0x29,0xE7,       //DW        0E729H                 ;Usage Maximum (231)=29 E7
//   0x15,0x00,       //DW        00015H                 ;Logical Minimum (0)=15 00
//   0x25,0x01,       //DW        00125H                 ;Logical Maximum (1)=25 01
//   0x75,0x01,       //DW        00175H                 ;Report Size (1)=75 01
//   0x95,0x08,       //DW        00895H                 ;Report Count (8)=95 08
//   0x81,0x02,       //DW        00281H                 ;Input (Data, Variable, Absolute)=81 02
//   //;-------- normal key 
//   0x19,0x00,       //DW        00019H                 ;Usage Minimum (0)=19 00
//   0x2A,0xFF,0x00,  //DW        H_INVALID+2AH,000FFH   ;Usage Maximum (255)=2A FF 00
//   0x15,0x00,       //DW        00015H                 ;Logical Minimum (0)=15 00
//   0x26,0xFF,0x00,  //DW        H_INVALID+26H,000FFH   ;Logical Maximum (255)=26 FF 00
//   0x75,0x08,       //DW        00875H                 ;Report Size (8)=75 08
//   0x95,0x06,       //DW        00E95H                 ;Report Count (D)=95 0D
//   0x81,0x00,       //DW        00081H                 ;Input (Data, Array)=81 00
//   0xC0,            //DW        H_INVALID+0C0H         ;End Collection
	
// *****************************************************
// ACPI REPORT DESCRIPTOR, bit0/bit1/bit2 correspond KeyCode 0x81/0x82/0x83.
// *****************************************************
	  0x06, 0x01, 0x00,  	//Usage Page (Generic Desktop)
		0x09, 0x80,  		//Usage (System control)
		0xa1, 0x01,  		//Collection (Application)
		0x85, ACPI_RID,  		//Report ID (1)
		0x19, 0x81,  		//Usage Min (Power\Sleep\Wakeup)
		0x29, 0x83,  		//Usage Max (WakeUp)
		0x15, 0x00,  		//Logical Minimum (0)
		0x25, 0x01,  		//Logical Maximum (1)
		0x95, 0x03,  		//Report Count (3)
		0x75, 0x01,  		//Report Size (1)
		0x81, 0x02,  		//Input (Dat, Var, Abs)	
		0x95, 0x01,  		//Report Count (1)
		0x75, 0x05,  		//Report Size (5)
		0x81, 0x01,  		//Input (constant)
		0xc0,  				//End Collection
// *****************************************************
// CONSUMER REPORT DESCRIPTOR, Report original code.
// *****************************************************
		0x05, 0x0c,  		//usage_page (Consumer Devices)
		0x09, 0x01,  		//Usage (Consumer Control)
		0xa1, 0x01, 		//Collection (Application)
		0x85, CONSUMER_RID, 		//Report_ID (2)		
		0x19, 0x00,  		// Logical_minimum (0)
		0x2a, 0xff,  		// Logical Maximun (1)		
		0x02, 0x15,  		// usage minimun (next track, prev track, cd_stop, eject)
		0x00, 0x26,  		// Usage_Maximun
		0xff, 0x7f,  		// Usage (play/pause)
		0x95, 0x01,  		// Usage (Mute)
		0x75, 0x10,  		// Usage (V+)		
		0x81, 0x00,  		// Usage (V-)
		0xc0,		  	   // End Collection
// Function VENDOR_Code
    0x06,00,0xff, //  ;Usage Page(Undefined )
    0x09,0x01,//;Usage(1)
    0xa1,0x01,// ;Collection(Application )
    0x85,VENDOR_RID,//;Report ID(0x3 )
    0x15,0x00,//;Logical Minimum(0x0 )
    0x26,0xff,0x00,//;Logical Maximum(0xFF )
    0x09,0x2f,// ;Usage(47)
    0x75,0x08,// ;Report Size(0x8)
    0x95,0x03,//	;Report Count(0x3)
    0x81,0x02,//	;Input(Data, Variable, Absolute)
    0xc0
};
/* HID device report Descriptor */
//uint8_t USB_HID_Custom_ReportDesc[] = 
//{
//    17*2-1,          /* Length */
////    0x05, 0x01,
////    0x09, 0x00,//custom
////    0xA1, 0x01,
////    0x15, 0x00,
////    0x25, 0xff,// 0~0xff
////    0x19, 0x01,
////    0x29, 0x08,
////    0x95, 0x40,
////    0x75, 0x08,
////    0x81, 0x02,
////    0x19, 0x01,
////    0x29, 0x08,
////    0x91, 0x02,
////    0xC0,
//    0x06, 0x01, 0xFF,   // Usage Page = 0xFF00 (Vendor Defined Page 1)
//    0x09, 0x01,         // Usage (Vendor Usage 1)
//    0xA1, 0x01,         // Collection (Application)
//    0x15, 0x00,         // Logical Minimum (data bytes in the report may have minimum value = 0x00)
//    0x26, 0xFF, 0x00,   // Logical Maximum (data bytes in the report may have maximum value = 0x00FF = unsigned 255)
//  	0x09, 0x20,		      //Vendor Usage
//		0x75, 0x08,          //Report Size(8)
//		0x95, 0x40,		      //Report Count(64)
//    // an "Input", "Output", or "Feature" item)
//    0x81, 0x02,         // Input (Data, Array, Abs): Instantiates input packet fields based on the
//  	0x09, 0x21,		      //Vendor Usage
//		0x95, 0x40,		      //Report Count(64)
//    0x91, 0x02,         // Output (Data, Array, Abs): Instantiates output packet fields. Uses same
//  	0x09, 0x22,		      //Vendor Usage
//		0x95, 0x08,		      //Report Count(8)
//		0xb1, 0x02,		      //Feature(Data, Variable, Relative)
//    0xC0                // End Collection	
//};
/* HID device report Descriptor */
uint8_t USB_HID_custom_ReportDesc[] = 
{
    0x1D,          /* Length */

    0x05, 0x01,
    0x09, 0x00,//custom
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,// 0~0xff
    0x19, 0x01,
    0x29, 0x08,
    0x95, 0x40,
    0x75, 0x08,
    0x81, 0x02,

    0x19, 0x01,
    0x29, 0x08,
    0x91, 0x02,

    0xC0,
//	33,
//    0x06, 0x01, 0xFF,   // Usage Page = 0xFF00 (Vendor Defined Page 1)
//    0x09, 0x01,         // Usage (Vendor Usage 1)
//    0xA1, 0x01,         // Collection (Application)
//    0x15, 0x00,         // Logical Minimum (data bytes in the report may have minimum value = 0x00)
//    0x26, 0xFF, 0x00,   // Logical Maximum (data bytes in the report may have maximum value = 0x00FF = unsigned 255)
//  	0x09, 0x20,		      //Vendor Usage
//		0x75, 0x08,          //Report Size(8)
//		0x95, 0x40,		      //Report Count(64)
//    // an "Input", "Output", or "Feature" item)
//    0x81, 0x02,         // Input (Data, Array, Abs): Instantiates input packet fields based on the
//  	0x09, 0x21,		      //Vendor Usage
//		0x95, 0x40,		      //Report Count(64)
//    0x91, 0x02,         // Output (Data, Array, Abs): Instantiates output packet fields. Uses same
//  	0x09, 0x22,		      //Vendor Usage
//		0x95, 0x08,		      //Report Count(8)
//		0xb1, 0x02,		      //Feature(Data, Variable, Relative)
//    0xC0                // End Collection		
};

uint8_t HIDReportDesc3 [] = 
{
	33,
    0x06, 0x01, 0xFF,   // Usage Page = 0xFF00 (Vendor Defined Page 1)
    0x09, 0x01,         // Usage (Vendor Usage 1)
    0xA1, 0x01,         // Collection (Application)
    0x15, 0x00,         // Logical Minimum (data bytes in the report may have minimum value = 0x00)
    0x26, 0xFF, 0x00,   // Logical Maximum (data bytes in the report may have maximum value = 0x00FF = unsigned 255)
  	0x09, 0x20,		      //Vendor Usage
		0x75, 0x08,          //Report Size(8)
		0x95, 0x40,		      //Report Count(64)
    // an "Input", "Output", or "Feature" item)
    0x81, 0x02,         // Input (Data, Array, Abs): Instantiates input packet fields based on the
  	0x09, 0x21,		      //Vendor Usage
		0x95, 0x40,		      //Report Count(64)
    0x91, 0x02,         // Output (Data, Array, Abs): Instantiates output packet fields. Uses same
  	0x09, 0x22,		      //Vendor Usage
		0x95, 0x08,		      //Report Count(8)
		0xb1, 0x02,		      //Feature(Data, Variable, Relative)
    0xC0                // End Collection														
};

/* HID Descriptor */
#define HID_DESCRIPTOR_0        0x09,    /* bLength */                  \
                                0x21,    /* bDescriptorType: HID */     \
                                0x10,    /* bcdHID: 1.10 */             \
                                0x01,                                   \
                                0x21,    /* bCountryCode */             \
                                0x01,    /* bNumDescriptors */          \
                                0x22,    /* bDescriptorType: Report */  \
								sizeof(USB_HID_Mouse_ReportDesc)-1,    /* wDescriptorLength */\
								0x00

/* HID Descriptor */
#define HID_DESCRIPTOR_1        0x09,    /* bLength */                  \
                                0x21,    /* bDescriptorType: HID */     \
                                0x10,    /* bcdHID: 1.10 */             \
                                0x01,                                   \
                                0x21,    /* bCountryCode */             \
                                0x01,    /* bNumDescriptors */          \
                                0x22,    /* bDescriptorType: Report */  \
								sizeof(USB_HID_Keyboard_ReportDesc)-1,    /* wDescriptorLength */\
								0x00
/* HID Descriptor */
#define HID_DESCRIPTOR_2        0x09,    /* bLength */                  \
                                0x21,    /* bDescriptorType: HID */     \
                                0x10,    /* bcdHID: 1.10 */             \
                                0x01,                                   \
                                0x21,    /* bCountryCode */             \
                                0x01,    /* bNumDescriptors */          \
                                0x22,    /* bDescriptorType: Report */  \
								sizeof(USB_HID_custom_ReportDesc)-1,    /* wDescriptorLength */\
								0x00
								/* HID Descriptor */
#define HID_DESCRIPTOR_3        0x09,    /* bLength */                  \
                                0x21,    /* bDescriptorType: HID */     \
                                0x10,    /* bcdHID: 1.10 */             \
                                0x01,                                   \
                                0x21,    /* bCountryCode */             \
                                0x01,    /* bNumDescriptors */          \
                                0x22,    /* bDescriptorType: Report */  \
								sizeof(HIDReportDesc3)-1,    /* wDescriptorLength */\
								0x00

/* USB Standard Configuration Descriptor */
const uint8_t USB_HID_ConfigurationDesc[] =
{
    /* Configuration Descriptor 9*/
    0x09,    /* bLength */             
    0x02,    /* bDescriptorType */     
    0x62+9+9+7+7,    /* 0x42,//wTotalLength �����������ܳ��� 9+ 9+9+7 +9+9+7+7 +9+9+7+7 +9+9+7+7*/        
    0x00,                              
    0x04,    /* bNumInterfaces */      
    0x01,    /* bConfigurationValue */ 
    0x00,    /* iConfiguration */      
    0xa0,    /* bmAttributes */        
    0x32,    /* bMaxPower */           
 /****************************************************/   
    /* HID Interface_0 Descriptor 9*/
    0x09,    /* bLength */           
    0x04,    /* bDescriptorType */   
    0x00,    /* bInterfaceNumber */  
    0x00,    /* bAlternateSetting */ 
    0x01,    /* bNumEndpoints */     
    0x03,    /* bInterfaceClass: HID */   
    0x01,    /* bInterfaceSubClass: Boot interface */
    0x02,    /* bInterfaceProtocol: Mouse */
    0x00,    /* iConfiguration */    

//    /* HID Descriptor 9*/
//    0x09,    /* bLength */
//    0x21,    /* bDescriptorType: HID */        
//    0x10,    /* bcdHID: 1.10 */
//    0x01,    
//    0x21,    /* bCountryCode */
//    0x01,    /* bNumDescriptors */
//    0x22,    /* bDescriptorType: Report */
//    sizeof(USB_HID_Mouse_ReportDesc)-1,//0x2E,    /* wDescriptorLength */
//    0x00,
HID_DESCRIPTOR_0,
    /* Endpoint 1 Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x81,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */

/************************************************/
    /* HID Interface_1 Descriptor 9*/
    0x09,    /* bLength */
    0x04,    /* bDescriptorType */
    0x01,    /* bInterfaceNumber */
    0x00,    /* bAlternateSetting */
    0x02,    /* bNumEndpoints */
    0x03,    /* bInterfaceClass: HID */
    0x01,    /* bInterfaceSubClass: Boot interface */
    0x01,    /* bInterfaceProtocol: Keyboard */
    0x00,    /* iConfiguration */

//    /* HID Descriptor 9*/
//    0x09,    /* bLength */
//    0x21,    /* bDescriptorType: HID */        
//    0x10,    /* bcdHID: 1.10 */
//    0x01,    
//    0x21,    /* bCountryCode */
//    0x01,    /* bNumDescriptors */
//    0x22,    /* bDescriptorType: Report */
//    sizeof(USB_HID_Keyboard_ReportDesc)-1,//0x41,    /* wDescriptorLength */
//    0x00,
HID_DESCRIPTOR_1,
    /* Endpoint 2 IN Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x82,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */
    
    /* Endpoint 2 OUT Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x02,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x0A,    /* bInterval */
    #if 1
/***************************************************/
    /* HID Interface_2 Descriptor 9*/
    0x09,    /* bLength */
    0x04,    /* bDescriptorType */
    0x02,    /* bInterfaceNumber */
    0x00,    /* bAlternateSetting */
    0x02,    /* bNumEndpoints */
    0x03,    /* bInterfaceClass: HID */
    0x00,    /* bInterfaceSubClass: */
    0x00,    /* bInterfaceProtocol:  */
    0x00,    /* iConfiguration */

//    /* HID Descriptor 9*/
//    0x09,    /* bLength */
//    0x21,    /* bDescriptorType: HID */        
//    0x10,    /* bcdHID: 1.10 */
//    0x01,    
//    0x21,    /* bCountryCode */
//    0x01,    /* bNumDescriptors */
//    0x22,    /* bDescriptorType: Report */
//    sizeof(USB_HID_custom_ReportDesc)-1,//0x1c,    /* wDescriptorLength */
//    0x00,
	HID_DESCRIPTOR_2,
    /* Endpoint 3 IN Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x83,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */
    
    /* Endpoint 3 OUT Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x03,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */
	
	
    /* HID Interface_3 Descriptor 9*/
    0x09,    /* bLength */
    0x04,    /* bDescriptorType */
    0x03,    /* bInterfaceNumber */
    0x00,    /* bAlternateSetting */
    0x02,    /* bNumEndpoints */
    0x03,    /* bInterfaceClass: HID */
    0x00,    /* bInterfaceSubClass: */
    0x00,    /* bInterfaceProtocol:  */
    0x00,    /* iConfiguration */

//    /* HID Descriptor 9*/
//    0x09,    /* bLength */
//    0x21,    /* bDescriptorType: HID */        
//    0x10,    /* bcdHID: 1.10 */
//    0x01,    
//    0x21,    /* bCountryCode */
//    0x01,    /* bNumDescriptors */
//    0x22,    /* bDescriptorType: Report */
//    sizeof(HIDReportDesc3)-1,//0x1c,    /* wDescriptorLength */
//    0x00,
HID_DESCRIPTOR_3,
    /* Endpoint 3 IN Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x84,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */
    
    /* Endpoint 3 OUT Descriptor 7*/
    0x07,    /* bLength */
    0x05,    /* bDescriptorType */
    0x04,    /* bEndpointAddress */
    0x03,    /* bmAttributes: Interrupt */ 
    0x40,    /* wMaxPacketSize: 64byte */
    0x00,
    0x01,    /* bInterval */	
	
    #endif
};

/* USB Standard Manufacture Descriptor */
const uint8_t USB_HID_ManufactureDesc[] =
{
    14,        /* bLength */    
    0x03,        /* bDescriptorType */    
    '3', 0x00,   /* BString */
    'T', 0x00,
    'h', 0x00,
    'i', 0x00,
    'h', 0x00,
    'k', 0x00,

};

/* USB Standard Configuration Descriptor */
const uint8_t USB_HID_ProductDesc[] =
{
    36,         /* bLength */
    0x03,         /* bDescriptorType */
    'W', 0x00,    /* BString */
    'i', 0x00,    
    'r', 0x00,    
    'e', 0x00,    
    'l', 0x00,    
    'e', 0x00,    
    's', 0x00,    
    's', 0x00,    
    ' ', 0x00,    
    'R', 0x00,    
    'e', 0x00,    
    'c', 0x00,    
    'e', 0x00,    
    'i', 0x00, 
    'v', 0x00,    
    'e', 0x00, 
    'r', 0x00, 	
};

/* USB Standard Configuration Descriptor */
const uint8_t USB_HID_SerialNumberDesc[] =
{
    0x1E,         /* bLength */
    0x03,         /* bDescriptorType */
    '2', 0x00,    /* BString */
    '0', 0x00,
    '2', 0x00,
    '2', 0x00,
    '-', 0x00,
    '1', 0x00,
    '2', 0x00,
    '3', 0x00,
    '0', 0x00,
    '-', 0x00,
    'B', 0x00,
    '1', 0x00,
    '3', 0x00,
    '5', 0x00,
};

/* USB Standard Configuration Descriptor */
const uint8_t USB_HID_LanuageIDDesc[] =
{
    0x04,    /* bLength */
    0x03,    /* bDescriptorType */
    0x09,    /* BString */
    0x04,
};


const uint8_t USB_HID_Desc_0[] =
{
    HID_DESCRIPTOR_0,
};

const uint8_t USB_HID_Desc_1[] =
{
    HID_DESCRIPTOR_1,
};
const uint8_t USB_HID_Desc_2[] =
{
    HID_DESCRIPTOR_2,
};

const uint8_t USB_HID_Desc_3[] =
{
    HID_DESCRIPTOR_3,
};
/*********************************************************************
 * @fn      usb_hid_set_mouse_report
 *
 * @brief   set report
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_set_mouse_report(uint8_t fu8_Index, uint8_t fu8_Value)
{
    HID_Mouse_Report[fu8_Index] = fu8_Value;
    /*
        HID_Report[0]: 0000 0001  left   button
                       0000 0010  right  button
                       0000 0100  middle button
        HID_Report[1]: X (-127 ~ +127)
        HID_Report[2]: Y (-127 ~ +127)
        HID_Report[3]: wheel (-127 ~ +127)
    */
}

/*********************************************************************
 * @fn      usb_hid_set_keyboard_report
 *
 * @brief   set report
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_set_keyboard_report(uint16_t fu16_Key)
{
    uint32_t i;
    
    switch (fu16_Key)
    {
        case KEY_LEFT_CONTROL:  HID_Keyboard_Report[0] |= 0x01;  break;
        case KEY_LEFT_SHIFT:    HID_Keyboard_Report[0] |= 0x02;  break;
        case KEY_LEFT_ALT:      HID_Keyboard_Report[0] |= 0x04;  break;
        case KEY_LEFT_GUI:      HID_Keyboard_Report[0] |= 0x08;  break;
        case KEY_RIGHT_CONTROL: HID_Keyboard_Report[0] |= 0x10;  break;
        case KEY_RIGHT_SHIFT:   HID_Keyboard_Report[0] |= 0x20;  break;
        case KEY_RIGHT_ALT:     HID_Keyboard_Report[0] |= 0x40;  break;
        case KEY_RIGHT_GUI:     HID_Keyboard_Report[0] |= 0x80;  break;

        default:
        {
            for (i = 2; i < 8; i++)
            {
                if (HID_Keyboard_Report[i] == fu16_Key) 
                {
                    break;
                }
                
                if (HID_Keyboard_Report[i] == 0x00) 
                {
                    HID_Keyboard_Report[i] = fu16_Key;

                    break;
                }
            }
        }break; 
    }
}

/*********************************************************************
 * @fn      usb_hid_clear_keyboard_report
 *
 * @brief   set report
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_clear_keyboard_report(uint16_t fu16_Key)
{
    uint32_t i, k;
    
    switch (fu16_Key)
    {
        case KEY_LEFT_CONTROL:  HID_Keyboard_Report[0] &= ~0x01;  break;
        case KEY_LEFT_SHIFT:    HID_Keyboard_Report[0] &= ~0x02;  break;
        case KEY_LEFT_ALT:      HID_Keyboard_Report[0] &= ~0x04;  break;
        case KEY_LEFT_GUI:      HID_Keyboard_Report[0] &= ~0x08;  break;
        case KEY_RIGHT_CONTROL: HID_Keyboard_Report[0] &= ~0x10;  break;
        case KEY_RIGHT_SHIFT:   HID_Keyboard_Report[0] &= ~0x20;  break;
        case KEY_RIGHT_ALT:     HID_Keyboard_Report[0] &= ~0x40;  break;
        case KEY_RIGHT_GUI:     HID_Keyboard_Report[0] &= ~0x80;  break;

        default:
        {
            for (i = 2; i < 8; i++)
            {
                if (HID_Keyboard_Report[i] == fu16_Key) 
                {
                    HID_Keyboard_Report[i] = 0x00;

                    for (k = i; k < 7; k++)
                    {
                        if (HID_Keyboard_Report[k + 1] != 0x00)
                        {
                            HID_Keyboard_Report[k] = HID_Keyboard_Report[k + 1];
                            HID_Keyboard_Report[k + 1] = 0x00;
                        }
                        else 
                        {
                            break;
                        }
                    }

                    break;
                }
            }
        }break; 
    }
}

/*********************************************************************
 * @fn      usb_hid_send_mouse_report
 *
 * @brief   send report to host
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_send_mouse_report(void)
{
    usb_selecet_endpoint(ENDPOINT_1);

    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_1, HID_Mouse_Report, REPORT0_LENGTH);

        usb_Endpoints_SET_TxPktRdy();
    }
    //memset(HID_Mouse_Report,0,REPORT0_LENGTH);
}

__attribute__((section("ram_code")))void usb_hid_send_mouse_report1(uint8_t *report)
{
    usb_selecet_endpoint(ENDPOINT_1);

    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_1, report, REPORT0_LENGTH);

        usb_Endpoints_SET_TxPktRdy();
    }
    //memset(HID_Mouse_Report,0,REPORT0_LENGTH);
}
/*********************************************************************
 * @fn      usb_hid_send_keyboard_report
 *
 * @brief   send report to host
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_send_keyboard_report(void)
{
    usb_selecet_endpoint(ENDPOINT_2);

    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_2, HID_Keyboard_Report, REPORT1_LENGTH);

        usb_Endpoints_SET_TxPktRdy();
    }
}
__attribute__((section("ram_code")))void usb_hid_send_keyboard_report1(uint8_t *report,uint8_t len)
{
    usb_selecet_endpoint(ENDPOINT_2);

    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_2, report, len);

        usb_Endpoints_SET_TxPktRdy();
    }
    //memset(HID_Mouse_Report,0,REPORT0_LENGTH);
}

__attribute__((section("ram_code")))void usb_hid_send_custom_report1(uint8_t *report)
{
    usb_selecet_endpoint(ENDPOINT_3);
	//show_reg(report,REPORT2_LENGTH,1);
    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_3, report, REPORT2_LENGTH);

        usb_Endpoints_SET_TxPktRdy();
    }
    //memset(HID_Mouse_Report,0,REPORT0_LENGTH);
}

__attribute__((section("ram_code")))void usb_hid_send_QuDongData_report(uint8_t *report,uint8_t len)
{
    usb_selecet_endpoint(ENDPOINT_4);

    /* FIFO empty */
    if (usb_Endpoints_GET_TxPkrRdy() == false) 
    {
        usb_write_fifo(ENDPOINT_4, report, len);

        usb_Endpoints_SET_TxPktRdy();
    }
    //memset(HID_Mouse_Report,0,REPORT0_LENGTH);
}
/*********************************************************************
 * @fn      usb_hid_SetReport_Out
 *
 * @brief   HID SET_REPORT 的数据阶段：读取主机下发的 LED 字节
 *********************************************************************/
static void usb_hid_SetReport_Out(void)
{
    uint8_t buf[8];
    uint8_t count = usb_Endpoint0_get_RxCount();

    if (count > sizeof(buf)) {
        count = (uint8_t)sizeof(buf);
    }
    usb_read_fifo(ENDPOINT_0, buf, count);
    usb_Endpoint0_FlushFIFO();

    Endpoint_0_DataOut_Handler = NULL;

    if (count >= 1u) {
        s_led_state   = buf[0];
        s_led_changed = 1u;
    }
}

/*********************************************************************
 * @fn      usb_hid_ClassRequest_Handler
 *
 * @brief   HID Class Request Handler
 *
 * @param   None.
 * @return  None.
 */
static void usb_hid_ClassRequest_Handler(usb_StandardRequest_t* pStandardRequest, usb_ReturnData_t* pReturnData)
{
    static uint8_t HID_Protocol = 1;    // 0: Boot   Protocol
                                        // 1: Report Protocol

    switch (pStandardRequest->bRequest)
    {
        case HID_GET_PROTOCOL:
        {
            pReturnData->DataBuffer = &HID_Protocol;
            pReturnData->DataLength = 1;
        }break;

        case HID_SET_PROTOCOL:
        {
            HID_Protocol = pStandardRequest->wValue[0];
        }break;

        case HID_SET_REPORT:
        {
            /* 数据阶段由 usb_hid_SetReport_Out 读取 */
            Endpoint_0_DataOut_Handler = usb_hid_SetReport_Out;
        }break;

        case HID_GET_IDLE:
        {
            /* Device does not support Getidle command */
            usb_Endpoint0_SendStall();
        }break;

        case HID_SET_IDLE:
        {
            /* Device does not support Setidle command */
            usb_Endpoint0_SendStall();
        }break;

        default: break; 
    }
}

/*********************************************************************
 * @fn      usb_hid_StandardClassRequest_Handler
 *
 * @brief   HID Standard Class Request Handler
 *
 * @param   None.
 * @return  None.
 */
static void usb_hid_StandardClassRequest_Handler(usb_StandardRequest_t* pStandardRequest, usb_ReturnData_t* pReturnData)
{
    switch (pStandardRequest->wValue[1])
    {
        case DESCRIPTOR_HID_REPORT:
        {
            if (pStandardRequest->wIndex[0] == 0){
                pReturnData->DataLength =  USB_HID_Mouse_ReportDesc[0];
                pReturnData->DataBuffer = &USB_HID_Mouse_ReportDesc[1];
            }
            else if (pStandardRequest->wIndex[0] == 1){
                pReturnData->DataLength =  USB_HID_Keyboard_ReportDesc[0];
                pReturnData->DataBuffer = &USB_HID_Keyboard_ReportDesc[1];
            }
            else if (pStandardRequest->wIndex[0] == 2){
                pReturnData->DataLength =  USB_HID_custom_ReportDesc[0];
                pReturnData->DataBuffer = &USB_HID_custom_ReportDesc[1];
            }
            else if (pStandardRequest->wIndex[0] == 3){
                pReturnData->DataLength =  HIDReportDesc3[0];
                pReturnData->DataBuffer = &HIDReportDesc3[1];
            }			
        }break;

        case DESCRIPTOR_HID:
        {
            if (pStandardRequest->wIndex[0] == 0){
                pReturnData->DataLength = 9;
                pReturnData->DataBuffer = (uint8_t *)USB_HID_Desc_0;
            }
            else if (pStandardRequest->wIndex[0] == 1){
                pReturnData->DataLength = 9;
                pReturnData->DataBuffer = (uint8_t *)USB_HID_Desc_1;
            }
            else if (pStandardRequest->wIndex[0] == 2){
                pReturnData->DataLength = 9;
                pReturnData->DataBuffer = (uint8_t *)USB_HID_Desc_2;
            }
            else if (pStandardRequest->wIndex[0] == 3){
                pReturnData->DataLength = 9;
                pReturnData->DataBuffer = (uint8_t *)USB_HID_Desc_3;
            }			
        }break;

        default: break; 
    }
}
/*********************************************************************
 * @fn      usb_hid_Endpoints_Handler
 *
 * @brief   hid Endpoints Handler
 *
 * @param   None.
 * @return  None.
 */
static char fput_c[]="0123456789abcdef";
void usb_hid_Endpoints_Handler(uint8_t RxStatus, uint8_t TxStatus)
{
    uint8_t lu8_RxCount;
    uint8_t lu8_Buffer[64] = {0,0,0,0};
    (void)fput_c;
    (void)TxStatus;
    if (RxStatus & 1 << ENDPOINT_2) 
    {
        usb_selecet_endpoint(ENDPOINT_2);

        lu8_RxCount = usb_Endpoints_get_RxCount();
        
        usb_read_fifo(ENDPOINT_2, lu8_Buffer, lu8_RxCount);

        usb_Endpoints_FlushRxFIFO();
    }
    if (RxStatus & 1 << ENDPOINT_3){
        usb_selecet_endpoint(ENDPOINT_3);

        lu8_RxCount = usb_Endpoints_get_RxCount();
        
        usb_read_fifo(ENDPOINT_3, lu8_Buffer, lu8_RxCount);
        

        usb_Endpoints_FlushRxFIFO();
        /* 本工程未启用厂商 OTA 通道（EP3 vendor 写入仅丢弃），
         * 故不向 usb_hid_ota 任务投递事件。 */
    }
	
}

/*********************************************************************
 * @fn      usb_hid_init
 *
 * @brief   hid device parameter initialization 
 *
 * @param   None.
 * @return  None.
 */
void usb_hid_init(void)
{
    /* Initialize the relevant pointer  */
    usbdev_get_dev_desc((uint8_t *)USB_HID_DeviceDesc);
    usbdev_get_config_desc((uint8_t *)USB_HID_ConfigurationDesc);
//    usbdev_get_hidreport_desc(0, (uint8_t *)USB_HID_Mouse_ReportDesc);
//    usbdev_get_hidreport_desc(1, (uint8_t *)USB_HID_Keyboard_ReportDesc);
//    usbdev_get_hidreport_desc(2, (uint8_t *)USB_HID_custom_ReportDesc);
//	usbdev_get_hidreport_desc(3, (uint8_t *)HIDReportDesc3);
    usbdev_get_string_Manufacture((uint8_t *)USB_HID_ManufactureDesc);
    usbdev_get_string_Product((uint8_t *)USB_HID_ProductDesc);
    usbdev_get_string_SerialNumber((uint8_t *)USB_HID_SerialNumberDesc);
    usbdev_get_string_LanuageID((uint8_t *)USB_HID_LanuageIDDesc);

    Endpoint_0_StandardClassRequest_Handler = usb_hid_StandardClassRequest_Handler;
    Endpoint_0_ClassRequest_Handler = usb_hid_ClassRequest_Handler;

    Endpoints_Handler = usb_hid_Endpoints_Handler;

    USB_Reset_Handler = usb_hid_init;

    /* 新 SDK 的 usb_device_init() 只开时钟/PHY，不配置数据端点 FIFO，
     * 这里沿用厂商 Dongle 例程的 FIFO 布局。 */
    usb_selecet_endpoint(ENDPOINT_1);
    usb_endpoint_Txfifo_config(0x08, 3);
    usb_TxMaxP_set(8);
    usb_RxMaxP_set(8);

    usb_selecet_endpoint(ENDPOINT_2);
    usb_endpoint_Txfifo_config(0x18, 3);
    usb_endpoint_Rxfifo_config(0x20, 3);
    usb_TxMaxP_set(8);
    usb_RxMaxP_set(8);
    usb_RxInt_Enable(ENDPOINT_2);

    usb_selecet_endpoint(ENDPOINT_3);
    usb_endpoint_Txfifo_config(0x28, 3);
    usb_endpoint_Rxfifo_config(0x30, 3);
    usb_TxMaxP_set(8);
    usb_RxMaxP_set(8);
    usb_RxInt_Enable(ENDPOINT_3);

    usb_SuspendDetectEn();
    usb_SingleInt_Enable(USB_INT_STATUS_SUSPEND);
    usb_SingleInt_Enable(USB_INT_STATUS_RESUME);
}

