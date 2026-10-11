/* CH58x port 层：实现 core/bridge_port.h
 *
 * 本文件**不含任何 SDK 头文件**，只通过 bridge_ch58x_sdk.h 访问硬件，
 * 因此可在 PC 上用 test_host/sdk_stub.c 顶替后编译运行（见 test_host/port_test.c）。
 *
 * 引脚依据：wireless/ch582f_pinmap.md 第 3/4 章，宏定义在 board/<target>/bridge_board.h。
 * 时序依据：wireless/ch582m_bridge_fw_spec.md 4.1（BRIDGE_UART_BAUD=921600）、6.x。
 */
#include <string.h>

#include "bridge_port.h"
#include "bridge_ch58x.h"
#include "bridge_ch58x_sdk.h"
#include "bridge_config.h"
#include "bridge_ringbuf.h"
#include "bridge_log.h"
#include "bridge_types.h"
#include "bridge_mode.h"
#include "bridge_test.h"
#include "bridge_uart.h"
#include "bridge_app.h"
#include "bridge_ota.h"
#include "bridge_ota_layout.h"   /* BRIDGE_OTA_FLAG_UART_IAP（port_enter_bootloader） */
#include "bridge_ble_multihost.h" /* BLE 多主机槽状态（port_ble_slots） */

#define BRIDGE_MOD_PORT "port"

/* ============================ 串口数据面 ============================ */

static uint8_t           s_tx_buf[BRIDGE_TX_RING_SIZE];
static bridge_ringbuf_t  s_tx;
static bool              s_uart_ready;

/* 睡眠窗口结束后的强制清醒监听截止时刻（port_millis 时基）。
 * 见 BRIDGE_POST_SLEEP_LISTEN_MS：保证主机重发的完整帧有一次落在 UART
 * 已上电的窗口内，解除"首字节丢失 → 收不到完整帧 → 立刻再睡"的自锁。 */
static uint32_t          s_listen_until_ms;

void port_uart_init(uint32_t baudrate)
{
    bridge_ringbuf_init(&s_tx, s_tx_buf, (uint16_t)sizeof(s_tx_buf));
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_uart1_init(baudrate);
    sdk_uart1_rx_irq_enable(true);
    s_uart_ready = true;
    LOG_I(BRIDGE_MOD_PORT, "uart1 %.0lu 8N1 (rx=%c%u tx=%c%u)", (unsigned long)baudrate,
          BRIDGE_PIN_UART_RX_PORT, BRIDGE_PIN_UART_RX_NUM,
          BRIDGE_PIN_UART_TX_PORT, BRIDGE_PIN_UART_TX_NUM);
#else
    /* Dongle 与主机走 USB（EP3 Raw HID），不使用 UART1：
     * 不初始化，PA8/PA9 保持推挽输出低（见 port_gpio_init）。 */
    (void)baudrate;
    LOG_I(BRIDGE_MOD_PORT, "uart1 disabled (dongle uses usb path)");
#endif
}

void port_uart_write(const uint8_t *data, uint16_t len)
{
    if (data == 0 || len == 0u) {
        return;
    }
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 UART1 数据面：直接丢弃，避免软件 TX 环积压使 tx_busy 恒真 */
    return;
#else
    /* 非阻塞语义：拷入 TX 环，环满则丢弃多余字节（core 已在外层做 5ms 帧间隔背压） */
    uint16_t n = bridge_ringbuf_write(&s_tx, data, len);
    if (n < len) {
        LOG_W(BRIDGE_MOD_PORT, "tx ring full, drop %u/%u", (unsigned)(len - n), len);
    }
#endif
}

bool port_uart_tx_busy(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    return false;   /* Dongle 无 UART1 数据面 */
#else
    /* 软件环有残留 或 硬件 TX FIFO 未排空 → 线路仍被占用 */
    return (bridge_ringbuf_count(&s_tx) > 0u) || (sdk_uart1_tx_bytes() > 0u);
#endif
}

void port_uart_irq_enable(bool en)
{
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_uart1_rx_irq_enable(en);
#else
    (void)en;   /* Dongle 未使用 UART1 */
#endif
}

/* 主循环调用：把软件 TX 环灌入硬件 FIFO（非阻塞，FIFO 满即返回） */
static void port_uart_tx_poll(void)
{
    while (bridge_ringbuf_count(&s_tx) > 0u) {
        uint8_t chunk[8];
        uint8_t n = 0u;
        uint8_t space;

        if (!s_uart_ready) {
            return;
        }
        space = (uint8_t)(8u - sdk_uart1_tx_bytes());
        if (space == 0u) {
            return;     /* 硬件 FIFO 满：下一轮主循环再来 */
        }
        while (n < space) {
            uint8_t b;
            if (!bridge_ringbuf_pop(&s_tx, &b)) {
                break;
            }
            chunk[n] = b;
            n++;
        }
        sdk_uart1_write(chunk, n);
    }
}

void port_uart_tx_flush(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 无 UART1 数据面：无需排空。 */
#else
    /* 把软件环灌入硬件 FIFO 并等其发完。@921600 单字节 ≈10.9µs，环内最多约 64B
     * → 约 0.7ms；取 200 次 1ms 轮询上限兜底，避免异常时把主循环卡死。 */
    for (uint8_t i = 0u; i < 200u; i++) {
        port_uart_tx_poll();
        if (!port_uart_tx_busy()) {
            return;
        }
        sdk_delay_ms(1u);
    }
#endif
}

void bridge_ch58x_uart_isr(void)
{
    uint8_t guard = 64u;
    uint8_t lsr;
    uint8_t rx_err = 0u;

    /* 读 IIR 清中断标志（RECV_RDY / RECV_TOUT / LINE_STAT 共用本处理） */
    (void)sdk_uart1_iir();

    /* ★ 必须读 LSR 清「线路状态」错误位（溢出/帧错；接收侧使能了 LINE_STAT）。
     * 不清会在持续收包触发溢出后形成中断风暴：ISR 反复进入、主循环被饿死
     * （真机 OTA 实测：连续传输数十 KB 后模组完全静默，PC 停在读 RFC 的循环里）。
     * 同时把错误位分类计数：接收 FIFO 溢出（OVER_ERR）会静默丢字节，
     * 是「一条腿全干净、只有并发射频负载下才丢」的根因候选，必须可观测。 */
    lsr = sdk_uart1_lsr();
    if (lsr & SDK_UART1_LSR_OVER_ERR) {
        rx_err |= BRIDGE_RX_ERR_OVERRUN;
    }
    if (lsr & SDK_UART1_LSR_FRAME_ERR) {
        rx_err |= BRIDGE_RX_ERR_FRAME;
    }
    if (lsr & SDK_UART1_LSR_PAR_ERR) {
        rx_err |= BRIDGE_RX_ERR_PARITY;
    }
    if (rx_err != 0u) {
        bridge_uart_note_rx_error(rx_err);
    }

    /* 读空 RX FIFO；@921600 单字节 ≈10.9µs，8 级 FIFO 满约需 87µs。
     * 加 64 次上限：即使 RFC 出现异常也不把主循环困在 ISR（剩余字节由
     * RECV_RDY 再次触发处理）。 */
    while (guard-- > 0u && sdk_uart1_rx_bytes() > 0u) {
        bridge_uart_isr_feed(sdk_uart1_rx_read());
    }
}

/* ============================ 时间 ============================ */

uint32_t port_millis(void)
{
    return sdk_millis();
}

void port_delay_ms(uint32_t ms)
{
    sdk_delay_ms(ms);
}

/* ============================ 非易失存储 ============================ */

/* NV 布局（DataFlash 32KB，0x70000~0x77FFF）：
 *   0x70000 + 0x0000 ~ 0x6FFF  留给 BLE 协议栈（除下方 SNV 外的其它用途）
 *   0x70000 + 0x7000 ~ 0x7FFF  本工程配置区（4KB，64 槽 × 64B）
 * 其中槽 48..63（绝对地址 0x77C00~0x77FFF，1024B）是 SDK GAPBondMgr 的 SNV 区
 * （HAL/config.h：BLE_SNV_ADDR/BLE_SNV_BLOCK/BLE_SNV_NUM=4），应用布局已将其预留，
 * 见 core/bridge_config.h「应用 NV 全槽分配」；应用侧禁止占用。
 * OTA 标志页（bridge_ota_layout.h：BRIDGE_OTA_FLAG_REL=0x7200）落在
 * 槽 8..11（绝对 0x77200~0x772FF），位于预留区 5..24，与上述三者互不重叠。
 */
#define NV_REGION_OFF   0x7000u
#define NV_REGION_SIZE  0x1000u
#define NV_SLOT_SIZE    64u
#define NV_SLOT_MAX     (NV_REGION_SIZE / NV_SLOT_SIZE)   /* 64 */

static uint32_t nv_slot_addr(uint16_t id)
{
    return (uint32_t)SDK_DATAFLASH_BASE + NV_REGION_OFF + ((uint32_t)id * NV_SLOT_SIZE);
}

static bool nv_slot_valid(uint16_t id, uint16_t len)
{
    return (id >= 1u) && (id < NV_SLOT_MAX) && (len <= NV_SLOT_SIZE);
}

int port_nv_read(uint16_t id, void *buf, uint16_t len)
{
    if (buf == 0 || !nv_slot_valid(id, len)) {
        return BRIDGE_ERR_PARAM;
    }
    return sdk_flash_read(nv_slot_addr(id), buf, len);
}

int port_nv_write(uint16_t id, const void *buf, uint16_t len)
{
    uint8_t  page[SDK_FLASH_ERASE_UNIT];
    uint32_t addr;
    uint32_t blk;
    uint16_t in_blk;

    if (buf == 0 || !nv_slot_valid(id, len)) {
        return BRIDGE_ERR_PARAM;
    }

    addr = nv_slot_addr(id);
    blk  = addr & ~(uint32_t)(SDK_FLASH_ERASE_UNIT - 1u);
    in_blk = (uint16_t)(addr - blk);

    /* 槽大小 64B 整除擦除粒度 256B，故一个槽不会跨块 */
    if ((uint32_t)in_blk + len > SDK_FLASH_ERASE_UNIT) {
        return BRIDGE_ERR_PARAM;
    }

    /* 读-改-写：同一 256B 块内可能还有其它槽，不能整块丢弃 */
    if (sdk_flash_read(blk, page, SDK_FLASH_ERASE_UNIT) != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }
    memcpy(&page[in_blk], buf, len);
    if (sdk_flash_erase(blk, SDK_FLASH_ERASE_UNIT) != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }
    if (sdk_flash_write(blk, page, SDK_FLASH_ERASE_UNIT) != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }
    return BRIDGE_OK;
}

/* ============================ 日志 ============================ */

void port_log_write(const char *s, uint16_t len)
{
    static bool s_log_ready;

    if (s == 0 || len == 0u) {
        return;
    }
    /* 惰性初始化：core 的 bridge_log_init(baud) 不做硬件动作（见 bridge_log.c），
     * UART0 由 port 层在首次真正要输出时初始化，避免 release 构建白占 UART0/PB4/PB7 */
    if (!s_log_ready) {
        sdk_uart0_init(BRIDGE_LOG_BAUD);
        s_log_ready = true;
    }
    sdk_uart0_write((const uint8_t *)s, len);
}

/* ============================ GPIO ============================ */

/* 未使用引脚清单：一律配置为推挽输出低，避免悬空输入漏电、受干扰。
 * 约束（见 ch582f_pinmap.md §6 与硬件铁律）：
 *   - PB14/PB15 为两线仿真（SDI 的 TIO/TCK），严禁改动；
 *   - PB23 为 RST#（外部复位输入），非 GPIO，不得驱动；
 *   - 已分配引脚（UART/USB/ADC/控制信号/ISP/调试串口）不在此列。 */
typedef struct {
    uint8_t port;   /* 'A' / 'B' */
    uint8_t pin;
} bridge_unused_pin_t;

static const bridge_unused_pin_t s_unused_pins[] = {
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：UART1/无电池/无 MODE_SEL/LINK 等全部闲置 */
    { 'A', 8 }, { 'A', 9 }, { 'A', 10 }, { 'A', 11 },
    { 'A', 12 }, { 'A', 13 }, { 'A', 14 }, { 'A', 15 },
    { 'B', 12 }, { 'B', 13 },
#else
    /* 键盘侧：PA10/PA11（未用 32.768kHz 晶振时释放为 GPIO）、PA12、
     *         PB10/PB11（键盘侧不走 USB，USB1 差分对闲置） */
    { 'A', 10 }, { 'A', 11 }, { 'A', 12 },
    { 'B', 10 }, { 'B', 11 },
#endif
};

void port_gpio_init(void)
{
#if BRIDGE_BOARD_HAS_MODE_SEL
    /* MODE_SEL0/1：板级拨码，内部下拉输入（仅键盘侧存在，由 SK32 驱动；
     * 悬空 = 00 = USB 默认模式） */
    sdk_gpio_in_pulldown(BRIDGE_PIN_MODE_SEL0_PORT, BRIDGE_PIN_MODE_SEL0_NUM);
    sdk_gpio_in_pulldown(BRIDGE_PIN_MODE_SEL1_PORT, BRIDGE_PIN_MODE_SEL1_NUM);
#endif

    /* ISP 配置脚：量产必须保持高（外部 10k 上拉 + 内部上拉兜底） */
    sdk_gpio_in_pullup(BRIDGE_PIN_ISP_CFG_PORT, BRIDGE_PIN_ISP_CFG_NUM);

#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle：LED_STAT 输出、PAIR_KEY 上拉输入 */
    sdk_gpio_out_init(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM, false);
    sdk_gpio_in_pullup(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM);
#else
    /* 键盘侧：WAKE/LINK 输出，CHRG 上拉输入，BAT_ADC 模拟输入 */
    sdk_gpio_in_pullup(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM);
    sdk_gpio_out_init(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM, false);
    sdk_gpio_out_init(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM, false);
    sdk_gpio_adc_in(BRIDGE_PIN_BAT_ADC_PORT, BRIDGE_PIN_BAT_ADC_NUM);
#endif

    /* 其余未使用引脚：推挽输出低 */
    for (uint32_t i = 0u;
         i < (uint32_t)(sizeof(s_unused_pins) / sizeof(s_unused_pins[0])); i++) {
        sdk_gpio_out_init(s_unused_pins[i].port, s_unused_pins[i].pin, false);
    }
}

uint8_t port_gpio_read_mode_sel(void)
{
#if BRIDGE_BOARD_HAS_MODE_SEL
    uint8_t v = 0u;

    if (sdk_gpio_read(BRIDGE_PIN_MODE_SEL0_PORT, BRIDGE_PIN_MODE_SEL0_NUM)) {
        v |= 0x01u;
    }
    if (sdk_gpio_read(BRIDGE_PIN_MODE_SEL1_PORT, BRIDGE_PIN_MODE_SEL1_NUM)) {
        v |= 0x02u;
    }
    return v;
#else
    /* Dongle 无 MODE_SEL：返回 0（core 视为 MODE_USB 取值 → Dongle 分支强制
     * 回 2.4G）。Dongle 的工作模式由 2.4G 配对结果决定，不由拨码决定。 */
    return 0u;
#endif
}

void port_gpio_set_wake(bool active)
{
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_gpio_write(BRIDGE_PIN_WAKE_PORT, BRIDGE_PIN_WAKE_NUM, active);
#else
    (void)active;
#endif
}

void port_gpio_set_link(bool connected)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 用 LED_STAT 指示链路 */
    sdk_gpio_write(BRIDGE_PIN_LED_STAT_PORT, BRIDGE_PIN_LED_STAT_NUM, connected);
#else
    sdk_gpio_write(BRIDGE_PIN_LINK_PORT, BRIDGE_PIN_LINK_NUM, connected);
#endif
}

/* ============================ 电源 / 检测 ============================ */

uint16_t port_bat_read_mv(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    return 0u;      /* Dongle 无电池 */
#else
    /* 分压还原：Vbat = Vadc * DEN / NUM（电阻取值见 pinmap 第 3 章，需实测标定） */
    uint32_t mv = (uint32_t)sdk_adc_read_mv(BRIDGE_ADC_CH_BAT);
    return (uint16_t)((mv * BRIDGE_BAT_DIV_DEN) / BRIDGE_BAT_DIV_NUM);
#endif
}

bool port_vbus_present(void)
{
    /* pinmap 第 5 章：VBUS 由 SK32 检测，桥不参与。
     * Dongle 恒由 USB 供电且 2.4G 是其正常工作模式，故同样不报告 VBUS，
     * 否则 core 的 VBUS 优先仲裁会永久拒绝无线模式。 */
    return false;
}

bool port_charger_active(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    return false;
#else
    /* CHRG_STAT 低有效（充电中 = 低） */
    return !sdk_gpio_read(BRIDGE_PIN_CHRG_PORT, BRIDGE_PIN_CHRG_NUM);
#endif
}

/* ============================ 芯片信息 ============================ */

int port_chip_get_mac(uint8_t mac[6])
{
    if (mac == 0) {
        return BRIDGE_ERR_PARAM;
    }
    sdk_ble_mac(mac);
    return BRIDGE_OK;
}

/* ============================ BLE 多主机槽状态 ============================ */

/* 4 组 BLE 主机绑定的槽位状态（数据源：bridge_ble_multihost 的 NV 槽索引表）。 */
int port_ble_slots(uint8_t out[5])
{
    if (out == 0) {
        return BRIDGE_ERR_PARAM;
    }
#if BRIDGE_BOARD_IS_DONGLE
    /* Dongle 恒为 2.4G 接收器，不承载 BLE 主机绑定。 */
    return BRIDGE_ERR_UNSUPPORTED;
#else
    uint8_t p;

    for (p = 1u; p <= BRIDGE_MH_PROFILE_MAX; p++) {
        out[p - 1u] = bridge_mh_has(p) ? 1u : 0u;
    }
    out[BRIDGE_MH_PROFILE_MAX] = bridge_mh_active();
    return BRIDGE_OK;
#endif
}

/* ============================ 固件升级（App 收包写 Stage） ============================
 * 键盘侧 App 经 UART 收 FW_*；Dongle 侧 App 经 USB HID 收 FW_*（方案A：全程留在
 * App，避免中途 USB 复位重枚举导致 Chrome 拿不回设备）。两侧都写 Stage，复位后由
 * 常驻 BOOT 统一搬运 Stage→APP_A。 */

int port_fw_begin(uint32_t total_len)
{
    return sdk_fw_begin(total_len);
}

int port_fw_write(uint32_t offset, const uint8_t *d, uint16_t len)
{
    return sdk_fw_write(offset, d, len);
}

void port_fw_finish(uint16_t crc)
{
    sdk_fw_finish(crc);
}

void port_fw_abort(void)
{
    sdk_fw_abort();
}

/* ============================ 系统控制 ============================ */

void port_enter_bootloader(void)
{
#if BRIDGE_BOARD_IS_DONGLE
    /* CH58x 的 BootLoader 在复位释放时采样 ISP 配置脚（PB22，本板 10k 上拉）：
     * 先尝试把该脚拉低再软复位。若所选 BootLoader 的采样窗口早于 GPIO 失效时刻，
     * 量产恢复必须依赖硬件方式（复位时 PB22 为低），见规格 10.4 / 清单 11.9。
     * TODO(核实)：确认该 SDK/BootLoader 版本是否需要写额外的进 ISP 标志。 */
    sdk_gpio_out_init(BRIDGE_PIN_ISP_CFG_PORT, BRIDGE_PIN_ISP_CFG_NUM, false);
    sdk_delay_ms(2);
    sdk_enter_bootloader();
#else
    /* 键盘侧：CMD_ENTER_BOOT 不再进 WCH 内置 ISP，而是写 UART_IAP 标志 + 复位，
     * 重启进**自家常驻 BOOT**（0x0，iap_main.c）驻留 UART 收包模式。
     * 语义与 Dongle 的 USB_IAP 对称（见 bridge_ota_layout.h / sdk_ota_flag_set_reset 注释）。
     * 收益：升级期间完全不跑 App 无线栈/睡眠，无 EVT_STATE 心跳噪音；
     *       BOOT 收满→写 IAP 搬运标志→复位走 BOOT 搬运；失败/取消→回原 App（AB 双区安全）。 */
    sdk_ota_flag_set_reset(BRIDGE_OTA_FLAG_UART_IAP);   /* 不复返 */
#endif
}

void port_reset(void)
{
    sdk_reset();
}

void port_on_leds(uint8_t led_state)
{
    /* 键盘侧：Caps/Num/Scroll 指示灯由 SK32 驱动（EVT_LEDS 已回传），桥侧无灯可点。
     * Dongle 侧：LED_STAT 由 port_gpio_set_link() 驱动，这里不需要额外动作。 */
    (void)led_state;
}

/* ============================ 配对按键（仅 Dongle） ============================ */

#define BRIDGE_PAIR_KEY_DEBOUNCE_MS  20u

#if BRIDGE_BOARD_HAS_PAIR_KEY
static void pair_key_poll(void)
{
    static bool     s_pressed;
    static uint32_t s_change_ms;

    bool     raw = !sdk_gpio_read(BRIDGE_PIN_PAIR_KEY_PORT, BRIDGE_PIN_PAIR_KEY_NUM); /* 低有效 */
    uint32_t now = sdk_millis();

    if (raw == s_pressed) {
        return;
    }
    if ((uint32_t)(now - s_change_ms) < BRIDGE_PAIR_KEY_DEBOUNCE_MS) {
        return;     /* 抖动 */
    }
    s_change_ms = now;
    s_pressed   = raw;

    if (raw) {
        /* 按下：Dongle 侧进入 2.4G 配对窗口，等待键盘接入 */
        int rc = bridge_mode_request((uint8_t)MODE_2G4_PAIR, true);
        LOG_I(BRIDGE_MOD_PORT, "pair key -> 2G4_PAIR rc=%d", rc);
    }
}
#endif

/* ================== 复位前阶段面包屑（看门狗复位排障，临时） ==================
 * 目的：定位"BLE 连接后自重启"的**具体停摆阶段**。复位源已坐实为看门狗超时
 * （R8_RESET_STATUS 低 3 位 = 0x02），但不知主循环停在哪一步 >400ms 未喂狗。
 * 做法：主循环/睡眠各关键点写入 g_bridge_dbg_stage；该变量放在链接脚本的
 * .dbg_retain 段（.bss 之后、栈 0x20007E00 之前），startup_CH583.S 只清
 * _sbss.._ebss → 看门狗复位后其值**留存**。为避免开机后主循环把它覆盖掉，
 * bridge_ch58x_init() 最早处先把它捕获进 s_bridge_dbg_stage_prev（普通 .bss）。
 * 读取口径：CMD_HID_DIAG 应答 out[12]=prev（复位前最后阶段）、out[13]=now（当前）。
 * 冷启动时 g_bridge_dbg_stage 为随机值，仅"看门狗复位后"读数有意义。 */
volatile uint8_t g_bridge_dbg_stage __attribute__((section(".dbg_retain")));
static uint8_t   s_bridge_dbg_stage_prev;

uint8_t bridge_dbg_stage_prev(void) { return s_bridge_dbg_stage_prev; }
uint8_t bridge_dbg_stage_now(void)  { return g_bridge_dbg_stage; }

/* 阶段标记（见 CMD_HID_DIAG 排障文档）：0x1x = 主循环，0x2x = 深睡，0x3x = 浅睡 */
#define BRIDGE_DBG(st)   (g_bridge_dbg_stage = (uint8_t)(st))

/* 供其它 port 文件（GAP 回调 / sdk_ble_on_connect 等）打点用：
 * 连接事件由协议栈在 sdk_tmos_process() 内部派发，其回调链会跨多个 .c 文件，
 * 单靠主循环的 0x1x 无法区分「卡在我方回调」还是「卡在闭源协议栈」。
 * 约定：0x4x = GAP 状态回调，0x5x = sdk_ble_on_connect，0x6x = HID 任务事件。 */
void bridge_dbg_mark(uint8_t st) { g_bridge_dbg_stage = st; }

/* ============================ 主循环 ============================ */

void bridge_ch58x_init(void)
{
    s_bridge_dbg_stage_prev = g_bridge_dbg_stage;   /* 捕获复位前最后阶段（须在覆盖之前） */
    g_bridge_dbg_stage      = 0x01u;                /* 本次启动：初始化中 */
    s_listen_until_ms = 0u;     /* 冷启动即无待监听窗口；亦使主机构建的状态基线确定 */
    /* 射频/协议栈基础初始化：CH58x 的 RF/BB 初始化是 BLE 与厂商 2.4G 用例
     * 共同的前提，且 USB 必须在协议栈之后初始化，故此处无条件先做（幂等）。 */
    if (sdk_ble_stack_init() != BRIDGE_OK) {
        LOG_E(BRIDGE_MOD_PORT, "rf/ble stack init failed");
    }
    /* sdk_ble_stack_init() 内部的 HidEmu_Init() 会顺带开启 BLE 广播；本工程中广播
     * 只应由 BLE 模式（bridge_mode → port_radio_connect_profile/pair_start）按需开启。
     * 此处立即撤下：否则非 BLE 模式（尤其恒为 2.4G 的 Dongle）下 BLE 广播会与厂商
     * 2.4G 争用同一射频，导致 2.4G 收发（含配对）失败。 */
    (void)sdk_ble_deinit();

#if BRIDGE_BOARD_HAS_USB
    if (sdk_usb_init() != BRIDGE_OK) {
        LOG_E(BRIDGE_MOD_PORT, "usb init failed");
    }
#endif

    bridge_app_init();

    /* 看门狗：core/射频/串口全部就绪后启用（避免初始化阶段的正常长耗时被误复位），
     * 之后由 bridge_ch58x_loop() 每轮喂狗。 */
    sdk_wdog_init(BRIDGE_WDOG_TIMEOUT_MS);
}

void bridge_ch58x_loop(void)
{
    BRIDGE_DBG(0x10);
    sdk_wdog_feed();        /* 看门狗：主循环每轮喂狗（400ms 窗口） */
    BRIDGE_DBG(0x11);       /* 即将进入协议栈事件处理（潜在长阻塞点） */
    sdk_tmos_process();     /* BLE 协议栈事件（含连接/配对回调） */
    sdk_ble_notify_poll();  /* 上一轮 notify 失败帧的延迟重发（TMOS 已释放缓冲） */
    BRIDGE_DBG(0x12);       /* 协议栈事件处理已返回 */
    port_uart_tx_poll();    /* 串口 TX 排空 */
    BRIDGE_DBG(0x13);

#if BRIDGE_BOARD_HAS_PAIR_KEY
    pair_key_poll();
    BRIDGE_DBG(0x14);
#endif

    /* 低延迟：不完全依赖 TMOS 事件驱动（921600 下单帧占线仅 174µs，需及时排空 FIFO） */
    BRIDGE_DBG(0x15);       /* 即将进入 app poll（含唤醒→模式跃迁） */
    bridge_app_poll();
    BRIDGE_DBG(0x16);       /* app poll 已返回 */
}

bool bridge_ch58x_can_sleep(void)
{
    /* 睡眠窗口后的强制清醒监听期：此时不许再进睡，等主机帧落进上电窗口 */
    if ((int32_t)(port_millis() - s_listen_until_ms) < 0) {
        return false;
    }
    if (bridge_test_is_active()) {
        return false;       /* SRRC 定频发射期间禁止休眠 */
    }
    /* 睡眠策略：MANUAL 下未经 CMD_SLEEP 指令许可则一律不进睡 */
    if (!bridge_mode_sleep_permitted()) {
        return false;
    }
    /* 仅在 core 已判定进入 MODE_SLEEP（射频已关闭、无需维持连接事件）时休眠，
     * 避免在维持 BLE 连接时因休眠窗口超过连接间隔而掉线。 */
    if (bridge_mode_get() != MODE_SLEEP) {
        return false;
    }
    if (port_uart_tx_busy()) {
        return false;       /* 有帧待发（软件 TX 环残留或硬件 FIFO 未排空） */
    }
    /* 方案 A（深睡镜像）：睡眠窗口内被 RXD1 唤醒后，若立即再深睡，后续帧
     * 字节全部随 UART 时钟停摆而丢失；须与 shallow_ok 同判据——接收活动后
     * 在 BRIDGE_RX_HOLD_AWAKE_MS 内禁止深睡，使主机连发/周期性 PING 中紧跟
     * 的完整帧得以收下并触发"休眠中被唤醒"逻辑。 */
    if (bridge_uart_rx_gap_ms() < BRIDGE_RX_HOLD_AWAKE_MS) {
        return false;
    }
    return true;
}

/* 统一进睡出口：RXD1 下降沿唤醒源只在睡眠窗口内开启（清醒期关闭，
 * 否则每个字节的起始位都会触发一次 GPIO 中断）。 */
static void port_sleep_enter(uint32_t ms)
{
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_uart1_rx_wake_enable(true);
#endif
    sdk_sleep(ms);
#if !BRIDGE_BOARD_IS_DONGLE
    sdk_uart1_rx_wake_enable(false);
    /* 方案 A：睡眠窗口内 RXD1 被唤醒 = 有下行数据到达，但帧首字节已随唤醒
     * 延迟丢失；此处续期"接收活动保持清醒"窗口，使紧跟其后的连续帧得以
     * 完整收下（break-in 后保持清醒）。 */
    if (sdk_uart1_rx_woke()) {
        bridge_uart_note_rx_activity();
    }
#endif
    /* 无论是否检测到唤醒，都强制一段清醒监听窗口：睡眠期间进入的帧首字节
     * 已随唤醒延迟丢失，靠这段窗口让主机的下一次完整重发落到 UART 上电期。 */
    s_listen_until_ms = port_millis() + BRIDGE_POST_SLEEP_LISTEN_MS;
}

void bridge_ch58x_sleep(void)
{
    BRIDGE_DBG(0x20);           /* 深睡入口 */
    sdk_wdog_suspend();         /* 睡眠 1s > 看门狗周期（约 557ms 上限）：先停复位使能 */
    BRIDGE_DBG(0x21);
    port_sleep_enter(1000u);    /* 至少 1s 后唤醒，与 1s 心跳周期对齐 */
    BRIDGE_DBG(0x22);           /* 深睡唤醒返回 */
    sdk_wdog_resume();          /* 唤醒后重挂并喂狗 */
    BRIDGE_DBG(0x23);
}

/* 浅睡（IDLE，保持链路）：主循环空闲时的 20ms 有界窗口。
 * 串口数据靠 RXD1 唤醒；注意 HAL_SleepInit() 只使能 RTC 唤醒，射频中断
 * 并不能唤醒，故 BLE 模式不能浅睡（见下方守卫）。 */
bool bridge_ch58x_shallow_ok(void)
{
    /* 睡眠窗口后的强制清醒监听期：此时不许再进睡，等主机帧落进上电窗口 */
    if ((int32_t)(port_millis() - s_listen_until_ms) < 0) {
        return false;
    }
    if (bridge_test_is_active()) {
        return false;           /* SRRC 定频发射期间禁止休眠 */
    }
    if (bridge_ota_active()) {
        return false;           /* OTA 传输期间保持清醒（Flash 写 + 高频收帧） */
    }
    if (port_uart_tx_busy()) {
        return false;           /* 有帧待发（软件 TX 环残留或硬件 FIFO 未排空） */
    }
#if BRIDGE_BOARD_IS_DONGLE
    return false;               /* Dongle 常供电接收，禁睡 */
#else
    /* 方案 A：近期有接收活动（唤醒事件或收到合法帧）则保持清醒，避免在浅睡
     * 期间吞掉下行帧首字节。注意判据用 bridge_uart_rx_gap_ms()（仅由 UART
     * 接收刷新），不可用 bridge_mode_idle_ms()——BLE 连接事件会持续刷新后者，
     * 会导致浅睡被完全抑制。 */
    if (bridge_uart_rx_gap_ms() < BRIDGE_RX_HOLD_AWAKE_MS) {
        return false;
    }
    /* BLE 模式禁止浅睡：BLE 连接/广播事件由协议栈按固定间隔（本工程请求
     * 10ms）精确调度，靠 RTC 定时唤醒维持。本工程浅睡是"盲睡固定
     * BRIDGE_SHALLOW_SLEEP_MS"，会调用 RTC_SetTignTime() 覆写协议栈为下一射频
     * 事件排定的 RTC 触发时刻；且 HAL_SleepInit() 只使能 RTC 唤醒（无射频中断
     * 唤醒），睡眠期间无法被连接/广播事件唤醒 → 睡过 TX/RX 窗口，基带留下未
     * 完成的收发事务，下一轮 LL_ProcessEvent → ll_tx_wait_finish 永久忙等 →
     * 主循环停摆（面包屑卡 0x11）→ 看门狗 WTR 复位。低功耗交由深睡
     * （MODE_SLEEP）承担。 */
    if (bridge_mode_get() == MODE_BLE || bridge_mode_get() == MODE_BLE_PAIR) {
        return false;
    }
    /* 睡眠策略与时间门限：MANUAL 下未经 CMD_SLEEP(IDLE/DEEP) 指令许可则不自动浅睡，
     * 使模块保持清醒（亦便于 SDI 调试口常连，避免误判"砖"）；AUTO 下还需距最近一次
     * 串口接收活动达 cfg->sleep_idle_s 秒（见 bridge_mode_shallow_permitted）。
     * 注：上方 BRIDGE_RX_HOLD_AWAKE_MS 硬下限始终生效，故有效门限取其与 idle_s 之大者。 */
    return bridge_mode_shallow_permitted();
#endif
}

void bridge_ch58x_idle(void)
{
    if (bridge_ch58x_can_sleep()) {
        BRIDGE_DBG(0x1F);
        bridge_ch58x_sleep();                          /* 深睡：MODE_SLEEP，1s 窗口 */
    } else if (bridge_ch58x_shallow_ok()) {
        BRIDGE_DBG(0x30);
        port_sleep_enter(BRIDGE_SHALLOW_SLEEP_MS);     /* 浅睡：保持链路 */
        BRIDGE_DBG(0x31);
    }
}
