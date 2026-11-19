/* 芯片适配层（port/fr2012a）对上层暴露的**唯一**入口。
 *
 * 与 port/ch58x/bridge_chip.h 同名同签名，因此 app/、core/、web/ 均无需改动：
 *
 *   app/main.c（或本端口自带的 user_main）──bridge_chip.h──► port/fr2012a ──► core/
 *
 * FR2012A（FreqChip FR800x，Cortex-M3）与 CH58x 的差异只在 port 层：
 *   - 厂商 ROM 启动流程要求用户实现 `user_main()`（而非 `main()`），
 *     由 port/fr2012a/bridge_fr2012a_boot.c 提供；
 *   - BLE 协议栈是闭源库 libfr800x_stack.a，需 jump_table 段描述镜像与栈顶；
 *   - 本芯片只有 BLE 射频，`BRIDGE_RADIO_2G4` 在 v1 映射为「与接收器的私有 BLE 链路」。
 */
#ifndef BRIDGE_CHIP_H
#define BRIDGE_CHIP_H

/* 上电引导：系统时钟 → MAC/BLE 协议栈 → 桥服务注册 → core 初始化。 */
void bridge_chip_boot(void);

/* 每轮主循环：串口 TX 排空 → BLE 连接维护 → USB(Dongle) → core 主循环。 */
void bridge_chip_poll(void);

#endif /* BRIDGE_CHIP_H */
