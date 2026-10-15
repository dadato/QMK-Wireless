/* CH58x SDK 原语边界（平台唯一接触点）
 *
 * 设计目的
 *   1. 全工程**只有 bridge_ch58x_sdk.c 一个文件** #include WCH SDK 头；
 *      bridge_port_ch58x.c / bridge_radio_ch58x.c 只依赖本头文件，
 *      因此可在 PC 上用 test_host/sdk_stub.c 顶替后真编译真运行，
 *      使 port 层逻辑（波特率、TX 背压、NV 布局、ADC 换算、射频状态翻译）
 *      获得与 core 同等强度的主机验证。
 *   2. 本头文件里的函数全部是 SDK 调用的**机械映射**，不含业务逻辑。
 *
 * API 名称基线：WCH CH583 EVT —— openwch/ch583 @ bd508ad7（本工程 submodule）。
 * 该基线下的 API 名已逐条核对完毕；本实现内无遗留 TODO。
 */
#ifndef BRIDGE_CH58X_SDK_H
#define BRIDGE_CH58X_SDK_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 系统 / 时间 ---------------- */
void     sdk_sys_init(void);                  /* SetSysClock 等基础初始化，必须最先调用 */
uint32_t sdk_sysclk_hz(void);                 /* GetSysClock() */
uint32_t sdk_millis(void);                    /* TMOS_GetSystemClock() / 0.625ms → ms */
void     sdk_delay_ms(uint32_t ms);           /* DelayMs() */
void     sdk_reset(void);                     /* SYS_ResetExecute() */
/* 复位源取证：启动最早阶段调用一次，抓取 R8_RESET_STATUS 低 3 位
 * （RST_FLAG_SW/RPOR/WTR/MR/GPWSM），供 CMD_HID_DIAG 上报排障。 */
void     sdk_capture_reset_cause(void);
uint8_t  sdk_last_reset_cause(void);

/* 复位前阶段面包屑（看门狗复位排障，临时）：详见 bridge_port_ch58x.c 定义处。
 * prev = 复位前最后执行到的阶段；now = 当前阶段。经 CMD_HID_DIAG 上报。 */
uint8_t  bridge_dbg_stage_prev(void);
uint8_t  bridge_dbg_stage_now(void);
void     bridge_dbg_mark(uint8_t st);   /* 在跨文件回调链中打点（见 bridge_port_ch58x.c） */

void     sdk_enter_bootloader(void);          /* 跳 BootLoader（有线 ISP 恢复通道）*/
void     sdk_ble_mac(uint8_t mac[6]);         /* BLE 公共设备地址（广播用） */

/* ---------------- UART1：桥数据面（PA8=RXD1 / PA9=TXD1） ---------------- */
void     sdk_uart1_init(uint32_t baud);       /* 复用脚 + 波特率 + 8 级 FIFO + 唤醒源 */
void     sdk_uart1_rx_irq_enable(bool en);    /* RB_IER_RECV_RDY | RB_IER_LINE_STAT */
void     sdk_uart1_write(const uint8_t *d, uint16_t len); /* 阻塞写满 TX FIFO 即返回 */
uint8_t  sdk_uart1_tx_bytes(void);            /* R8_UART1_TFC：TX FIFO 中待发字节数 */
uint8_t  sdk_uart1_rx_bytes(void);            /* R8_UART1_RFC：RX FIFO 中可读字节数 */
uint8_t  sdk_uart1_rx_read(void);             /* R8_UART1_RBR */
uint8_t  sdk_uart1_iir(void);                 /* R8_UART1_IIR：读后清中断 */
uint8_t  sdk_uart1_lsr(void);                 /* R8_UART1_LSR：读后清溢出/帧错（LINE_STAT） */
/* R8_UART1_LSR 位掩码（见 CH583SFR.h / CH58x_uart.h，收侧使能 LINE_STAT）。
 * 用于把 ISR 观测到的「线路状态错误」分类计数，定位字节级丢失根因。 */
#define SDK_UART1_LSR_OVER_ERR   0x02u        /* RB_LSR_OVER_ERR：接收 FIFO 溢出（overrun） */
#define SDK_UART1_LSR_PAR_ERR    0x04u        /* RB_LSR_PAR_ERR ：校验错 */
#define SDK_UART1_LSR_FRAME_ERR  0x08u        /* RB_LSR_FRAME_ERR：帧错（stop bit / 断线） */
/* Sleep 期间 UART 不能唤醒内核：把 RXD1(PA8) 叠加为下降沿 GPIO 唤醒源。
 * 引脚保持 UART 复用不变；由端口层在进睡窗口内开关（避免清醒期每字节中断）。 */
void     sdk_uart1_rx_wake_enable(bool en);
/* 读并清除「上一个睡眠窗口内 RXD1 唤醒过」标志：port 层据此续期
 * 「接收活动保持清醒」窗口（见 BRIDGE_RX_HOLD_AWAKE_MS）。 */
bool     sdk_uart1_rx_woke(void);

/* ---------------- UART0：printf 日志（PB4=RXD0 / PB7=TXD0） ---------------- */
void     sdk_uart0_init(uint32_t baud);
void     sdk_uart0_write(const uint8_t *d, uint16_t len);

/* ---------------- GPIO（port 用 ASCII 'A'/'B'） ---------------- */
void     sdk_gpio_out_init(uint8_t port, uint8_t pin, bool high);
void     sdk_gpio_in_pullup(uint8_t port, uint8_t pin);
void     sdk_gpio_in_pulldown(uint8_t port, uint8_t pin);
void     sdk_gpio_adc_in(uint8_t port, uint8_t pin);
void     sdk_gpio_write(uint8_t port, uint8_t pin, bool high);
bool     sdk_gpio_read(uint8_t port, uint8_t pin);

/* ---------------- ADC ---------------- */
/* 单次转换并换算为引脚上的毫伏值（参考电压/校准由 SDK 层负责） */
uint16_t sdk_adc_read_mv(uint8_t ch);

/* ---------------- Flash / DataFlash ---------------- */
/* CH58x 地址空间：CodeFlash 0x00000-0x6FFFF，DataFlash 0x70000-0x77FFF，
 * InfoFlash 0x78000-0x79FFF。以下宏供 port 层规划 NV 布局使用。 */
#define SDK_DATAFLASH_BASE   0x70000u
#define SDK_DATAFLASH_SIZE   0x8000u   /* 32KB */
#define SDK_FLASH_ERASE_UNIT 256u      /* DataFlash 擦除粒度；CodeFlash 为 4KB */

int      sdk_flash_read(uint32_t addr, void *buf, uint16_t len);
int      sdk_flash_write(uint32_t addr, const void *buf, uint16_t len);
int      sdk_flash_erase(uint32_t addr, uint16_t len);   /* 地址与长度须 256B 对齐 */

/* ---------------- 固件升级（键盘侧 UART OTA，WCH IAP 分区） ----------------
 * 暂存区与标志位布局见同目录 bridge_ota_layout.h。 */
int      sdk_fw_begin(uint32_t total_len);                       /* 校验长度、复位擦除进度 */
int      sdk_fw_write(uint32_t offset, const uint8_t *d, uint16_t len); /* 顺序写，按需擦 4K */
void     sdk_fw_finish(uint16_t crc);   /* 写 IAP 标志记录 + 复位（不复返） */
void     sdk_fw_abort(void);
/* 通用 OTA 标志写入 + 复位（不复返）：len/crc 清 0。
 * Dongle App 收到 HID ENTER_OTA 后写 USB_IAP 标志，重启进常驻 IAP 的 USB 收包模式。 */
void     sdk_ota_flag_set_reset(uint8_t flag);

/* ---------------- 低功耗 ---------------- */
/* 进入低功耗直至下次唤醒源；内部保证至少 max_ms 后唤醒一次 */
void     sdk_sleep(uint32_t max_ms);

/* ---------------- 看门狗（WDOG，M6） ---------------- */
void     sdk_wdog_init(uint16_t timeout_ms);   /* 0 = 关闭；上限约 557ms（60MHz） */
void     sdk_wdog_feed(void);                  /* 主循环每轮喂狗 */
void     sdk_wdog_suspend(void);               /* 长睡眠前：停复位使能（计数保留） */
void     sdk_wdog_resume(void);                /* 唤醒后：重挂并重新使能复位 */

/* ---------------- BLE（HOGP 外设） ---------------- */
int      sdk_ble_stack_init(void);            /* CH58x_BLEInit + HAL_Init + GAPRole + GATTS + HidDev，幂等 */
int      sdk_ble_deinit(void);                /* 停广播 + 断开连接（协议栈本身不可反初始化） */
/* 发送 HOGP Input Report：rid = BRIDGE_HID_RID_*（见 bridge_ble_hidsvc.h），
 * 载荷不含 Report ID（Report ID 由 GATT Report Reference 描述符告知主机）。 */
int      sdk_ble_hid_send(uint8_t rid, const uint8_t *d, uint8_t len);
/* 出站 notify 瞬态失败的诊断计数与延迟重试（见 bridge_ch58x_sdk.c）：
 * 主循环每轮调 sdk_ble_notify_poll() 补发上一轮失败的暂存帧；
 * *_count()（32 位）供 port_hid_diag 读出，用于区分模组侧丢帧与主机侧丢弃。 */
void     sdk_ble_notify_poll(void);
uint32_t sdk_ble_notify_fail_count(void);
uint32_t sdk_ble_notify_retry_ok_count(void);
uint32_t sdk_ble_notify_retry_drop_count(void);
bool     sdk_ble_is_connected(void);
int      sdk_ble_pair_start(uint16_t timeout_s);   /* 开可发现广播：仅配对窗口业务入口 */
int      sdk_ble_pair_stop(void);                  /* 撤广播（窗口退出 / 切槽前奏） */
int      sdk_ble_adv_resume(void);                 /* 待连：开可连接不可发现广播（常规模式 / 掉线恢复） */
void     sdk_ble_on_gap_waiting(void);             /* GAPROLE_WAITING：按意图回待连；供 GAP 状态回调调用 */
int      sdk_ble_connect_profile(uint8_t profile); /* 1..4：多绑定槽选路 */
int      sdk_ble_unpair(uint8_t profile);          /* 0xFF = 全部（含 bond 记录清除） */
bool     sdk_ble_resume_pending(void);             /* 断链后补做「有连接期间发起的切槽」；供 GAP 状态回调调用 */
void     sdk_ble_gap_ready(void);                  /* GAPROLE_STARTED 后应用当前槽专属 BLE 地址；供 GAP 状态回调调用 */
int      sdk_ble_set_conn_interval(uint8_t units_1p25ms);
int      sdk_ble_set_adv_name_with_mac(bool en);   /* 广播名是否带 MAC 后缀 */
int      sdk_ble_set_adv_name(const uint8_t *name, uint8_t len); /* 广播名（CMD_SET_NAME target=BLE） */
int      sdk_ble_set_ids(uint16_t vid, uint16_t pid); /* 写 BLE DIS PnP ID（CMD_SET_USB_ID 同步） */
int      sdk_ble_test_mode(uint8_t mode, uint8_t channel, uint8_t power); /* SRRC 定频 */
uint8_t  sdk_ble_protocol_ver(void);

/* ---------------- 2.4G（D11：厂商用例薄封装） ---------------- */
int      sdk_rf24_init(void);                 /* → 厂商 RF 初始化（含配对主循环） */
int      sdk_rf24_deinit(void);
int      sdk_rf24_send(const uint8_t *d, uint8_t len);
bool     sdk_rf24_is_connected(void);
int      sdk_rf24_pair_start(void);
int      sdk_rf24_pair_stop(void);            /* 停配对窗口的周期重发 */
int      sdk_rf24_unpair(void);
int      sdk_rf24_set_report_period_ms(uint16_t ms);
int      sdk_rf24_send_cfg(const uint8_t *d, uint8_t len); /* 配置中继（键盘 → Dongle） */
int      sdk_rf24_send_pair(const uint8_t *d, uint8_t len); /* 配对帧（Dongle 应答） */
int      sdk_rf24_send_host(uint8_t state);   /* 主机电源状态（Dongle → 键盘，BRIDGE_HOST_*） */
uint8_t  sdk_rf24_protocol_ver(void);
/* 建链取证快照（24B，仅诊断用）：[0]=up [1]=conn [2]=bound [3]=tx_busy [4]=tx_q_cnt
 * [5]=hb_miss [6..7]=report_period LE16，随后 8 个 LE16 计数依次为
 * tx_submit_ok/tx_submit_fail/tx_done_ok/tx_done_fail/tx_drop/rx_any/rx_pair/rx_data_bad。*/
int      sdk_rf24_diag(uint8_t *out, uint8_t *out_len);
/* 接收邮箱覆写（静默丢帧）累计次数（32 位，供 Dongle OTA GET_DIAG2 读取）。 */
uint32_t sdk_rf24_rx_overwrite(void);

/* ---------------- USB（仅 Dongle 侧） ---------------- */
int      sdk_usb_init(void);
int      sdk_usb_hid_send(const uint8_t *d, uint8_t len);
bool     sdk_usb_is_configured(void);
/* 运行时改 USB 标识：更新描述符并触发重新枚举（CMD_SET_NAME / CMD_SET_USB_ID） */
int      sdk_usb_set_product_name(const uint8_t *name, uint8_t len);
int      sdk_usb_set_ids(uint16_t vid, uint16_t pid);
/* 透传（VIA Raw HID）：Dongle → 上位机的 32B Raw HID 报文 */
int      sdk_usb_tunnel_send(const uint8_t *d, uint8_t len);
/* EP3 上行软件队列取证计数：入队总数 / 队满丢帧 / 队列占用高水位（任一指针可为 NULL）。
 * 高水位 ≥2 即证明曾有「上一包未被主机取走又来一帧」——旧实现会在此静默丢帧。 */
void     sdk_usb_ep3_stats(uint32_t *pushed, uint32_t *q_drop, uint8_t *hwm);

/* USB 远程唤醒（2.4G Dongle → 主机）：主机关闭 USB 前会先下发
 * SET_FEATURE(DEVICE_REMOTE_WAKEUP)；挂起期间收到键盘报文且已获许可时，
 * 由 sdk_usb_hid_send() 自动触发 resume 并暂存该报文，待主机重新枚举完成后
 * 由 sdk_usb_poll()（主循环每轮）补发，避免唤醒那一下的按键丢失。 */
bool     sdk_usb_is_suspended(void);
bool     sdk_usb_remote_wakeup_enabled(void);
int      sdk_usb_remote_wakeup(void);
void     sdk_usb_poll(void);

/* ---------------- 透传通道（借鉴市售模组 0x88/0x89） ---------------- */
/* 键盘侧：经 2.4G 送到 Dongle（USB 隧道）/ 经 BLE GATT 送到手机 APP（BLE 隧道） */
int      sdk_rf24_send_tunnel(const uint8_t *d, uint8_t len);
int      sdk_ble_tunnel_send(const uint8_t *d, uint8_t len);

/* ---------------- TMOS（仅供 BLE 协议栈运行） ---------------- */
void     sdk_tmos_process(void);              /* TMOS_SystemProcess() */

/* ---------------- 厂商例程需回调本工程的入口（见清单 2.2） ---------------- */
/* 以下函数由本工程实现（bridge_ch58x_sdk.c）、由**厂商例程文件**调用：
 *   - `sdk_ble_on_led_report()`  ← bridge_ble_hidsvc.c 的 Output Report 回调
 *   - `sdk_ble_on_raw_report()`  ← bridge_ble_hidsvc.c 的 VIA vendor Output Report(RID 0)
 *   - `sdk_ble_on_connect()`     ← bridge_ble_hidsvc.c 的 GAP 状态回调
 *   - `sdk_ble_on_disconnect()`  ← 同上
 *   - `sdk_rf24_on_recv()`       ← 厂商 2.4G 用例的收包回调（**必须在任务上下文调用**）
 *   - `sdk_rf24_on_link()`       ← 厂商 2.4G 用例的链路状态回调
 *   - `sdk_usb_on_led_report()`  ← Dongle 侧 USB HID Output Report（LED）回调
 * 它们内部统一转成 bridge_ch58x_radio_evt() / bridge_ch58x_radio_hid_in()。
 */
void     sdk_ble_on_led_report(uint8_t led_state);
/* 主机经 BLE HID vendor 报表（RID 0 隐式，QMK Raw HID）下发的 VIA 报文：
 * 走 BLE 隧道上报 SK32（bridge_ch58x_radio_tunnel_in(BRIDGE_TUNNEL_BLE,...)）。
 * 实现位于 bridge_ch58x_callbacks.c（纯翻译层，可在主机端验证）。 */
void     sdk_ble_on_raw_report(const uint8_t *d, uint8_t len);
void     sdk_ble_on_connect(uint16_t conn_handle);
void     sdk_ble_on_disconnect(uint16_t conn_handle);
/* BLE HID Control Point 的 Suspend(0x00)/Exit Suspend(0x01)：PC 对模组的休眠控制。
 * suspended=true 表示主机挂起，键盘侧据此更新主机电源状态。 */
void     sdk_ble_on_host_power(bool suspended);
void     sdk_rf24_on_recv(const uint8_t *d, uint8_t len);
void     sdk_rf24_on_link(bool connected);
void     sdk_usb_on_led_report(uint8_t led_state);
/* Dongle 侧 USB 挂起/恢复边沿：经 2.4G 把主机电源状态下发键盘。 */
void     sdk_usb_on_host_power(bool suspended);

#endif /* BRIDGE_CH58X_SDK_H */
