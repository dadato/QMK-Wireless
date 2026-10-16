/* 芯片适配层（port/<chip>）对上层暴露的**唯一**入口。
 *
 * 分层约定（换无线芯片 = 新增 port/<chip>/ 并保持本头同名同签名）：
 *
 *   app/main.c ──bridge_chip.h──► port/<chip>  ──► core/（bridge_port.h）
 *
 * app/ 层只依赖本头，不引用任何芯片专有符号；因此移植到新芯片时
 * app/、core/、web/ 均无需改动，只需在 port/<newchip>/ 下提供：
 *   - bridge_chip.h      本头（同名）
 *   - bridge_chip_boot() 系统时钟/复位源/射频与 core 初始化
 *   - bridge_chip_poll() 每轮主循环服务
 *   - bridge_port.h 中全部 port_* 实现（含射频/USB/NV/日志）
 *   - port.mk            该芯片的构建描述（SDK 路径、源文件、BOOT 镜像）
 */
#ifndef BRIDGE_CHIP_H
#define BRIDGE_CHIP_H

/* 上电引导：系统时钟等基础初始化（必须最先执行）→ 抓取复位源 → 射频/协议栈
 * 与 core 初始化（桥串口 / HID / 电量 / 协议 / 模式）。 */
void bridge_chip_boot(void);

/* 每轮主循环：服务协议栈与桥逻辑 → 按当前状态择一进入深睡/浅睡，唤醒后返回。 */
void bridge_chip_poll(void);

#endif /* BRIDGE_CHIP_H */
