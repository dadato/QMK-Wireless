/* CH582F 透传桥 —— 目标侧 main（键盘侧 / Dongle 侧共用）
 *
 * 两个构建目标共用本文件，靠 board/<target>/bridge_board.h 区分：
 *   keyboard : -Iboard/keyboard
 *   dongle   : -Iboard/dongle
 * （BRIDGE_BOARD_IS_DONGLE / BRIDGE_BOARD_HAS_USB 由被选中的板级头文件给出，
 *   无需在命令行重复定义，见 Makefile）
 *
 * 设计说明（与清单 §5 的差异，已在 checklist 中同步）：
 *   目标侧**不使用 TMOS 任务/事件驱动** bridge 逻辑。串口帧占线短（921600
 *   下 64B 载荷帧约 0.7ms），事件驱动只会叠加调度延迟；故由 Main_Circulation 直接调用
 *   bridge_app_poll()，TMOS 只用于服务 BLE 协议栈（sdk_tmos_process）。
 */
#include "bridge_ch58x.h"
#include "bridge_ch58x_sdk.h"

int main(void)
{
    /* 1. 系统时钟等基础初始化（必须最先执行）
     *    并立即抓取复位源（R8_RESET_STATUS），供 CMD_HID_DIAG 排障上报。 */
    sdk_sys_init();
    sdk_capture_reset_cause();

    /* 2. 射频/协议栈基础初始化 + USB(Dongle) + core 初始化
     *    core 内部完成：日志 → GPIO → 配置 → 桥串口 → HID → 电量 → 协议 → 模式
     *    模式初始化会按 MODE_SEL / 上次记忆初始化 BLE 或 2.4G 射频。 */
    bridge_ch58x_init();

    /* 3. 主循环：TMOS 服务 → 串口 TX 排空 → 配对键 → bridge_app_poll()；
     *    尾部按状态择一进睡（深睡 MODE_SLEEP / 浅睡保持链路），唤醒后继续。 */
    for (;;) {
        bridge_ch58x_loop();
        sdk_usb_poll();     /* USB 远程唤醒：重枚举完成后补发唤醒前的按键（键盘侧空实现） */
        bridge_ch58x_idle();
    }
}
