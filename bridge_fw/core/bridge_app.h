/* 应用胶水层：把 core 各模块串成一个主循环
 *
 * 平台无关，目标侧（Bridge_ProcessEvent / main）与主机测试都调用同一份逻辑，
 * 因此主机端跑通的用例即代表目标侧的控制流正确。
 */
#ifndef BRIDGE_APP_H
#define BRIDGE_APP_H

#include "bridge_config.h"
#include "bridge_types.h"

void     bridge_app_init(void);

/* 执行一次主循环体（协议收帧、心跳、模式/电量轮询、排队重发） */
void     bridge_app_poll(void);

/* 中断/事件侧入口：若有字节到达，由 port 层把字节喂给 core 后调用 */
void     bridge_app_on_uart_rx(void);

#endif /* BRIDGE_APP_H */
