/* 平台接口层（port）
 *
 * core/ 只依赖本头文件，不依赖任何 SDK。
 *   - 目标侧实现：port/ch58x/bridge_port_ch58x.c（WCH CH58x SDK + 厂商 2.4G 用例）
 *   - 主机侧实现：test_host/port_host.c（用于 PC 上验证协议与状态机）
 */
#ifndef BRIDGE_PORT_H
#define BRIDGE_PORT_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 串口（桥数据面 UART1） ---------------- */
void     port_uart_init(uint32_t baudrate);
/* 非阻塞写：拷入 TX 环，满则丢弃多余字节（配合 core 的背压策略） */
void     port_uart_write(const uint8_t *data, uint16_t len);
bool     port_uart_tx_busy(void);
/* 阻塞排空：把软件 TX 环灌入硬件 FIFO 并等待其发完（带上限）。
 * 仅用于"应答后立即复位/重启"的路径，确保已排入软件环的帧真正上线。 */
void     port_uart_tx_flush(void);
/* 由 ISR 调用（目标侧）；主机侧由测试直接调用 core 的 bridge_uart_isr_feed */
void     port_uart_irq_enable(bool en);

/* ---------------- 时间 ---------------- */
uint32_t port_millis(void);
void     port_delay_ms(uint32_t ms);

/* ---------------- 非易失存储 ---------------- */
int      port_nv_read(uint16_t id, void *buf, uint16_t len);
int      port_nv_write(uint16_t id, const void *buf, uint16_t len);

/* ---------------- 射频（转发到 bridge_ble / bridge_rf24） ---------------- */
int      port_radio_init(uint8_t mode);
int      port_radio_deinit(uint8_t mode);
int      port_radio_send(uint8_t mode, const uint8_t *payload, uint8_t len);
bool     port_radio_is_connected(uint8_t mode);
uint8_t  port_radio_protocol_ver(uint8_t mode);
int      port_radio_pair_start(uint8_t mode);
/* 结束配对窗口（BLE 关广播 / 2.4G 停周期重发），幂等 */
int      port_radio_pair_stop(uint8_t mode);
/* 待连：进入常规无线模式时恢复可连性——BLE = 可连接不可发现广播（已配对主机
 * bond 掉线后直接重连）；2.4G = RX 常开无需动作，返回 OK。 */
int      port_radio_adv_resume(uint8_t mode);
int      port_radio_connect_profile(uint8_t mode, uint8_t profile);
int      port_radio_unpair(uint8_t mode, uint8_t profile);
int      port_radio_set_report_period_ms(uint8_t mode, uint16_t ms);
/* 2.4G 建链取证快照（仅诊断，24B，布局见 port/ch58x/bridge_ch58x_sdk.h 的
 * sdk_rf24_diag）；非 2.4G 平台返回 UNSUPPORTED。 */
int      port_radio_diag(uint8_t *out, uint8_t *out_len);

/* BLE HID 服务注册取证（仅诊断，32B，布局见 port/ch58x/bridge_ble_hidsvc.c 的
 * port_hid_diag）：out[0]=注册返回值(0xFF=未调用)，随后 4 个 uint16(LE) 句柄
 * （服务/ReportMap/vendor IN/vendor OUT），out[9]=报表条数，out[10]=属性表项数，
 * out[11]=复位源，out[12]/out[13]=面包屑(prev/now)；
 * out[14..17]/out[18..21]/out[22..25] = 3 个 uint32(LE) 出站 notify 计数：
 * fail(HidDev_Report 失败) / retry_ok(延迟重发成功) / retry_drop(延迟重发仍失败)；
 * out[26]=最近断链 LL reason，out[27]=终止事件本地角色，
 * out[28..29]=已建立累计次数，out[30..31]=已终止累计次数（均 16 位 LE）。
 * 仅键盘侧有意义；Dongle 侧返回 BRIDGE_OK + 全 0/0xFF。 */
int      port_hid_diag(uint8_t *out, uint8_t *out_len);

/* BLE 多主机槽绑定状态（CMD_GET_BLE_SLOTS）：
 *   out[0..3] = Profile 1..4 是否已存有效绑定（1 = 已绑定 / 0 = 空）
 *   out[4]    = 最近活跃 Profile（1..4），0 = 无
 * 仅键盘侧支持；Dongle 无 BLE 主机绑定，返回 BRIDGE_ERR_UNSUPPORTED。 */
int      port_ble_slots(uint8_t out[5]);

/* ---------------- 透传通道（借鉴市售模组 0x88/0x89） ----------------
 * kind = BRIDGE_TUNNEL_USB / BRIDGE_TUNNEL_BLE。
 * 板级决定出口：键盘侧经射频送到 Dongle/手机；Dongle 侧经 USB 送给上位机。 */
int      port_tunnel_send(uint8_t kind, const uint8_t *payload, uint8_t len);

/* ---------------- 名称 / USB 标识（CMD_SET_NAME / CMD_SET_USB_ID） ----------------
 * target = BRIDGE_NAME_TARGET_*。本板无法生效的 target 返回 BRIDGE_ERR_UNSUPPORTED，
 * 由 core 决定是否经 2.4G 中继给 Dongle（键盘侧改 Dongle USB 名 / VID-PID）。 */
int      port_set_name(uint8_t target, const uint8_t *name, uint8_t len);
int      port_set_usb_ids(uint16_t vid, uint16_t pid);

/* 本板是否为 USB 接收器（Dongle）：决定配置命令本地生效还是中继 */
bool     port_is_dongle(void);

/* 配置中继（仅键盘侧有意义）：帧 = [CMD][payload...]，经 2.4G 送 Dongle */
int      port_radio_cfg_send(const uint8_t *frame, uint8_t len);

/* ---------------- 芯片信息 / SRRC 定频 ---------------- */
int      port_chip_get_mac(uint8_t mac[6]);
int      port_radio_test_mode(uint8_t mode, uint8_t channel, uint8_t power);

/* ---------------- 固件升级（键盘侧 UART OTA，WCH IAP 分区） ----------------
 * 由 core/bridge_ota.c 状态机驱动；Dongle 侧 port_fw_begin 返回 UNSUPPORTED。 */
int      port_fw_begin(uint32_t total_len);                       /* 校验长度、准备暂存区 */
int      port_fw_write(uint32_t offset, const uint8_t *d, uint16_t len); /* 顺序写（4 对齐） */
void     port_fw_finish(uint16_t crc);   /* 写升级标志（IAP）+ 复位，正常不复返 */
void     port_fw_abort(void);            /* 放弃（暂存区残留由下次 begin 覆盖） */

/* ---------------- 日志（独立 UART0，严禁走 UART1） ---------------- */
void     port_log_write(const char *s, uint16_t len);

/* ---------------- GPIO ---------------- */
void     port_gpio_init(void);
uint8_t  port_gpio_read_mode_sel(void);     /* 0..3 */
void     port_gpio_set_wake(bool active);
void     port_gpio_set_link(bool connected);

/* ---------------- 电源 / 检测 ---------------- */
uint16_t port_bat_read_mv(void);
bool     port_vbus_present(void);
bool     port_charger_active(void);

/* ---------------- 系统控制 ---------------- */
void     port_enter_bootloader(void);
void     port_reset(void);
void     port_on_leds(uint8_t led_state);

#endif /* BRIDGE_PORT_H */
