/* CH582F/CH583 透传桥 —— 角色应用入口（模组 / 接收器共用）
 *
 * 两个构建目标共用本文件，靠 app/<target>/bridge_board.h 区分：
 *   keyboard : -Iapp/keyboard   （模组：键盘侧无线模组）
 *   dongle   : -Iapp/dongle     （接收器：USB Dongle）
 * （BRIDGE_BOARD_IS_DONGLE / BRIDGE_BOARD_HAS_USB 由被选中的板级头文件给出，
 *   无需在命令行重复定义，见 Makefile。）
 *
 * 本文件**不引用任何芯片专有符号**：初始化与主循环全部经 port/<chip>/bridge_chip.h
 * 暴露的 bridge_chip_boot()/bridge_chip_poll() 入口（见该头分层说明）。
 *
 * 设计说明（与清单 §5 的差异，已在 checklist 中同步）：
 *   目标侧**不使用 TMOS 任务/事件驱动** bridge 逻辑。串口帧占线短（921600
 *   下 64B 载荷帧约 0.7ms），事件驱动只会叠加调度延迟；故由主循环直接调用
 *   bridge_app_poll()，TMOS 只用于服务 BLE 协议栈（sdk_tmos_process）。
 */
#include "bridge_chip.h"

int main(void)
{
    /* 1. 系统时钟等基础初始化 + 抓取复位源（供 CMD_HID_DIAG 排障上报）
     * 2. 射频/协议栈基础初始化 + USB(接收器) + core 初始化 */
    bridge_chip_boot();

    /* 3. 主循环：TMOS 服务 → 串口 TX 排空 → 配对键 → bridge_app_poll()；
     *    尾部按状态择一进睡（深睡 MODE_SLEEP / 浅睡保持链路），唤醒后继续。 */
    for (;;) {
        bridge_chip_poll();
    }
}
