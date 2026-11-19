/* 本工程自有的 BLE 电池服务实现（替代 SDK 例程 Profile/battservice.c）
 *
 * 提供内容（与厂商文件同名同签名，供 hiddev.c / bridge_ble_hidsvc.c 链接）：
 *   Batt_AddService() / Batt_Register() / Batt_SetParameter() /
 *   Batt_GetParameter() / Batt_MeasLevel() / Batt_Setup() /
 *   Batt_HandleConnStatusCB()
 *
 * 替换原因（修复「模组电量变化但 BLE 主机电量不变」）：
 *   厂商实现的电量来源与模组真实电量无关：
 *     - battMeasure() 硬编码 adc = 300，且 Batt_Setup() 从未被调用
 *       → 恒走回退公式算出常量 20%；
 *     - battReadAttrCB() / Batt_MeasLevel() 只在 level < battLevel
 *       时更新（单调下降语义），电量回升方向永不刷新；
 *     - Batt_SetParameter() 无 BATT_PARAM_LEVEL 写分支。
 *   故主机读到的电量恒定不变。
 *
 * 本实现：
 *   1) 电量直接取自本工程电源模块 bridge_power_battery_percent()
 *      （唯一来源：SK32 经 CMD_SET_BATTERY 下发的百分比，模组不做本地采样）；
 *   2) 读路径与周期测量路径均**双向**刷新 battLevel 并在变化时通知。
 *
 * 属性表布局（服务声明 / 电量声明 / 电量值 / 电量 CCCD / Report Reference）
 * 与厂商完全一致，以保证 HID 服务 Included Service 引用与报告句柄兼容。
 *
 * 头文件依赖：仅本文件接触 WCH SDK 头。
 */
#include "CONFIG.h"
#include "hiddev.h"
#include "battservice.h"

#include "bridge_power.h"

/*********************************************************************
 * MACROS
 */
#define BATT_LEVEL_VALUE_IDX         2    // 电量值在属性表的下标
#define BATT_LEVEL_VALUE_CCCD_IDX    3    // 电量 CCCD 在属性表的下标
#define BATT_LEVEL_VALUE_LEN         1

/*********************************************************************
 * GLOBAL VARIABLES
 */
// Battery service
const uint8_t battServUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(BATT_SERV_UUID), HI_UINT16(BATT_SERV_UUID)};

// Battery level characteristic
const uint8_t battLevelUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(BATT_LEVEL_UUID), HI_UINT16(BATT_LEVEL_UUID)};

/*********************************************************************
 * LOCAL VARIABLES
 */

// Application callback
static battServiceCB_t battServiceCB;

// Measurement setup/teardown/calculation callbacks（保留 API，仅本实现不再使用）
static battServiceSetupCB_t    battServiceSetupCB = NULL;
static battServiceTeardownCB_t battServiceTeardownCB = NULL;
static battServiceCalcCB_t     battServiceCalcCB = NULL;

static uint16_t battMinLevel = 0;
static uint16_t battMaxLevel = 0;

// Critical battery level setting
static uint8_t battCriticalLevel;

/*********************************************************************
 * Profile Attributes - variables
 */

// Battery Service attribute
static const gattAttrType_t battService = {ATT_BT_UUID_SIZE, battServUUID};

// Battery level characteristic
static uint8_t       battLevelProps = GATT_PROP_READ | GATT_PROP_NOTIFY;
static uint8_t       battLevel = 100;
static gattCharCfg_t battLevelClientCharCfg[GATT_MAX_NUM_CONN];

// HID Report Reference characteristic descriptor, battery level
static uint8_t hidReportRefBattLevel[HID_REPORT_REF_LEN] = {
    HID_RPT_ID_BATT_LEVEL_IN, HID_REPORT_TYPE_INPUT};

/*********************************************************************
 * Profile Attributes - Table
 */

static gattAttribute_t battAttrTbl[] = {
    // Battery Service
    {
        {ATT_BT_UUID_SIZE, primaryServiceUUID}, /* type */
        GATT_PERMIT_READ,                       /* permissions */
        0,                                      /* handle */
        (uint8_t *)&battService                 /* pValue */
    },

    // Battery Level Declaration
    {
        {ATT_BT_UUID_SIZE, characterUUID},
        GATT_PERMIT_READ,
        0,
        &battLevelProps},

    // Battery Level Value
    {
        {ATT_BT_UUID_SIZE, battLevelUUID},
        GATT_PERMIT_READ,
        0,
        &battLevel},

    // Battery Level Client Characteristic Configuration
    {
        {ATT_BT_UUID_SIZE, clientCharCfgUUID},
        GATT_PERMIT_READ | GATT_PERMIT_WRITE,
        0,
        (uint8_t *)&battLevelClientCharCfg},

    // HID Report Reference characteristic descriptor, battery level input
    {
        {ATT_BT_UUID_SIZE, reportRefUUID},
        GATT_PERMIT_READ,
        0,
        hidReportRefBattLevel}
};

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static bStatus_t battReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method);
static bStatus_t battWriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                 uint8_t *pValue, uint16_t len, uint16_t offset, uint8_t method);
static void      battNotifyCB(linkDBItem_t *pLinkItem);
static uint8_t   battMeasure(void);
static void      battNotifyLevel(void);

/*********************************************************************
 * PROFILE CALLBACKS
 */
gattServiceCBs_t battCBs = {
    battReadAttrCB,  // Read callback function pointer
    battWriteAttrCB, // Write callback function pointer
    NULL             // Authorization callback function pointer
};

/*********************************************************************
 * @fn      Batt_AddService
 *
 * @brief   Initializes the Battery Service by registering
 *          GATT attributes with the GATT server.
 *
 * @return  Success or Failure
 */
bStatus_t Batt_AddService(void)
{
    uint8_t status = SUCCESS;

    // Initialize Client Characteristic Configuration attributes
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, battLevelClientCharCfg);

    // Register GATT attribute list and CBs with GATT Server App
    status = GATTServApp_RegisterService(battAttrTbl,
                                         GATT_NUM_ATTRS(battAttrTbl),
                                         GATT_MAX_ENCRYPT_KEY_SIZE,
                                         &battCBs);

    return (status);
}

/*********************************************************************
 * @fn      Batt_Register
 *
 * @brief   Register a callback function with the Battery Service.
 *
 * @param   pfnServiceCB - Callback function.
 *
 * @return  None.
 */
extern void Batt_Register(battServiceCB_t pfnServiceCB)
{
    battServiceCB = pfnServiceCB;
}

/*********************************************************************
 * @fn      Batt_SetParameter
 *
 * @brief   Set a Battery Service parameter.
 *
 * @param   param - Profile parameter ID
 * @param   len - length of data to right
 * @param   value - pointer to data to write.
 *
 * @return  bStatus_t
 */
bStatus_t Batt_SetParameter(uint8_t param, uint8_t len, void *value)
{
    bStatus_t ret = SUCCESS;

    switch(param)
    {
        case BATT_PARAM_CRITICAL_LEVEL:
            battCriticalLevel = *((uint8_t *)value);

            // If below the critical level and critical state not set, notify it
            if(battLevel < battCriticalLevel)
            {
                battNotifyLevel();
            }
            break;

        default:
            ret = INVALIDPARAMETER;
            break;
    }

    return (ret);
}

/*********************************************************************
 * @fn      Batt_GetParameter
 *
 * @brief   Get a Battery Service parameter.
 *
 * @param   param - Profile parameter ID
 * @param   value - pointer to data to get.
 *
 * @return  bStatus_t
 */
bStatus_t Batt_GetParameter(uint8_t param, void *value)
{
    bStatus_t ret = SUCCESS;
    switch(param)
    {
        case BATT_PARAM_LEVEL:
            *((uint8_t *)value) = battLevel;
            break;

        case BATT_PARAM_CRITICAL_LEVEL:
            *((uint8_t *)value) = battCriticalLevel;
            break;

        case BATT_PARAM_SERVICE_HANDLE:
            *((uint16_t *)value) = GATT_SERVICE_HANDLE(battAttrTbl);
            break;

        case BATT_PARAM_BATT_LEVEL_IN_REPORT:
        {
            hidRptMap_t *pRpt = (hidRptMap_t *)value;

            pRpt->id = hidReportRefBattLevel[0];
            pRpt->type = hidReportRefBattLevel[1];
            pRpt->handle = battAttrTbl[BATT_LEVEL_VALUE_IDX].handle;
            pRpt->cccdHandle = battAttrTbl[BATT_LEVEL_VALUE_CCCD_IDX].handle;
            pRpt->mode = HID_PROTOCOL_MODE_REPORT;
        }
        break;

        default:
            ret = INVALIDPARAMETER;
            break;
    }

    return (ret);
}

/*********************************************************************
 * @fn          Batt_MeasLevel
 *
 * @brief       Measure the battery level and update the battery
 *              level value in the service characteristics.  If
 *              the battery level-state characteristic is configured
 *              for notification and the battery level has changed
 *              since the last measurement, then a notification
 *              will be sent.
 *
 * @return      Success
 */
bStatus_t Batt_MeasLevel(void)
{
    uint8_t level;

    level = battMeasure();

    // 双向刷新：电量升降均更新并通知（厂商原实现仅在下降时更新）
    if(level != battLevel)
    {
        battLevel = level;
        battNotifyLevel();
    }

    return SUCCESS;
}

/*********************************************************************
 * @fn      Batt_Setup
 *
 * @brief   Set up which ADC source is to be used. 本实现电量取自
 *          bridge_power 模块，此接口仅为保持 API 兼容（no-op）。
 *
 * @return  none.
 */
void Batt_Setup(uint8_t adc_ch, uint16_t minVal, uint16_t maxVal,
                battServiceSetupCB_t sCB, battServiceTeardownCB_t tCB,
                battServiceCalcCB_t cCB)
{
    (void)adc_ch;

    battMinLevel = minVal;
    battMaxLevel = maxVal;

    battServiceSetupCB = sCB;
    battServiceTeardownCB = tCB;
    battServiceCalcCB = cCB;
}

/*********************************************************************
 * @fn          battReadAttrCB
 *
 * @brief       Read an attribute.
 *
 * @return      Success or Failure
 */
static bStatus_t battReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method)
{
    uint16_t  uuid;
    bStatus_t status = SUCCESS;

    // Make sure it's not a blob operation (no attributes in the profile are long)
    if(offset > 0)
    {
        return (ATT_ERR_ATTR_NOT_LONG);
    }

    uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);

    // Measure battery level if reading level
    if(uuid == BATT_LEVEL_UUID)
    {
        // 实时取模组电量并刷新缓存，使读取路径也反映最新电量（双向）
        battLevel = battMeasure();

        *pLen = BATT_LEVEL_VALUE_LEN;
        pValue[0] = battLevel;
    }
    else if(uuid == GATT_REPORT_REF_UUID)
    {
        *pLen = HID_REPORT_REF_LEN;
        tmos_memcpy(pValue, pAttr->pValue, HID_REPORT_REF_LEN);
    }
    else
    {
        status = ATT_ERR_ATTR_NOT_FOUND;
    }

    return (status);
}

/*********************************************************************
 * @fn      battWriteAttrCB
 *
 * @brief   Validate attribute data prior to a write operation
 *
 * @return  Success or Failure
 */
static bStatus_t battWriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                 uint8_t *pValue, uint16_t len, uint16_t offset, uint8_t method)
{
    bStatus_t status = SUCCESS;

    uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
    switch(uuid)
    {
        case GATT_CLIENT_CHAR_CFG_UUID:
            status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len,
                                                    offset, GATT_CLIENT_CFG_NOTIFY);
            if(status == SUCCESS)
            {
                uint16_t charCfg = BUILD_UINT16(pValue[0], pValue[1]);

                if(battServiceCB)
                {
                    (*battServiceCB)((charCfg == GATT_CFG_NO_OPERATION) ? BATT_LEVEL_NOTI_DISABLED : BATT_LEVEL_NOTI_ENABLED);
                }
            }
            break;

        default:
            status = ATT_ERR_ATTR_NOT_FOUND;
            break;
    }

    return (status);
}

/*********************************************************************
 * @fn          battNotifyCB
 *
 * @brief       Send a notification of the level state characteristic.
 *
 * @param       pLinkItem - linkDB item
 *
 * @return      None.
 */
static void battNotifyCB(linkDBItem_t *pLinkItem)
{
    if(pLinkItem->stateFlags & LINK_CONNECTED)
    {
        uint16_t value = GATTServApp_ReadCharCfg(pLinkItem->connectionHandle,
                                                 battLevelClientCharCfg);
        if(value & GATT_CLIENT_CFG_NOTIFY)
        {
            attHandleValueNoti_t noti;

            noti.pValue = GATT_bm_alloc(pLinkItem->connectionHandle, ATT_HANDLE_VALUE_NOTI,
                                        BATT_LEVEL_VALUE_LEN, NULL, 0);
            if(noti.pValue != NULL)
            {
                noti.handle = battAttrTbl[BATT_LEVEL_VALUE_IDX].handle;
                noti.len = BATT_LEVEL_VALUE_LEN;
                noti.pValue[0] = battLevel;

                if(GATT_Notification(pLinkItem->connectionHandle, &noti, FALSE) != SUCCESS)
                {
                    GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
                }
            }
        }
    }
}

/*********************************************************************
 * @fn      battMeasure
 *
 * @brief   Return the module battery level as a percentage 0-100%.
 *
 * @return  Battery level.
 */
static uint8_t battMeasure(void)
{
    uint8_t percent = bridge_power_battery_percent();

    // 规整到 0..100（BLE 电量特征取值范围）
    if(percent > 100)
    {
        percent = 100;
    }

    return percent;
}

/*********************************************************************
 * @fn      battNotifyLevel
 *
 * @brief   Send a notification of the battery level state
 *          characteristic if a connection is established.
 *
 * @return  None.
 */
static void battNotifyLevel(void)
{
    // Execute linkDB callback to send notification
    linkDB_PerformFunc(battNotifyCB);
}

/*********************************************************************
 * @fn          Batt_HandleConnStatusCB
 *
 * @brief       Battery Service link status change handler function.
 *
 * @param       connHandle - connection handle
 * @param       changeType - type of change
 *
 * @return      none
 */
void Batt_HandleConnStatusCB(uint16_t connHandle, uint8_t changeType)
{
    // Make sure this is not loopback connection
    if(connHandle != LOOPBACK_CONNHANDLE)
    {
        // Reset Client Char Config if connection has dropped
        if((changeType == LINKDB_STATUS_UPDATE_REMOVED) ||
           ((changeType == LINKDB_STATUS_UPDATE_STATEFLAGS) &&
            (!linkDB_Up(connHandle))))
        {
            GATTServApp_InitCharCfg(connHandle, battLevelClientCharCfg);
        }
    }
}

/*********************************************************************
*********************************************************************/
