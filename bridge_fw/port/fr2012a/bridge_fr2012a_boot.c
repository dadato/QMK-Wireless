/* FR2012A 上电引导 + 角色入口
 *
 * 与 CH58x 的差异（见 port/fr2012a/bridge_chip.h 注释）：
 *   1. 厂商 ROM 启动流程调用用户 `user_main()`，因此本文件**不定义 main()**；
 *   2. BLE 闭源库要求两个 jump_table 段：
 *        jump_table_1 = 镜像类型/大小（OTA 与 ROM 自检用）
 *        jump_table_3 = 栈顶地址（ROM 用它设置 MSP）
 *   3. 协议栈调度由 `ble_stack_schedule()` 驱动，放在本文件的主循环里，
 *      保持 bridge_chip_poll() 不含任何厂商头文件。
 *
 * 对应 CH58x：port/ch58x/bridge_ch58x_sdk.c 的 sdk_sys_init/sdk_ble_stack_init
 * + port/ch58x/bridge_ch58x.c 的 bridge_ch58x_init/loop。
 */
#include <string.h>

#include "plf.h"
#include "driver_system.h"
#include "driver_pmu.h"
#include "jump_table.h"
#include "ble_stack.h"
#include "gap_api.h"
#include "sys_utils.h"

#include "app_config.h"

#include "bridge_chip.h"
#include "bridge_fr2012a.h"
#include "bridge_fr2012a_sdk.h"
#include "bridge_ble_fr2012a.h"
#include "bridge_gatt_fr2012a.h"
#include "bridge_usb_fr2012a.h"
#include "bridge_app.h"
#include "bridge_log.h"

#define BRIDGE_MOD_BOOT "boot"

/* 厂商库里没有对外声明（只有库内符号），此处自行声明 */
extern void patch_init(void);

#define FR_STACK_SIZE 0x800u

/* ROM 用的栈：由 jump_table_3.stack_top_address 指向其顶端（见 ld/app.ld） */
static uint32_t s_stack[FR_STACK_SIZE / sizeof(uint32_t)]
    __attribute__((section("stack_section")));

const struct jump_table_version_t _jump_table_version
    __attribute__((section("jump_table_3"))) = {
        .stack_top_address = &s_stack[FR_STACK_SIZE / sizeof(uint32_t)],
        .firmware_version  = 0x00000001u,
};

const struct jump_table_image_t _jump_table_image
    __attribute__((section("jump_table_1"))) = {
        .image_size = 0x32000u,
        .image_type = IMAGE_TYPE_APP,
};

static void fr_ble_stack_boot(void)
{
    uint8_t     mac[6];
    mac_addr_t  addr;

    if ((sdk_mac_get(mac) == 0) &&
        !((mac[0] == 0xFFu) && (mac[1] == 0xFFu) && (mac[2] == 0xFFu) &&
          (mac[3] == 0xFFu) && (mac[4] == 0xFFu) && (mac[5] == 0xFFu))) {
        memcpy(addr.addr, mac, sizeof(addr.addr));
        gap_address_set(&addr, BLE_ADDR_TYPE_PUBLIC);
    }

    (void)ble_stack_configure(BLE_STACK_ENABLE_MESH,
                              BLE_STACK_ENABLE_CONNECTIONS,
                              BLE_STACK_RX_BUFFER_CNT,
                              BLE_STACK_RX_BUFFER_SIZE,
                              BLE_STACK_TX_BUFFER_CNT,
                              BLE_STACK_TX_BUFFER_SIZE,
                              BLE_STACK_ADV_BUFFER_SIZE,
                              BLE_STACK_RETENTION_RAM_SIZE,
                              BLE_STACK_KEY_STORAGE_OFFSET);
    ble_stack_init();
    patch_init();

    /* 绑定信息 / 对端服务表分别落在 0x7D000 / 0x7E000（避开本工程 NV 0x7A000） */
    gap_bond_manager_init(BLE_BONDING_INFO_SAVE_ADDR,
                          BLE_REMOTE_SERVICE_SAVE_ADDR, 8, true);
}

void bridge_chip_boot(void)
{
    sdk_sys_init();             /* 时钟 / Flash / GPIO */

    fr_ble_stack_boot();        /* MAC + 协议栈 + 绑定管理 */
    /* 厂商快速响应（double rate）：必须在 patch_init() 之后调用；
     * 本包库未导出 → 返回 false，连接间隔回落到 7.5ms。 */
    (void)sdk_rf_fast_response_enable();
    sdk_rf_set_tx_power_max();
    sdk_tick_init();            /* 1ms 时基（协议栈 os_timer 就绪后） */

    bridge_fr_ble_init();       /* GAP 回调 / 安全参数 / HID + 桥服务注册 */
    bridge_gatt_init();

    bridge_app_init();          /* core：日志/GPIO/NV/串口/HID/协议/模式 */

#if BRIDGE_BOARD_IS_DONGLE
    bridge_usb_fr2012a_init();  /* 接收器：USB HID 复合设备 */
#endif

    LOG_I(BRIDGE_MOD_BOOT, "fr2012a up (%s)",
          BRIDGE_BOARD_IS_DONGLE ? "dongle" : "keyboard");
}

/* 厂商 ROM 的 main() 会调用本函数（见 components/toolchain/gcc/boot_vectors.c） */
void user_main(void)
{
    bridge_chip_boot();

    for (;;) {
        if (ble_stack_schedule_allow()) {
            ble_stack_schedule();
        }
        bridge_chip_poll();
    }
}
