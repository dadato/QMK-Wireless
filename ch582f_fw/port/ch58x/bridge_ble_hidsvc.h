/* 本工程自有的 HOGP HID 服务（替代 SDK 例程的 Profile/hidkbdservice.c）
 *
 * 为什么另起替代文件而不改 SDK：
 *   厂商 hidReportMap[] 只有「键盘 RID 0」一张报表，且是 static const，
 *   无法从外部扩展；hidkbdservice.c 属 SDK submodule（固定 commit bd508ad7），
 *   按硬规则不得修改。故 Makefile 用 filter-out %hidkbdservice.c 排除该文件，
 *   由本文件提供同名同签名的 Hid_AddService / Hid_SetParameter / Hid_GetParameter
 *   与 hidReportMapLen / hidProtocolMode（hiddev.c 依赖这两个全局）。
 *
 * Report ID 分配（一旦使用非 0 Report ID，所有报表都必须带 ID）：
 *   1 = 键盘输入 8B    2 = 多媒体输入 2B
 *   3 = 系统控制输入 1B  4 = 键盘 LED 输出 1B
 *   5 = VIA vendor 输入/输出 各 32B（QMK Raw HID，Usage Page 0xFF60/Usage 0x61）
 * 鼠标（4B）与 NKRO（16B）本服务不含对应报表：BLE 模式下上游已把 NKRO
 * 降级为 8B 键盘报文，鼠标报文在 BLE 侧返回 BRIDGE_ERR_UNSUPPORTED。
 */
#ifndef BRIDGE_BLE_HIDSVC_H
#define BRIDGE_BLE_HIDSVC_H

#include <stdint.h>
#include <stdbool.h>

#define BRIDGE_HID_RID_KEYBOARD  1u
#define BRIDGE_HID_RID_CONSUMER  2u
#define BRIDGE_HID_RID_SYSTEM    3u
#define BRIDGE_HID_RID_LED_OUT   4u
#define BRIDGE_HID_RID_RAW       5u

/* VIA vendor 报表长度（QMK Raw HID 端点大小 RAW_EPSIZE == 32） */
#define BRIDGE_HID_RAW_REPORT_LEN 32u

/* 必须在 HidEmu_Init() **之后**调用：
 * 用本工程的 report/evt/state 回调覆盖厂商例程回调。
 * 厂商 reportCB 在主机写 CCCD（HID_DEV_OPER_ENABLE）时会启动 2s 周期的
 * 演示键码定时器（hidkbd.c START_REPORT_EVT，键码 4..29），若不覆盖，
 * 每次连接后 BLE 侧会周期性地冒出乱码按键。 */
void bridge_ble_hidsvc_override_cbs(void);

/* 广播意图开关：
 *   true  = 断链后自动恢复广播（上电默认 / 进入配对窗口）
 *   false = 主动停广播（sdk_ble_deinit），断链后不再自动开
 * 用于区分「链路异常断开」与「上层要求关闭射频」两种 GAPROLE_WAITING。 */
void bridge_ble_hidsvc_set_adv_intent(bool on);

#endif /* BRIDGE_BLE_HIDSVC_H */
