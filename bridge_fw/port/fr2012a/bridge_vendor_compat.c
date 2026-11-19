/* 厂商示例主程序提供的符号（本工程用不到）——空实现，避免链接缺失。
 *
 *   master_write_data : 厂商 OTA/私有协议的 68B 上行通道（usb_dev.c 的
 *                       HID_GetKbData/HID_GetEEpromData 会调用）；
 *   show_reg          : vendor profile 里的调试打印（hid_service.c 使用）。
 */
#include <stdint.h>

/* 厂商闭源库（sys_utisl.o）已提供 flash_capacity，这里不再定义。 */

void master_write_data(uint8_t *data, uint8_t len)
{
    (void)data;
    (void)len;
}

void show_reg(uint8_t *data, uint32_t len, uint8_t dbg_on)
{
    (void)data;
    (void)len;
    (void)dbg_on;
}
