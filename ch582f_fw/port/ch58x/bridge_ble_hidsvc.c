/* 本工程自有的 HOGP HID 服务实现（替代 SDK 例程的 hidkbdservice.c）
 *
 * 提供内容（与厂商文件同名同签名，供 hidkbd.c / hiddev.c 链接）：
 *   Hid_AddService() / Hid_SetParameter() / Hid_GetParameter()
 *   hidReportMapLen / hidProtocolMode（hiddev.c 的两个 extern 全局）
 * 差异：
 *   1) hidReportMap 扩展为 5 张带 Report ID 的报表：
 *        RID 1 键盘输入 8B / RID 2 多媒体输入 2B /
 *        RID 3 系统控制输入 1B / RID 4 键盘 LED 输出 1B /
 *        RID 5 VIA vendor Raw HID 32B 输入 + 32B 输出
 *      （原厂商只有键盘 RID 0，且一旦引入非 0 Report ID，所有报表都必须带 ID）
 *   2) 提供本工程自己的 HID Dev 回调（bridge_ble_hidsvc_override_cbs()），
 *      屏蔽厂商 reportCB 的演示键码定时器，把 LED Output 转发给 SK32，
 *      并把 VIA vendor Output 经隧道转发给 SK32（sdk_ble_on_raw_report）。
 *   3) 在 GAP 状态回调里把连接/断开事件接入 sdk_ble_on_connect/disconnect，
 *      否则 sdk_ble_is_connected() 永远为假、BLE 报文发不出去。
 *
 * 头文件依赖：仅本文件与 bridge_ch58x_sdk.c 接触 WCH SDK 头。
 */
#include <string.h>

#include "CONFIG.h"
#include "hidkbdservice.h"     /* HID_FEATURE_FLAGS */
#include "hiddev.h"
#include "battservice.h"

#include "bridge_ble_hidsvc.h"
#include "bridge_port.h"       /* port_hid_diag 实现（诊断命令 0x2B 用） */
#include "bridge_types.h"      /* BRIDGE_OK / BRIDGE_ERR_PARAM */
#include "bridge_cfg.h"        /* bridge_cfg_get()->conn_interval */
#include "bridge_ch58x_sdk.h"  /* sdk_ble_on_connect/disconnect/led_report */

/* 下列常量在厂商 hidkbd.c 内部以 #define 私有定义（不经头文件导出），
 * 本文件独立实现同一套初始化逻辑，故按厂商取值自带一份。 */
#define BRIDGE_HID_IDLE_TIMEOUT     60000u  /* 空闲超时 60s，与 hidkbd.c 一致 */
#define BRIDGE_CONN_INTERVAL_UNITS  8u      /* 8 × 1.25ms = 10ms；cfg 未配置时的回退值 */
#define BRIDGE_SLAVE_LATENCY        0u
#define BRIDGE_CONN_TIMEOUT_UNITS   500u    /* 500 × 10ms = 5s */

/* ---------------- 报表清单 ----------------
 * 下标即 hidRptMap 顺序，Hid_AddService() 里逐项填充。 */
enum {
    BRIDGE_HID_NUM_REPORTS,

    BRIDGE_HID_IDX_KEY_IN = 0,
    BRIDGE_HID_IDX_CONSUMER,
    BRIDGE_HID_IDX_SYSTEM,
    BRIDGE_HID_IDX_LED_OUT,
    BRIDGE_HID_IDX_BOOT_KEY_IN,
    BRIDGE_HID_IDX_BOOT_KEY_OUT,
    BRIDGE_HID_IDX_FEATURE,
    BRIDGE_HID_IDX_RAW_IN,
    BRIDGE_HID_IDX_RAW_OUT,
    BRIDGE_HID_IDX_BATTERY
};

#define BRIDGE_HID_REPORT_CNT (BRIDGE_HID_IDX_BATTERY + 1u)

/* ---------------- 属性表下标 ---------------- */
enum {
    HID_SERVICE_IDX,
    HID_INCLUDED_SERVICE_IDX,
    HID_INFO_DECL_IDX,
    HID_INFO_IDX,
    HID_CONTROL_POINT_DECL_IDX,
    HID_CONTROL_POINT_IDX,
    HID_PROTOCOL_MODE_DECL_IDX,
    HID_PROTOCOL_MODE_IDX,
    HID_REPORT_MAP_DECL_IDX,
    HID_REPORT_MAP_IDX,
    HID_EXT_REPORT_REF_DESC_IDX,
    HID_REPORT_KEY_IN_DECL_IDX,
    HID_REPORT_KEY_IN_IDX,
    HID_REPORT_KEY_IN_CCCD_IDX,
    HID_REPORT_REF_KEY_IN_IDX,
    HID_REPORT_CONSUMER_DECL_IDX,
    HID_REPORT_CONSUMER_IDX,
    HID_REPORT_CONSUMER_CCCD_IDX,
    HID_REPORT_REF_CONSUMER_IDX,
    HID_REPORT_SYSTEM_DECL_IDX,
    HID_REPORT_SYSTEM_IDX,
    HID_REPORT_SYSTEM_CCCD_IDX,
    HID_REPORT_REF_SYSTEM_IDX,
    HID_REPORT_LED_OUT_DECL_IDX,
    HID_REPORT_LED_OUT_IDX,
    HID_REPORT_REF_LED_OUT_IDX,
    HID_BOOT_KEY_IN_DECL_IDX,
    HID_BOOT_KEY_IN_IDX,
    HID_BOOT_KEY_IN_CCCD_IDX,
    HID_BOOT_KEY_OUT_DECL_IDX,
    HID_BOOT_KEY_OUT_IDX,
    HID_FEATURE_DECL_IDX,
    HID_FEATURE_IDX,
    HID_REPORT_REF_FEATURE_IDX,
    HID_REPORT_RAW_IN_DECL_IDX,
    HID_REPORT_RAW_IN_IDX,
    HID_REPORT_RAW_IN_CCCD_IDX,
    HID_REPORT_REF_RAW_IN_IDX,
    HID_REPORT_RAW_OUT_DECL_IDX,
    HID_REPORT_RAW_OUT_IDX,
    HID_REPORT_REF_RAW_OUT_IDX
};

/* ---------------- UUID ---------------- */
static const uint8_t hidServUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(HID_SERV_UUID), HI_UINT16(HID_SERV_UUID)};
static const uint8_t hidBootKeyInputUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(BOOT_KEY_INPUT_UUID), HI_UINT16(BOOT_KEY_INPUT_UUID)};
static const uint8_t hidBootKeyOutputUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(BOOT_KEY_OUTPUT_UUID), HI_UINT16(BOOT_KEY_OUTPUT_UUID)};
static const uint8_t hidInfoUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(HID_INFORMATION_UUID), HI_UINT16(HID_INFORMATION_UUID)};
static const uint8_t hidReportMapUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(REPORT_MAP_UUID), HI_UINT16(REPORT_MAP_UUID)};
static const uint8_t hidControlPointUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(HID_CTRL_PT_UUID), HI_UINT16(HID_CTRL_PT_UUID)};
static const uint8_t hidReportUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(REPORT_UUID), HI_UINT16(REPORT_UUID)};
static const uint8_t hidProtocolModeUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(PROTOCOL_MODE_UUID), HI_UINT16(PROTOCOL_MODE_UUID)};

/* ---------------- 值 ---------------- */
static const uint8_t hidInfo[HID_INFORMATION_LEN] = {
    LO_UINT16(0x0111), HI_UINT16(0x0111),   /* bcdHID */
    0x00,                                   /* bCountryCode */
    HID_FEATURE_FLAGS};

/* HID Report Map：报表描述符（含 Report ID 前缀项） */
static const uint8_t hidReportMap[] = {
    /* ---- 键盘：RID 1，8 字节 ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, BRIDGE_HID_RID_KEYBOARD,  /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Min (224) */
    0x29, 0xE7,        /*   Usage Max (231) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data,Var,Abs)：修饰键字节 */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x01,        /*   Input (Const)：保留字节 */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x65,        /*   Logical Max (101) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Min (0) */
    0x29, 0x65,        /*   Usage Max (101) */
    0x81, 0x00,        /*   Input (Data,Array)：6 键阵列 */
    /* ---- 键盘 LED 输出：RID 4，1 字节 ---- */
    0x85, BRIDGE_HID_RID_LED_OUT,   /*   Report ID (4) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Min (1) */
    0x29, 0x05,        /*   Usage Max (5) */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x91, 0x02,        /*   Output (Data,Var,Abs)：Num/Caps/Scroll/Compose/Kana */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x03,        /*   Report Size (3) */
    0x91, 0x01,        /*   Output (Const)：补齐 1 字节 */
    0xC0,              /* End Collection */

    /* ---- 多媒体：RID 2，2 字节（16 位 usage 码，Array 形式） ---- */
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, BRIDGE_HID_RID_CONSUMER,  /*   Report ID (2) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x26, 0xFF, 0x03,  /*   Logical Max (0x3FF) */
    0x19, 0x00,        /*   Usage Min (0) */
    0x2A, 0xFF, 0x03,  /*   Usage Max (0x3FF) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x00,        /*   Input (Data,Array,Abs) */
    0xC0,              /* End Collection */

    /* ---- 系统控制：RID 3，1 字节（bit0=PowerDown 0x81 / bit1=Sleep 0x82 /
     *      bit2=WakeUp 0x83，与 QMK host_system_send() 位定义一致） ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x80,        /* Usage (System Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, BRIDGE_HID_RID_SYSTEM,    /*   Report ID (3) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x19, 0x81,        /*   Usage Min (0x81) */
    0x29, 0x83,        /*   Usage Max (0x83) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x03,        /*   Report Count (3) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x81, 0x01,        /*   Input (Const)：补齐 1 字节 */
    0xC0,              /* End Collection */

    /* ---- VIA vendor：RID 5，32B 输入 + 32B 输出（QMK Raw HID） ----
     * 与 Dongle 侧 USB Raw HID 描述符（bridge_usb_ch58x.c 的 s_raw_report）
     * 保持一致（Usage Page 0xFF60 / Usage 0x61 / 32B IN + 32B OUT），
     * 使 usevia.app 经 WebHID 连接 BLE HID 设备时能识别出同一套 raw HID 端点。 */
    0x06, 0x60, 0xFF,  /* Usage Page (Vendor Defined 0xFF60) */
    0x09, 0x61,        /* Usage (0x61) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, BRIDGE_HID_RID_RAW,  /*   Report ID (5) */
    0x09, 0x62,        /*   Usage (0x62) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x26, 0xFF, 0x00,  /*   Logical Max (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x20,        /*   Report Count (32) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x09, 0x63,        /*   Usage (0x63) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x26, 0xFF, 0x00,  /*   Logical Max (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x20,        /*   Report Count (32) */
    0x91, 0x02,        /*   Output (Data,Var,Abs) */
    0xC0               /* End Collection */
};

uint16_t hidReportMapLen = sizeof(hidReportMap);

/* hiddev.c 通过 extern 读写（协议模式切换、断链复位） */
uint8_t hidProtocolMode = HID_PROTOCOL_MODE_REPORT;

/* ---------------- 属性表 ---------------- */
static const gattAttrType_t hidService = {ATT_BT_UUID_SIZE, hidServUUID};

static uint16_t include = GATT_INVALID_HANDLE;

static uint8_t hidInfoProps          = GATT_PROP_READ;
static uint8_t hidReportMapProps     = GATT_PROP_READ;
static uint8_t hidExtReportRefDesc[ATT_BT_UUID_SIZE] =
    {LO_UINT16(BATT_LEVEL_UUID), HI_UINT16(BATT_LEVEL_UUID)};

static uint8_t hidControlPointProps  = GATT_PROP_WRITE_NO_RSP;
static uint8_t hidControlPoint;

static uint8_t hidProtocolModeProps  = GATT_PROP_READ | GATT_PROP_WRITE_NO_RSP;

/* 键盘输入（RID 1） */
static uint8_t       hidReportKeyInProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       hidReportKeyIn;
static gattCharCfg_t hidReportKeyInClientCharCfg[GATT_MAX_NUM_CONN];
static uint8_t hidReportRefKeyIn[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_KEYBOARD, HID_REPORT_TYPE_INPUT};

/* 多媒体输入（RID 2） */
static uint8_t       hidReportConsumerProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       hidReportConsumer[2];     /* 16 位 usage 码 */
static gattCharCfg_t hidReportConsumerClientCharCfg[GATT_MAX_NUM_CONN];
static uint8_t hidReportRefConsumer[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_CONSUMER, HID_REPORT_TYPE_INPUT};

/* 系统控制输入（RID 3） */
static uint8_t       hidReportSystemProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       hidReportSystem;
static gattCharCfg_t hidReportSystemClientCharCfg[GATT_MAX_NUM_CONN];
static uint8_t hidReportRefSystem[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_SYSTEM, HID_REPORT_TYPE_INPUT};

/* LED 输出（RID 4） */
static uint8_t hidReportLedOutProps = GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP;
static uint8_t hidReportLedOut;
static uint8_t hidReportRefLedOut[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_LED_OUT, HID_REPORT_TYPE_OUTPUT};

/* Boot 键盘（无 Report ID 语义，id 与键盘输入保持一致仅用于查表） */
static uint8_t       hidReportBootKeyInProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       hidReportBootKeyIn;
static gattCharCfg_t hidReportBootKeyInClientCharCfg[GATT_MAX_NUM_CONN];
static uint8_t hidReportBootKeyOutProps = GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP;
static uint8_t hidReportBootKeyOut;

/* Feature（厂商保留，id 0） */
static uint8_t hidReportFeatureProps = GATT_PROP_READ | GATT_PROP_WRITE;
static uint8_t hidReportFeature;
static uint8_t hidReportRefFeature[HID_REPORT_REF_LEN] =
    {0, HID_REPORT_TYPE_FEATURE};

/* VIA vendor 输入（RID 5，32B，QMK Raw HID：模组 → 主机） */
static uint8_t       hidReportRawInProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       hidReportRawIn[BRIDGE_HID_RAW_REPORT_LEN];
static gattCharCfg_t hidReportRawInClientCharCfg[GATT_MAX_NUM_CONN];
static uint8_t hidReportRefRawIn[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_RAW, HID_REPORT_TYPE_INPUT};

/* VIA vendor 输出（RID 5，32B，QMK Raw HID：主机 → 模组） */
static uint8_t hidReportRawOutProps = GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP;
static uint8_t hidReportRawOut[BRIDGE_HID_RAW_REPORT_LEN];
static uint8_t hidReportRefRawOut[HID_REPORT_REF_LEN] =
    {BRIDGE_HID_RID_RAW, HID_REPORT_TYPE_OUTPUT};

static gattAttribute_t hidAttrTbl[] = {
    /* HID Service */
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&hidService},
    /* Included service (battery) */
    {{ATT_BT_UUID_SIZE, includeUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&include},
    /* HID Information */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidInfoProps},
    {{ATT_BT_UUID_SIZE, hidInfoUUID}, GATT_PERMIT_ENCRYPT_READ, 0, (uint8_t *)hidInfo},
    /* HID Control Point */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidControlPointProps},
    {{ATT_BT_UUID_SIZE, hidControlPointUUID}, GATT_PERMIT_ENCRYPT_WRITE, 0, &hidControlPoint},
    /* HID Protocol Mode */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidProtocolModeProps},
    {{ATT_BT_UUID_SIZE, hidProtocolModeUUID},
     GATT_PERMIT_ENCRYPT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, &hidProtocolMode},
    /* HID Report Map */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportMapProps},
    {{ATT_BT_UUID_SIZE, hidReportMapUUID}, GATT_PERMIT_ENCRYPT_READ, 0,
     (uint8_t *)hidReportMap},
    /* HID External Report Reference Descriptor */
    {{ATT_BT_UUID_SIZE, extReportRefUUID}, GATT_PERMIT_READ, 0, hidExtReportRefDesc},
    /* 键盘输入 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportKeyInProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID}, GATT_PERMIT_ENCRYPT_READ, 0, &hidReportKeyIn},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID},
     GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, (uint8_t *)&hidReportKeyInClientCharCfg},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefKeyIn},
    /* 多媒体输入 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportConsumerProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID}, GATT_PERMIT_ENCRYPT_READ, 0, hidReportConsumer},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID},
     GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, (uint8_t *)&hidReportConsumerClientCharCfg},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefConsumer},
    /* 系统控制输入 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportSystemProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID}, GATT_PERMIT_ENCRYPT_READ, 0, &hidReportSystem},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID},
     GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, (uint8_t *)&hidReportSystemClientCharCfg},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefSystem},
    /* LED 输出 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportLedOutProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID},
     GATT_PERMIT_ENCRYPT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, &hidReportLedOut},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefLedOut},
    /* Boot 键盘输入 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportBootKeyInProps},
    {{ATT_BT_UUID_SIZE, hidBootKeyInputUUID}, GATT_PERMIT_ENCRYPT_READ, 0, &hidReportBootKeyIn},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID},
     GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, (uint8_t *)&hidReportBootKeyInClientCharCfg},
    /* Boot 键盘输出 */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportBootKeyOutProps},
    {{ATT_BT_UUID_SIZE, hidBootKeyOutputUUID},
     GATT_PERMIT_ENCRYPT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, &hidReportBootKeyOut},
    /* Feature */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportFeatureProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID},
     GATT_PERMIT_ENCRYPT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, &hidReportFeature},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefFeature},
    /* VIA vendor 输入（RID 5，32B） */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportRawInProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID}, GATT_PERMIT_ENCRYPT_READ, 0, hidReportRawIn},
    {{ATT_BT_UUID_SIZE, clientCharCfgUUID},
     GATT_PERMIT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, (uint8_t *)&hidReportRawInClientCharCfg},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefRawIn},
    /* VIA vendor 输出（RID 5，32B） */
    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &hidReportRawOutProps},
    {{ATT_BT_UUID_SIZE, hidReportUUID},
     GATT_PERMIT_ENCRYPT_READ | GATT_PERMIT_ENCRYPT_WRITE, 0, hidReportRawOut},
    {{ATT_BT_UUID_SIZE, reportRefUUID}, GATT_PERMIT_READ, 0, hidReportRefRawOut},
};

/* 报表映射表：hiddev.c 按 (id,type,mode) 或 cccdHandle 查表 */
static hidRptMap_t hidRptMap[BRIDGE_HID_REPORT_CNT];

static gattServiceCBs_t s_gatt_cbs = {HidDev_ReadAttrCB, HidDev_WriteAttrCB, NULL};

/* ---------------- 本工程 HID Dev 回调 ---------------- */

static uint8_t bridge_hid_report_cb(uint8_t id, uint8_t type, uint16_t uuid,
                                    uint8_t oper, uint16_t *pLen, uint8_t *pData)
{
    if (oper == HID_DEV_OPER_WRITE) {
        /* 主机下发（REPORT_UUID 或 Boot 输出）→ 按 Report ID 分流：
         *   RID_RAW    = VIA vendor 32B Raw HID → 经隧道转发给 SK32
         *   RID_LED_OUT= 键盘 LED 状态 → 回传 SK32 驱动指示灯
         * 长度校验交给 Hid_SetParameter。 */
        if (type == HID_REPORT_TYPE_OUTPUT && pLen != 0 && pData != 0 && *pLen >= 1u) {
            if (id == BRIDGE_HID_RID_RAW) {
                sdk_ble_on_raw_report(pData, (uint8_t)(*pLen));
            } else {
                sdk_ble_on_led_report(pData[0]);
            }
        }
        return Hid_SetParameter(id, type, uuid,
                                (pLen != 0) ? (uint8_t)(*pLen) : 0u, pData);
    }
    if (oper == HID_DEV_OPER_READ) {
        return Hid_GetParameter(id, type, uuid, pLen, pData);
    }
    /* HID_DEV_OPER_ENABLE / DISABLE：**故意**不做任何事。
     * 厂商 reportCB 会在此 tmos_start_task(START_REPORT_EVT)，
     * 每 2s 发送演示键码 4..29，导致主机侧周期性乱码按键。 */
    return SUCCESS;
}

static void bridge_hid_evt_cb(uint8_t evt)
{
    /* HID Control Point（UUID 0x2A4C）：主机写 Suspend(0x00)/Exit Suspend(0x01)，
     * 即「PC 对模组的休眠控制」。转成主机电源状态（挂起时许可浅睡，保持链路）。 */
    if (evt == HID_DEV_SUSPEND_EVT) {
        sdk_ble_on_host_power(true);
    } else if (evt == HID_DEV_EXIT_SUSPEND_EVT) {
        sdk_ble_on_host_power(false);
    }
}

/* ---------------- 连接参数更新（须在 TMOS 任务上下文发起） ----------------
 * 厂商 hidkbd.c 在 GAP_LINK_ESTABLISHED 里 tmos_start_task(START_PARAM_UPDATE_EVT,
 * 12800) 延迟发起，并在**任务事件上下文**调用 GAPRole_PeripheralConnParamUpdateReq()
 * 且传入**有效 taskId** —— SDK 明确该参数用于接收 L2CAP_SIGNAL_EVENT 响应消息
 * （见 CH58xBLE_LIB.h 该函数声明注释）。
 * 此前实现直接在 GAP 回调里调用并传 INVALID_TASK_ID，响应事件被投递到非法任务，
 * 实测连接建立后约 0.7s 模组复位（看门狗）。故按厂商方式重实现。 */
#define BRIDGE_CONN_PARAM_EVT     0x0001u
#define BRIDGE_CONN_PARAM_DELAY   12800u  /* 12800 × 625us ≈ 8s，与厂商 hidkbd.c 一致 */

static uint8_t  s_hid_task    = 0xFFu;
static uint16_t s_conn_handle = INVALID_CONNHANDLE;

static uint16_t bridge_hid_process_event(uint8_t task_id, uint16_t events)
{
    bridge_dbg_mark(0x60);      /* HID 任务事件上下文进入 */
    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg = tmos_msg_receive(task_id);
        if (pMsg != 0) {
            tmos_msg_deallocate(pMsg);
        }
        return (uint16_t)(events ^ SYS_EVENT_MSG);
    }
    if (events & BRIDGE_CONN_PARAM_EVT) {
        bridge_dbg_mark(0x61);  /* 连接参数更新（延迟约 8s 发起） */
        if (s_conn_handle != INVALID_CONNHANDLE) {
            /* 与 port_radio_init() 的 cfg 下发保持一致：cfg 为 0 时用回退值 */
            uint8_t itv_units = bridge_cfg_get()->conn_interval;
            if (itv_units == 0u) {
                itv_units = BRIDGE_CONN_INTERVAL_UNITS;
            }
            (void)GAPRole_PeripheralConnParamUpdateReq(
                s_conn_handle,
                itv_units, itv_units,
                BRIDGE_SLAVE_LATENCY, BRIDGE_CONN_TIMEOUT_UNITS,
                s_hid_task);
        }
        bridge_dbg_mark(0x62);
        return (uint16_t)(events ^ BRIDGE_CONN_PARAM_EVT);
    }
    return 0u;
}

static bool s_adv_intent = true;

void bridge_ble_hidsvc_set_adv_intent(bool on)
{
    s_adv_intent = on;
}

static void bridge_hid_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    if (newState == GAPROLE_CONNECTED) {
        if (pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT) {
            gapEstLinkReqEvent_t *ev = (gapEstLinkReqEvent_t *)pEvent;

            bridge_dbg_mark(0x40);      /* GAP 连接建立事件进入本回调 */
            bridge_dbg_mark(0x41);      /* 即将进入 sdk_ble_on_connect()（内含 NV 写） */
            sdk_ble_on_connect(ev->connectionHandle);
            bridge_dbg_mark(0x42);      /* sdk_ble_on_connect() 已返回 */
            /* 等价厂商 hidkbd.c 的 START_PARAM_UPDATE_EVT：连上后延迟发起一次
             * 连接参数更新。必须经 TMOS 任务投递（本回调可能正处在 GAP 事件派发中），
             * 且传有效 taskId 以接收 L2CAP_SIGNAL_EVENT 响应。 */
            s_conn_handle = ev->connectionHandle;
            if (s_hid_task != 0xFFu) {
                tmos_start_task(s_hid_task, BRIDGE_CONN_PARAM_EVT, BRIDGE_CONN_PARAM_DELAY);
            }
            bridge_dbg_mark(0x43);      /* 连接事件处理完毕 */
        }
    } else if (newState == GAPROLE_WAITING) {
        bridge_dbg_mark(0x44);          /* GAP WAITING（断链/可广播）分支 */
        if (pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT) {
            s_conn_handle = INVALID_CONNHANDLE;
            sdk_ble_on_disconnect(0u);
        }
        /* 有连接期间发起的切槽：此时已无连接，补做切槽（含换槽地址 + 重开广播）。
         * 该路径自己会开启目标槽的广播，故直接返回，不再走下面的通用恢复。 */
        if (sdk_ble_resume_pending()) {
            return;
        }
        /* 仅在「本意要保持可连接」时才自动恢复广播；
         * sdk_ble_deinit() 会先置 adv_intent=false，避免停广播被立刻撤销。 */
        if (s_adv_intent) {
            uint8_t en = TRUE;
            (void)GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &en);
        }
        bridge_dbg_mark(0x45);          /* WAITING 分支处理完毕 */
    } else if (newState == GAPROLE_STARTED) {
        /* GAP 刚就绪（GAP_ConfigDeviceAddr 此刻才被允许调用）：应用当前槽的专属地址，
         * 使各槽对 PC 表现为不同设备，从根上避免旧绑定被误重连。 */
        bridge_dbg_mark(0x46);
        sdk_ble_gap_ready();
        bridge_dbg_mark(0x47);
    }
}

static hidDevCfg_t s_hid_cfg = {
    BRIDGE_HID_IDLE_TIMEOUT,
    HID_FEATURE_FLAGS};

static hidDevCB_t s_hid_cbs = {
    bridge_hid_report_cb,
    bridge_hid_evt_cb,
    NULL,
    bridge_hid_state_cb};

void bridge_ble_hidsvc_override_cbs(void)
{
    /* TMOS 任务**仅注册一次**（本函数在每次 BLE init 都被调用），
     * 否则持续占用 TMOS 任务槽。 */
    if (s_hid_task == 0xFFu) {
        s_hid_task = TMOS_ProcessEventRegister(bridge_hid_process_event);
    }
    /* HidDev_Register 只保存指针，故后注册者生效（覆盖 hidkbd.c 的 hidEmuHidCBs） */
    HidDev_Register(&s_hid_cfg, &s_hid_cbs);
}

/* ---------------- 服务注册 ---------------- */

/* HID 服务注册结果缓存（排障用）：SDK HidEmu_Init() 丢弃 Hid_AddService() 的返回值，
 * 且模组日志走 UART0 未引出，故在此留存结果供 CMD_HID_DIAG(0x2B) 取回。
 * 0xFF = Hid_AddService() 从未被调用。 */
static uint8_t s_hid_add_status = 0xFFu;

bStatus_t Hid_AddService(void)
{
    bStatus_t status;

    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hidReportKeyInClientCharCfg);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hidReportConsumerClientCharCfg);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hidReportSystemClientCharCfg);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hidReportBootKeyInClientCharCfg);
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, hidReportRawInClientCharCfg);

    status = GATTServApp_RegisterService(hidAttrTbl, GATT_NUM_ATTRS(hidAttrTbl),
                                         GATT_MAX_ENCRYPT_KEY_SIZE, &s_gatt_cbs);
    s_hid_add_status = (uint8_t)status;

    Batt_GetParameter(BATT_PARAM_SERVICE_HANDLE,
                      &GATT_INCLUDED_HANDLE(hidAttrTbl, HID_INCLUDED_SERVICE_IDX));

    hidRptMap[BRIDGE_HID_IDX_KEY_IN].id        = hidReportRefKeyIn[0];
    hidRptMap[BRIDGE_HID_IDX_KEY_IN].type      = hidReportRefKeyIn[1];
    hidRptMap[BRIDGE_HID_IDX_KEY_IN].handle    = hidAttrTbl[HID_REPORT_KEY_IN_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_KEY_IN].cccdHandle = hidAttrTbl[HID_REPORT_KEY_IN_CCCD_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_KEY_IN].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_CONSUMER].id        = hidReportRefConsumer[0];
    hidRptMap[BRIDGE_HID_IDX_CONSUMER].type      = hidReportRefConsumer[1];
    hidRptMap[BRIDGE_HID_IDX_CONSUMER].handle    = hidAttrTbl[HID_REPORT_CONSUMER_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_CONSUMER].cccdHandle = hidAttrTbl[HID_REPORT_CONSUMER_CCCD_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_CONSUMER].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_SYSTEM].id        = hidReportRefSystem[0];
    hidRptMap[BRIDGE_HID_IDX_SYSTEM].type      = hidReportRefSystem[1];
    hidRptMap[BRIDGE_HID_IDX_SYSTEM].handle    = hidAttrTbl[HID_REPORT_SYSTEM_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_SYSTEM].cccdHandle = hidAttrTbl[HID_REPORT_SYSTEM_CCCD_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_SYSTEM].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_LED_OUT].id        = hidReportRefLedOut[0];
    hidRptMap[BRIDGE_HID_IDX_LED_OUT].type      = hidReportRefLedOut[1];
    hidRptMap[BRIDGE_HID_IDX_LED_OUT].handle    = hidAttrTbl[HID_REPORT_LED_OUT_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_LED_OUT].cccdHandle = 0;
    hidRptMap[BRIDGE_HID_IDX_LED_OUT].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_IN].id        = hidReportRefKeyIn[0];
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_IN].type      = hidReportRefKeyIn[1];
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_IN].handle    = hidAttrTbl[HID_BOOT_KEY_IN_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_IN].cccdHandle = hidAttrTbl[HID_BOOT_KEY_IN_CCCD_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_IN].mode      = HID_PROTOCOL_MODE_BOOT;

    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_OUT].id        = hidReportRefLedOut[0];
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_OUT].type      = hidReportRefLedOut[1];
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_OUT].handle    = hidAttrTbl[HID_BOOT_KEY_OUT_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_OUT].cccdHandle = 0;
    hidRptMap[BRIDGE_HID_IDX_BOOT_KEY_OUT].mode      = HID_PROTOCOL_MODE_BOOT;

    hidRptMap[BRIDGE_HID_IDX_FEATURE].id        = hidReportRefFeature[0];
    hidRptMap[BRIDGE_HID_IDX_FEATURE].type      = hidReportRefFeature[1];
    hidRptMap[BRIDGE_HID_IDX_FEATURE].handle    = hidAttrTbl[HID_FEATURE_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_FEATURE].cccdHandle = 0;
    hidRptMap[BRIDGE_HID_IDX_FEATURE].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_RAW_IN].id        = hidReportRefRawIn[0];
    hidRptMap[BRIDGE_HID_IDX_RAW_IN].type      = hidReportRefRawIn[1];
    hidRptMap[BRIDGE_HID_IDX_RAW_IN].handle    = hidAttrTbl[HID_REPORT_RAW_IN_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_RAW_IN].cccdHandle = hidAttrTbl[HID_REPORT_RAW_IN_CCCD_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_RAW_IN].mode      = HID_PROTOCOL_MODE_REPORT;

    hidRptMap[BRIDGE_HID_IDX_RAW_OUT].id        = hidReportRefRawOut[0];
    hidRptMap[BRIDGE_HID_IDX_RAW_OUT].type      = hidReportRefRawOut[1];
    hidRptMap[BRIDGE_HID_IDX_RAW_OUT].handle    = hidAttrTbl[HID_REPORT_RAW_OUT_IDX].handle;
    hidRptMap[BRIDGE_HID_IDX_RAW_OUT].cccdHandle = 0;
    hidRptMap[BRIDGE_HID_IDX_RAW_OUT].mode      = HID_PROTOCOL_MODE_REPORT;

    Batt_GetParameter(BATT_PARAM_BATT_LEVEL_IN_REPORT, &hidRptMap[BRIDGE_HID_IDX_BATTERY]);

    HidDev_RegisterReports(BRIDGE_HID_REPORT_CNT, hidRptMap);
    return status;
}

/* HID 服务注册取证（CMD_HID_DIAG 0x2B，仅排障用）：
 *   out[0]    = s_hid_add_status（GATTServApp_RegisterService 返回值；0xFF=未调用）
 *   out[1..2] = HID 服务首属性 handle（LE；非 0 表示 GATT_RegisterService 已分配句柄）
 *   out[3..4] = Report Map 属性 handle（LE）
 *   out[5..6] = VIA vendor 输入(RID5 IN) 属性 handle（LE）
 *   out[7..8] = VIA vendor 输出(RID5 OUT) 属性 handle（LE）
 *   out[9]    = 报表条数（BRIDGE_HID_REPORT_CNT，应为 9）
 *   out[10]   = 属性表项数（GATT_NUM_ATTRS(hidAttrTbl)，应为 41）
 *   out[11]   = 上次复位源（R8_RESET_STATUS & 0x07，见 sdk_last_reset_cause）
 *   out[12]   = 复位前最后阶段面包屑（bridge_dbg_stage_prev，见 bridge_port_ch58x.c）
 *   out[13]   = 当前阶段面包屑（bridge_dbg_stage_now） */
int port_hid_diag(uint8_t *out, uint8_t *out_len)
{
    if (out == 0 || out_len == 0) {
        return BRIDGE_ERR_PARAM;
    }

    out[0] = s_hid_add_status;
    out[1] = (uint8_t)(hidAttrTbl[HID_SERVICE_IDX].handle & 0xFFu);
    out[2] = (uint8_t)(hidAttrTbl[HID_SERVICE_IDX].handle >> 8);
    out[3] = (uint8_t)(hidAttrTbl[HID_REPORT_MAP_IDX].handle & 0xFFu);
    out[4] = (uint8_t)(hidAttrTbl[HID_REPORT_MAP_IDX].handle >> 8);
    out[5] = (uint8_t)(hidAttrTbl[HID_REPORT_RAW_IN_IDX].handle & 0xFFu);
    out[6] = (uint8_t)(hidAttrTbl[HID_REPORT_RAW_IN_IDX].handle >> 8);
    out[7] = (uint8_t)(hidAttrTbl[HID_REPORT_RAW_OUT_IDX].handle & 0xFFu);
    out[8] = (uint8_t)(hidAttrTbl[HID_REPORT_RAW_OUT_IDX].handle >> 8);
    out[9] = (uint8_t)BRIDGE_HID_REPORT_CNT;
    out[10] = (uint8_t)GATT_NUM_ATTRS(hidAttrTbl);
    out[11] = sdk_last_reset_cause();
    out[12] = bridge_dbg_stage_prev();
    out[13] = bridge_dbg_stage_now();

    *out_len = 14u;
    return BRIDGE_OK;
}

/* ---------------- 参数读写 ---------------- */

uint8_t Hid_SetParameter(uint8_t id, uint8_t type, uint16_t uuid, uint8_t len, void *pValue)
{
    bStatus_t ret = SUCCESS;

    switch (uuid) {
        case REPORT_UUID:
            if (type == HID_REPORT_TYPE_OUTPUT) {
                if (id == BRIDGE_HID_RID_RAW) {
                    if (len == BRIDGE_HID_RAW_REPORT_LEN) {
                        memcpy(hidReportRawOut, pValue, BRIDGE_HID_RAW_REPORT_LEN);
                    } else {
                        ret = ATT_ERR_INVALID_VALUE_SIZE;
                    }
                } else if (len == 1u) {
                    hidReportLedOut = *((uint8_t *)pValue);
                } else {
                    ret = ATT_ERR_INVALID_VALUE_SIZE;
                }
            } else if (type == HID_REPORT_TYPE_FEATURE) {
                if (len == 1u) {
                    hidReportFeature = *((uint8_t *)pValue);
                } else {
                    ret = ATT_ERR_INVALID_VALUE_SIZE;
                }
            } else {
                ret = ATT_ERR_ATTR_NOT_FOUND;
            }
            break;

        case BOOT_KEY_OUTPUT_UUID:
            if (len == 1u) {
                hidReportBootKeyOut = *((uint8_t *)pValue);
            } else {
                ret = ATT_ERR_INVALID_VALUE_SIZE;
            }
            break;

        default:
            break;
    }

    return (ret);
}

uint8_t Hid_GetParameter(uint8_t id, uint8_t type, uint16_t uuid, uint16_t *pLen, void *pValue)
{
    switch (uuid) {
        case REPORT_UUID:
            if (type == HID_REPORT_TYPE_OUTPUT) {
                if (id == BRIDGE_HID_RID_RAW) {
                    memcpy(pValue, hidReportRawOut, BRIDGE_HID_RAW_REPORT_LEN);
                    *pLen = BRIDGE_HID_RAW_REPORT_LEN;
                } else {
                    *((uint8_t *)pValue) = hidReportLedOut;
                    *pLen = 1u;
                }
            } else if (type == HID_REPORT_TYPE_FEATURE) {
                *((uint8_t *)pValue) = hidReportFeature;
                *pLen = 1u;
            } else {
                *pLen = 0u;
            }
            break;

        case BOOT_KEY_OUTPUT_UUID:
            *((uint8_t *)pValue) = hidReportBootKeyOut;
            *pLen = 1u;
            break;

        default:
            *pLen = 0u;
            break;
    }

    return (SUCCESS);
}
