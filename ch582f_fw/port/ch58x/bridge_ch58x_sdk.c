/* CH58x SDK 原语实现 —— 全工程**唯一**包含 WCH SDK 头文件的文件
 *
 * API 基线（已逐项核对）：WCH CH583 EVT —— 仓库 openwch/ch583，
 * git submodule 固定在 wireless/ch582f_fw/sdk/ch583 @ bd508ad7。
 * 核对依据：
 *   EVT/EXAM/SRC/StdPeriphDriver/{inc/ 与 *.c}   外设驱动
 *   EVT/EXAM/SRC/RVMSIS/core_riscv.h             PFIC_EnableIRQ / __INTERRUPT
 *   EVT/EXAM/BLE/LIB/CH58xBLE_LIB.h              BLE 协议栈（预编译 LIBCH58xBLE.a）
 *   EVT/EXAM/BLE/HAL/{MCU,SLEEP,RTC}.c           CH58X_BLEInit / CH58X_LowPower
 *   EVT/EXAM/BLE/HID_Keyboard/{APP,Profile}      HOGP 例程基座
 *
 * 本文件只做 1:1 机械映射，不含业务逻辑；业务逻辑在
 *   bridge_port_ch58x.c / bridge_radio_ch58x.c（可在 PC 上真编译真运行）。
 */

/* ---------------- SDK 头文件 ----------------
 * 只需 CONFIG.h + HAL.h + HOGP 例程头：
 *   CONFIG.h → CH58xBLE_LIB.h（不定义 CH58xBLE_ROM 即走预编译库）
 *            → CH58x_common.h（内含 gpio/uart/adc/flash/sys/pwr/clk/ISP583 全部外设头）
 * 注意：**不要**定义 CH58xBLE_ROM —— 那是"烧在 0x10000 的 ROM 库"变体，
 *       本工程链接 EVT/EXAM/BLE/LIB/LIBCH58xBLE.a，对应 CH58xBLE_LIB.h。 */
#include "CONFIG.h"            /* BLE 配置 + 协议栈头 + 外设头 */
#include "HAL.h"               /* HAL_Init / CH58X_BLEInit / CH58X_LowPower / RTC 宏 */
#include "hiddev.h"            /* HidDev_Init / HidDev_Report / HID_REPORT_TYPE_INPUT */
#include "hidkbd.h"            /* HidEmu_Init */

#include "bridge_ch58x_sdk.h"   /* 本文件实现的接口 + DataFlash 布局宏 */
#include "bridge_ble_hidsvc.h"  /* 本工程替代 hidkbdservice.c 的 HID 服务 + RID */
#include "bridge_ble_multihost.h" /* 4 槽 BLE 绑定存档 / 还原（整段 SNV 区） */
#include "bridge_ota_layout.h"  /* 键盘侧 OTA 分区与标志位 */
#include "bridge_ch58x.h"
#include "bridge_config.h"
#include "bridge_types.h"
#include "bridge_mode.h"       /* BRIDGE_RADIO_EVT_*（厂商回调翻译用） */
#include "bridge_cfg.h"        /* bridge_cfg_get()->ble_profile（连接时同步多主机活跃槽） */

/* ============================ 常量 ============================ */

/* ADC 参考电压（mV）：CH58x ADC 以 VDD(3.3V) 为参考。
 * ADC_ExtSingleChSampInit 不改变参考源，PGA 增益为 0dB，故按 3300mV 满量程换算。 */
#define SDK_ADC_VREF_MV      3300u
#define SDK_ADC_FULL_SCALE   4096u      /* 12 bit */

#define SDK_TMOS_TICK_MS_NUM 5u         /* TMOS 时钟单位 = 0.625ms = 5/8 ms */
#define SDK_TMOS_TICK_MS_DEN 8u

#define SDK_BLE_CONN_INVALID 0xFFFFu

/* ---------------- BLE SNV 区 ---------------- */
/* 库在 DataFlash 内划一段区存放绑定/CCCD 信息：cfg.SNVAddr = BLE_SNV_ADDR，
 * 该宏是**相对 DataFlash 基址的偏移**（本工程 0x77C00-FLASH_ROM_MAX_SIZE → 0x7C00），
 * 区大小为 BLE_SNV_BLOCK × BLE_SNV_NUM（256×4 = 1024B）。
 * BLE_SNV_NUM=4 使闭源 GAPBondMgr **原生记忆 4 个主机绑定**，切槽后库按主机地址
 * 自动匹配，应用层无需整段 SNV 存档/还原。绝对地址 0x77C00 对应应用 NV 槽 48..63，
 * 应用侧禁止占用（见 core/bridge_config.h 的 BRIDGE_NV_SNV_LO）。 */

/* BLE 协议栈内存堆：CONFIG.h 中 extern 声明、CH58X_BLEInit() 里赋给 cfg.MEMAddr，
 * 必须由应用层定义（官方例程放在 hidkbd_main.c，本工程放在此边界文件，
 * 以免 board/main.c 引入 SDK 头）。 */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

static bool     s_ble_inited;
static uint16_t s_ble_conn = SDK_BLE_CONN_INVALID;
static bool     s_ble_teardown;              /* sdk_ble_deinit 主动断链中（见 sdk_ble_on_disconnect） */
static bool     s_adv_name_mac;             /* 广播名是否追加 MAC 后缀（D6） */
static uint8_t  s_ble_addr_prof;            /* 已应用的「按槽区分」BLE 地址所属槽，0=未应用 */
static uint8_t  s_ble_switch_pending;       /* 待执行的切槽（有连接时延后），0=无 */

/* RXD1(PA8) 唤醒源被触发过（读清）：仅睡眠窗口内使能该源，故仅代表
 * "Sleep 期间有下行数据到达"。供 port 层据此续期"接收活动保持清醒"窗口。 */
static volatile bool s_rx_wake_flag;

static int      sdk_ble_tunnel_svc_init(void);      /* 前置声明：由 sdk_ble_stack_init 调用 */

/* ============================ 系统 / 时间 ============================ */

void sdk_sys_init(void)
{
    /* BLE 协议栈对主频有要求（库内 RF 校准/SysTick 按 60MHz 计），不可随意改动。
     * 与官方例程 hidkbd_main.c 一致。 */
    SetSysClock(CLK_SOURCE_PLL_60MHz);
}

uint32_t sdk_sysclk_hz(void)
{
    return GetSysClock();
}

uint32_t sdk_millis(void)
{
    /* TMOS_GetSystemClock() 单位为 0.625ms。
     * 用 (t/8)*5 + (t%8)*5/8 换算，避免 t*5 在 t 较大时溢出 32 位
     * （数学上与原式 floor(t*5/8) 等价），32 位回绕周期约 31 天。 */
    uint32_t t = TMOS_GetSystemClock();
    return ((t / SDK_TMOS_TICK_MS_DEN) * SDK_TMOS_TICK_MS_NUM) +
           (((t % SDK_TMOS_TICK_MS_DEN) * SDK_TMOS_TICK_MS_NUM) / SDK_TMOS_TICK_MS_DEN);
}

void sdk_delay_ms(uint32_t ms)
{
    /* mDelaymS 形参为 uint16_t；本工程调用值均 <= 2ms，故截断安全。 */
    DelayMs((uint16_t)ms);
}

void sdk_reset(void)
{
    SYS_ResetExecute();
}

/* ---------------- 复位源取证（仅排障用，见 CMD_HID_DIAG 0x2B） ----------------
 * R8_RESET_STATUS（0x40001044，RO）低 3 位记录**本次启动之前**那一次复位的来源，
 * 读后不被清（维持到下次复位），故只需在启动最早阶段抓一次存入 RAM：
 *   0x00=RST_FLAG_SW   软件复位（SYS_ResetExecute）
 *   0x01=RST_FLAG_RPOR 上电复位
 *   0x02=RST_FLAG_WTR  看门狗复位（HardFault/挂死未喂狗）
 *   0x03=RST_FLAG_MR   外部复位引脚（MR）
 *   0x05=RST_FLAG_GPWSM 睡眠/关机期间上电复位
 * 取值宏见 CH583SFR.h 的 RST_FLAG_*；SYS_GetLastResetSta() 已 & RB_RESET_FLAG(0x07)。 */
static uint8_t s_last_reset_cause;

void sdk_capture_reset_cause(void)
{
    s_last_reset_cause = (uint8_t)SYS_GetLastResetSta();
}

uint8_t sdk_last_reset_cause(void)
{
    return s_last_reset_cause;
}

void sdk_enter_bootloader(void)
{
    /* 与官方 IAP 例程 jumpToIap()（EVT/EXAM/IAP/APP/src/app_flag.c）一致：
     * 先关总中断再复位，避免复位瞬间被中断打断。
     * 进入工厂 ISP 的硬件前提：复位释放时 PB22 为低（见 pinmap：PB22 为 ISP 配置脚，
     * 板上 10k 上拉）。软件侧无专用"进 ISP"接口，故此处只做复位。 */
    uint32_t irq;

    SYS_DisableAllIrq(&irq);
    SYS_ResetExecute();
}

void sdk_ble_mac(uint8_t mac[6])
{
    /* GetMACAddress(Buffer) = FLASH_EEPROM_CMD(CMD_GET_ROM_INFO, ROM_CFG_MAC_ADDR, ...)，
     * 取自芯片出厂 MAC（ISP583.h）。Buffer 必须 4 字节对齐，故先读到对齐临时区。 */
    __attribute__((aligned(4))) uint8_t tmp[8];

    (void)GetMACAddress(tmp);
    memcpy(mac, tmp, 6u);
}

/* ============================ UART1（桥数据面） ============================ */

void sdk_uart1_init(uint32_t baud)
{
    /* PA9 = TXD1（先置高避免起始位毛刺），PA8 = RXD1
     * 注意：PA8/PA9 是 UART1 的**默认**映射脚，无需设置 R16_PIN_ALTERNATE；
     *       切勿误置 RB_PIN_UART1（会切到 PB12/PB13，本板为 MODE_SEL）。 */
    GPIOA_SetBits(GPIO_Pin_9);
    GPIOA_ModeCfg(GPIO_Pin_8, GPIO_ModeIN_PU);
    GPIOA_ModeCfg(GPIO_Pin_9, GPIO_ModeOut_PP_5mA);

    UART1_DefInit();
    UART1_BaudRateCfg(baud);
    /* RX FIFO 触发点：1 字节。CH58x 无接收超时中断，若用 7 字节触发，则长度
     * 非 7 整数倍的孤立帧（最小请求帧恰 8 字节）末字节会滞留 FIFO 且永不触发
     * 中断，整帧无法组包。1 字节触发保证任何完整帧都能被及时排空组包。 */
    UART1_ByteTrigCfg(UART_1BYTE_TRIG);

    /* TX 采用查询/主动灌 FIFO 方式，不使能 THR 中断，避免与主循环争用 */
    UART1_INTCfg(DISABLE, RB_IER_THR_EMPTY);
    UART1_INTCfg(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);

    /* 必须显式使能 UART1 中断（EVT/EXAM/UART1/src/Main.c 同），
     * 否则 UART1_IRQHandler 永不触发、桥 RX 完全收不到数据。 */
    PFIC_EnableIRQ(UART1_IRQn);

    /* 低功耗唤醒：CH583 的 R8_SLP_WAKE_CTRL 没有 UART 唤醒位，故 RXD1(PA8)
     * 叠加为下降沿 GPIO 唤醒源；由端口层在每次进睡前开启、唤醒后关闭
     * （见 sdk_uart1_rx_wake_enable / bridge_port_ch58x.c）。 */
}

void sdk_uart1_rx_irq_enable(bool en)
{
    UART1_INTCfg(en ? ENABLE : DISABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
}

void sdk_uart1_write(const uint8_t *d, uint16_t len)
{
    /* UART1_SendString 会等待 TX FIFO 有空位后写入，写满 FIFO 即返回 */
    UART1_SendString((uint8_t *)d, len);
}

uint8_t sdk_uart1_tx_bytes(void)
{
    return R8_UART1_TFC;
}

uint8_t sdk_uart1_rx_bytes(void)
{
    return R8_UART1_RFC;
}

uint8_t sdk_uart1_rx_read(void)
{
    return R8_UART1_RBR;
}

uint8_t sdk_uart1_iir(void)
{
    return R8_UART1_IIR;
}

uint8_t sdk_uart1_lsr(void)
{
    return R8_UART1_LSR;
}

/* Sleep 期间 RXD1 下降沿（起始位）唤醒：引脚保持 UART1 RX 复用（默认映射），
 * 仅叠加 GPIO 边沿检测；唤醒后字节仍由 UART1 中断读空 FIFO。 */
void sdk_uart1_rx_wake_enable(bool en)
{
    if (en) {
        s_rx_wake_flag = false;   /* 新一个睡眠窗口：清上一次的唤醒痕迹 */
        GPIOA_ITModeCfg(GPIO_Pin_8, GPIO_ITMode_FallEdge);   /* 内部：清标志 + 使能引脚中断 */
        PFIC_EnableIRQ(GPIO_A_IRQn);
        PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_GPIO_WAKE, Long_Delay);
    } else {
        R16_PA_INT_EN &= ~(uint16_t)GPIO_Pin_8;
        PWR_PeriphWakeUpCfg(DISABLE, RB_SLP_GPIO_WAKE, Short_Delay);
    }
}

/* 读并清除「RXD1 唤醒过」标志：port 层在每个睡眠窗口结束后调用，
 * 为真说明睡眠期间有下行数据，应续期「接收活动保持清醒」窗口。
 * 除 ISR 置的标志外，再取 GPIO 外设硬件判据（R16_PA_INT_IF）兜底：
 * 若唤醒中断因故未被服务（ISR 尚未执行/被屏蔽），该位由外设硬件置位且
 * 不会丢，据此仍可确认「睡眠期间收到过 RXD1 起始位」。无需在此清标志，
 * 下一个睡眠窗口开头的 GPIOA_ITModeCfg() 会写 R16_PA_INT_IF 清除。 */
bool sdk_uart1_rx_woke(void)
{
    bool woke = s_rx_wake_flag;
    if ((R16_PA_INT_IF & (uint16_t)GPIO_Pin_8) != 0u) {
        woke = true;
    }
    s_rx_wake_flag = false;
    return woke;
}

/* ============================ UART0（printf 日志） ============================ */

void sdk_uart0_init(uint32_t baud)
{
    /* PB7 = TXD0（先置高），PB4 = RXD0（UART0 默认映射，R16_PIN_ALTERNATE 不置位）
     * 硬约束：日志只能走 UART0，严禁改用 UART1（桥数据面）。 */
    GPIOB_SetBits(GPIO_Pin_7);
    GPIOB_ModeCfg(GPIO_Pin_4, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_7, GPIO_ModeOut_PP_5mA);

    UART0_DefInit();
    UART0_BaudRateCfg(baud);
}

void sdk_uart0_write(const uint8_t *d, uint16_t len)
{
    UART0_SendString((uint8_t *)d, len);
}

/* ============================ GPIO ============================ */

static void sdk_gpio_mode(uint8_t port, uint32_t mask, GPIOModeTypeDef mode)
{
    if (port == 'B') {
        GPIOB_ModeCfg(mask, mode);
    } else {
        GPIOA_ModeCfg(mask, mode);
    }
}

static void sdk_gpio_write_raw(uint8_t port, uint32_t mask, bool high)
{
    if (high) {
        if (port == 'B') {
            GPIOB_SetBits(mask);
        } else {
            GPIOA_SetBits(mask);
        }
    } else {
        if (port == 'B') {
            GPIOB_ResetBits(mask);
        } else {
            GPIOA_ResetBits(mask);
        }
    }
}

void sdk_gpio_out_init(uint8_t port, uint8_t pin, bool high)
{
    uint32_t mask = (uint32_t)1u << pin;

    sdk_gpio_mode(port, mask, GPIO_ModeOut_PP_5mA);
    sdk_gpio_write_raw(port, mask, high);
}

void sdk_gpio_in_pullup(uint8_t port, uint8_t pin)
{
    sdk_gpio_mode(port, (uint32_t)1u << pin, GPIO_ModeIN_PU);
}

void sdk_gpio_in_pulldown(uint8_t port, uint8_t pin)
{
    sdk_gpio_mode(port, (uint32_t)1u << pin, GPIO_ModeIN_PD);
}

void sdk_gpio_adc_in(uint8_t port, uint8_t pin)
{
    /* 模拟输入：浮空输入（不带上/下拉，避免影响分压） */
    sdk_gpio_mode(port, (uint32_t)1u << pin, GPIO_ModeIN_Floating);
}

void sdk_gpio_write(uint8_t port, uint8_t pin, bool high)
{
    sdk_gpio_write_raw(port, (uint32_t)1u << pin, high);
}

bool sdk_gpio_read(uint8_t port, uint8_t pin)
{
    uint32_t mask = (uint32_t)1u << pin;
    uint32_t v    = (port == 'B') ? GPIOB_ReadPortPin(mask) : GPIOA_ReadPortPin(mask);
    return (v != 0u);
}

/* ============================ ADC ============================ */

uint16_t sdk_adc_read_mv(uint8_t ch)
{
    static bool    s_adc_ready;
    static int32_t s_adc_rough;
    int32_t        raw;

    /* 用法与官方 EVT/EXAM/ADC/src/Main.c 一致：
     *   ADC_ExtSingleChSampInit(SampleFreq_3_2, ADC_PGA_0)  外部单通道采样使能
     *   ADC_DataCalib_Rough()                               取内部偏置，换算时加回
     *   ADC_ChannelCfg(ch)                                  选通道（ch4 = AIN4 = PA14）
     *   ADC_ExcutSingleConver()                             单次转换
     * SampleFreq_3_2（3.2M 采样时钟）适配电池分压这类高阻源。 */
    if (!s_adc_ready) {
        ADC_ExtSingleChSampInit(SampleFreq_3_2, ADC_PGA_0);
        s_adc_rough = (int32_t)ADC_DataCalib_Rough();
        s_adc_ready = true;
    }

    ADC_ChannelCfg(ch);

    raw = (int32_t)ADC_ExcutSingleConver() + s_adc_rough;
    if (raw < 0) {
        raw = 0;
    }
    return (uint16_t)(((uint32_t)raw * SDK_ADC_VREF_MV) / SDK_ADC_FULL_SCALE);
}

/* ============================ Flash / DataFlash ============================ */

/* CH58x 地址空间：CodeFlash 0x00000-0x6FFFF 直接映射可读（FLASH_ROM_READ），
 * DataFlash 0x70000-0x77FFF 只能经 ROM 命令接口访问：
 *   EEPROM_READ/WRITE/ERASE = FLASH_EEPROM_CMD(CMD_EEPROM_*, ...)
 * 且 **地址为相对 DataFlash 基址的偏移**（官方 config.h 的 BLE_SNV_ADDR 即如此
 * 由 0x77E00 - FLASH_ROM_MAX_SIZE 折算出 0x7E00）。
 * 本工程对外统一用绝对地址（与 SDK_DATAFLASH_BASE 对齐、与主机 stub 的模型一致），
 * 故此处做一次基址折算。Buffer 必须在 RAM 且 4 字节对齐。 */

static bool sdk_flash_addr_ok(uint32_t addr, uint16_t len)
{
    uint32_t off = addr - SDK_DATAFLASH_BASE;

    return (addr >= SDK_DATAFLASH_BASE) &&
           ((uint32_t)len <= SDK_DATAFLASH_SIZE) &&
           (off <= (uint32_t)(SDK_DATAFLASH_SIZE - len));
}

int sdk_flash_read(uint32_t addr, void *buf, uint16_t len)
{
    if (buf == 0 || !sdk_flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_PARAM;
    }
    return (EEPROM_READ(addr - SDK_DATAFLASH_BASE, buf, len) == 0u)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

int sdk_flash_write(uint32_t addr, const void *buf, uint16_t len)
{
    uint32_t irq;
    uint32_t rc;

    if (buf == 0 || !sdk_flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_PARAM;
    }

    SYS_DisableAllIrq(&irq);
    rc = EEPROM_WRITE(addr - SDK_DATAFLASH_BASE, (void *)buf, len);
    SYS_RecoverIrq(irq);

    return (rc == 0u) ? BRIDGE_OK : BRIDGE_ERR_IO;
}

int sdk_flash_erase(uint32_t addr, uint16_t len)
{
    uint32_t irq;
    uint32_t rc;

    if (!sdk_flash_addr_ok(addr, len)) {
        return BRIDGE_ERR_PARAM;
    }
    if (((addr - SDK_DATAFLASH_BASE) % SDK_FLASH_ERASE_UNIT) != 0u ||
        (len % SDK_FLASH_ERASE_UNIT) != 0u) {
        return BRIDGE_ERR_PARAM;
    }

    SYS_DisableAllIrq(&irq);
    rc = EEPROM_ERASE(addr - SDK_DATAFLASH_BASE, len);
    SYS_RecoverIrq(irq);

    return (rc == 0u) ? BRIDGE_OK : BRIDGE_ERR_IO;
}

/* ============================ 固件升级（UART OTA / WCH IAP 分区） ============================
 * 暂存区 = Stage（0x38000，216K）。CodeFlash 擦除粒度 4KB：顺序写时按需擦除
 * 尚未擦过的 4K 页（FLASH_ROM_ERASE/WRITE，ROM 接口）。写期间关总中断。
 * 结束时把记录（flag=IAP / len / crc）写入 DataFlash 预留页（槽 8..11）再复位：
 * 0x0 常驻 BOOT → 按记录把 Stage 搬运到 APP_A(0x2000) → 跳新 App。 */
static bool     s_fw_active;
static uint32_t s_fw_total;
static uint32_t s_fw_erased;

/* FLASH_ROM_WRITE 要求缓冲在 RAM 且 4 字节对齐（ISP583.h 明确要求）。
 * 上游数据在栈上帧结构里（对齐=1，随调用深度漂移）→ 直接传会随机 HardFault
 * （真机 OTA 实测：传若干包后复位）。故统一经本对齐缓冲中转。 */
static __attribute__((aligned(4))) uint8_t s_fw_wbuf[64];

int sdk_fw_begin(uint32_t total_len)
{
    if (total_len == 0u || total_len > BRIDGE_OTA_STAGE_MAX) {
        return BRIDGE_ERR_PARAM;
    }
    s_fw_total  = total_len;
    s_fw_erased = 0u;
    s_fw_active = true;
    /* OTA 期间挂起看门狗：Flash 擦写（尤其 4K 页擦）会长时间关中断，
     * 400ms 窗口会误复位（真机实测）；升级全程受上位机监督，结束即复位。 */
    sdk_wdog_suspend();
    return BRIDGE_OK;
}

int sdk_fw_write(uint32_t offset, const uint8_t *d, uint16_t len)
{
    uint32_t irq;
    uint32_t end;

    if (!s_fw_active || d == 0 || len == 0u) {
        return BRIDGE_ERR_STATE;
    }
    end = offset + (uint32_t)len;
    if (end > s_fw_total || (offset & 3u) != 0u || (len & 3u) != 0u ||
        (uint16_t)len > (uint16_t)sizeof(s_fw_wbuf)) {
        return BRIDGE_ERR_PARAM;
    }
    memcpy(s_fw_wbuf, d, len);      /* 对齐中转（见 s_fw_wbuf 注释） */

    SYS_DisableAllIrq(&irq);
    while (s_fw_erased < ((end + 4095u) & ~4095u)) {          /* 按需擦 4K 页 */
        if (FLASH_ROM_ERASE(BRIDGE_OTA_STAGE_ADDR + s_fw_erased, 4096u) != 0u) {
            SYS_RecoverIrq(irq);
            return BRIDGE_ERR_IO;
        }
        s_fw_erased += 4096u;
    }
    if (FLASH_ROM_WRITE(BRIDGE_OTA_STAGE_ADDR + offset, s_fw_wbuf, len) != 0u) {
        SYS_RecoverIrq(irq);
        return BRIDGE_ERR_IO;
    }
    SYS_RecoverIrq(irq);
    return BRIDGE_OK;
}

void sdk_fw_finish(uint16_t crc)
{
    __attribute__((aligned(4))) uint8_t rec[BRIDGE_OTA_REC_LEN];
    uint32_t irq;

    rec[0] = (uint8_t)BRIDGE_OTA_FLAG_IAP;
    rec[1] = (uint8_t)(s_fw_total & 0xFFu);
    rec[2] = (uint8_t)((s_fw_total >> 8) & 0xFFu);
    rec[3] = (uint8_t)((s_fw_total >> 16) & 0xFFu);
    rec[4] = (uint8_t)((s_fw_total >> 24) & 0xFFu);
    rec[5] = (uint8_t)(crc & 0xFFu);
    rec[6] = (uint8_t)(crc >> 8);
    rec[7] = 0u;

    SYS_DisableAllIrq(&irq);
    (void)EEPROM_ERASE(BRIDGE_OTA_FLAG_REL, BRIDGE_OTA_FLAG_PAGE);   /* 槽 8..11 全为预留页 */
    (void)EEPROM_WRITE(BRIDGE_OTA_FLAG_REL, rec, BRIDGE_OTA_REC_LEN);
    DelayMs(10u);
    SYS_ResetExecute();
    /* 不返回 */
}

void sdk_fw_abort(void)
{
    s_fw_active = false;
    sdk_wdog_resume();      /* 与 sdk_fw_begin 的挂起配对 */
}

void sdk_ota_flag_set_reset(uint8_t flag)
{
    __attribute__((aligned(4))) uint8_t rec[BRIDGE_OTA_REC_LEN] = {0};
    uint32_t irq;

    rec[0] = flag;      /* len/crc 保持 0：本路径只作「阶段请求」，不带镜像记录 */
    SYS_DisableAllIrq(&irq);
    (void)EEPROM_ERASE(BRIDGE_OTA_FLAG_REL, BRIDGE_OTA_FLAG_PAGE);   /* 槽 8..11 全为预留页 */
    (void)EEPROM_WRITE(BRIDGE_OTA_FLAG_REL, rec, BRIDGE_OTA_REC_LEN);
    DelayMs(10u);
    SYS_ResetExecute();
    /* 不返回 */
}

/* ============================ 低功耗 ============================
 * 本工程**自带** CH58X_LowPower() / HAL_SleepInit() 实现（Makefile 已把 SDK 的
 * EVT/EXAM/BLE/HAL/SLEEP.c 排除出编译，两份同名符号不可共存）。
 *
 * 为什么必须接管：CH58X_BLEInit()（SDK HAL/MCU.c）在 HAL_SLEEP==TRUE 时会把
 * CH58X_LowPower 注册为协议栈配置里的 pfnIdleCB（cfg.sleepCB），而该回调的语义是
 * "协议栈空闲时自行进睡"——协议栈在 TMOS 无事可做时会**直接调用它**，不经过
 * bridge_ch58x_idle()，于是本工程的全部睡眠策略（sleep_permitted / MODE_SLEEP /
 * rx_gap 窗口 / s_listen_until_ms）都被绕过。
 *
 * 真机症状（SLEEP=1 vs SLEEP=0 唯一差异）：清醒占空比仅 ~0.8%，应答呈 ~1s 周期
 * 簇状；同硬件同指令链路的 SLEEP=0 版 100% 应答。协议栈自行进睡会在主机帧首字节
 * 到达**之前**就把 UART 时钟停掉（LowPower_Sleep 停 CPU/外设时钟）：
 *   首字节丢失 → 收不到完整帧 → 主机看不出设备在睡，设备也无法被完整帧唤醒 →
 *   表现为"睡眠无法唤醒"、稀疏重发全部落空（只有恰好撞进清醒窗的帧才有应答）。
 *
 * 因此这里加一道**授权门**：只有本工程策略层（bridge_ch58x_idle → port_sleep_enter
 * → sdk_sleep）显式授权时才真正进睡；协议栈的空闲回调未授权，一律返回 2
 * （SDK 自身约定的"跳过本次睡眠"返回值），行为与 SLEEP=0 构建完全一致。
 * 低功耗时序与 SDK SLEEP.c 逐行一致（sdk submodule 固定 commit，未做任何改动）。 */

static volatile bool s_lowpower_allowed;    /* 仅 sdk_sleep() 在调用期间置位 */

void HAL_SleepInit(void)
{
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_RTC_WAKE;    /* RTC 唤醒 */
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL |= RB_RTC_TRIG_EN;     /* 触发模式 */
    sys_safe_access_disable();
    PFIC_EnableIRQ(RTC_IRQn);
#endif
}

uint32_t CH58X_LowPower(uint32_t time)
{
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    uint32_t      time_sleep, time_curr;
    unsigned long irq_status;

    /* 授权门：协议栈空闲回调（未授权）一律跳过，绝不自作主张进睡 */
    if (!s_lowpower_allowed) {
        return 2;
    }

    SYS_DisableAllIrq(&irq_status);
    time_curr = RTC_GetCycle32k();
    /* 限制睡眠时间 */
    if (time < time_curr) {
        time_sleep = time + (RTC_TIMER_MAX_VALUE - time_curr);
    } else {
        time_sleep = time - time_curr;
    }
    /* 若睡眠时间小于最小睡眠时间或大于最大睡眠时间，则不睡眠 */
    if ((time_sleep < SLEEP_RTC_MIN_TIME) || (time_sleep > SLEEP_RTC_MAX_TIME)) {
        SYS_RecoverIrq(irq_status);
        return 2;
    }
    RTC_SetTignTime(time);
    SYS_RecoverIrq(irq_status);

    /* LOW POWER-sleep 模式 */
    if (!RTCTigFlag) {
        LowPower_Sleep(RB_PWR_RAM2K | RB_PWR_RAM30K | RB_PWR_EXTEND);
        if (RTCTigFlag) {   /* 注意：若使用仅 RTC 唤醒方式，需注意此时 32M 尚未稳定 */
            time += WAKE_UP_RTC_MAX_TIME;
            if (time > 0xA8C00000) {
                time -= 0xA8C00000;
            }
            RTC_SetTignTime(time);
            LowPower_Idle();
        }
        HSECFG_Current(HSE_RCur_100);   /* 改为额定电流（低功耗后需把 HSE 偏置调低） */
    } else {
        return 3;
    }
#endif
    return 0;
}

void sdk_sleep(uint32_t max_ms)
{
    /* CH58X_LowPower(uint32_t time)（本文件「低功耗」段）的 time 是 **RTC 绝对时刻**
     * （32k 周期计数），不是 TMOS/系统 tick，故不能传 TMOS_GetSystemClock()。
     * 取当前 RTC 计数 + max_ms 折算值；32k 计数器满量程 RTC_TIMER_MAX_VALUE 需回绕。
     * 该函数仅在 HAL_SLEEP==TRUE 时真正睡眠；返回 0 正常，2/3 表示本次跳过睡眠。
     * s_lowpower_allowed 是本工程策略层唯一的合法授权点：桥的睡眠策略判定已在上层
     * （bridge_ch58x_idle → bridge_ch58x_can_sleep / bridge_ch58x_shallow_ok）完成，
     * 走到这里即表示许可。 */
    uint32_t now  = RTC_GetCycle32k();
    uint32_t tick = (uint32_t)(((uint64_t)max_ms * (uint64_t)FREQ_RTC) / 1000u);
    uint32_t wake = now + tick;

    if ((wake < now) || (wake > RTC_TIMER_MAX_VALUE)) {
        wake -= RTC_TIMER_MAX_VALUE;
    }
    s_lowpower_allowed = true;
    (void)CH58X_LowPower(wake);
    s_lowpower_allowed = false;
}

/* ============================ 看门狗 ============================
 * CH58x WDOG：计数时钟 = Fsys/131072（60MHz → 1 count ≈ 2.185ms），
 * 8bit 计数从重装值计到 0xFF 溢出；溢出置 RB_WDOG_INT_FLAG，
 * 若 RB_WDOG_RST_EN=1 则产生复位（本工程只用复位，不使能中断）。
 * WWDG_SetCounter(c) 写入即重装并清溢出标志，故"喂狗"= 重写重装值。 */
#define SDK_WDOG_COUNT_US  2185u    /* ceil(131072/60e6*1e6) = 2184.53µs */
#define SDK_WDOG_MAX_COUNT 255u

static uint8_t s_wdog_reload;
static bool    s_wdog_on;

void sdk_wdog_init(uint16_t timeout_ms)
{
    uint32_t counts;

    if (timeout_ms == 0u) {
        sdk_wdog_suspend();
        s_wdog_on = false;
        return;
    }
    /* counts = ceil(timeout_ms / 2.185ms) */
    counts = ((uint32_t)timeout_ms * 1000u + (SDK_WDOG_COUNT_US - 1u)) / SDK_WDOG_COUNT_US;
    if (counts == 0u) {
        counts = 1u;
    }
    if (counts >= (uint32_t)SDK_WDOG_MAX_COUNT) {
        counts = (uint32_t)SDK_WDOG_MAX_COUNT - 1u;   /* 至少留 1 count 余量 */
    }
    s_wdog_reload = (uint8_t)((uint32_t)SDK_WDOG_MAX_COUNT - counts);

    WWDG_ITCfg(DISABLE);
    WWDG_ResetCfg(ENABLE);
    WWDG_SetCounter(s_wdog_reload);
    s_wdog_on = true;
}

void sdk_wdog_feed(void)
{
    if (s_wdog_on) {
        WWDG_SetCounter(s_wdog_reload);
    }
}

void sdk_wdog_suspend(void)
{
    /* 只关复位使能（计数与溢出标志保留）：睡眠时长可能超过看门狗周期 */
    WWDG_ResetCfg(DISABLE);
}

void sdk_wdog_resume(void)
{
    if (s_wdog_on) {
        WWDG_SetCounter(s_wdog_reload);   /* 先重装清计数 */
        WWDG_ResetCfg(ENABLE);
    }
}

/* ============================ TMOS ============================ */

void sdk_tmos_process(void)
{
    TMOS_SystemProcess();
}

/* ============================ BLE ============================ */

int sdk_ble_stack_init(void)
{
    if (s_ble_inited) {
        return BRIDGE_OK;       /* 幂等：main 与 port_radio_init 都会调用 */
    }

    /* 顺序与官方 HID_Keyboard 例程 hidkbd_main.c 完全一致，不可调整：
     *   CH58X_BLEInit()        协议栈内存/参数初始化（用 MEM_BUF）
     *   HAL_Init()             HAL 任务 + 时间服务
     *   GAPRole_PeripheralInit() GAP 角色初始化
     *   HidDev_Init()          HID service 注册（hiddev.c）
     *   HidEmu_Init()          HOGP 应用任务 + 广播数据/绑定参数（hidkbd.c） */
    CH58X_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    HidDev_Init();
    HidEmu_Init();
    /* HidEmu_Init 内部先 Hid_AddService() 再 HidDev_Register(厂商回调)，
     * 故必须在它之后覆盖：屏蔽厂商演示键码定时器，并把连接/断开、
     * LED 输出接入本工程（见 bridge_ble_hidsvc.c）。 */
    bridge_ble_hidsvc_override_cbs();
    bridge_ble_hidsvc_set_adv_intent(true);
    /* 多主机：上电装载 4 槽绑定索引表 */
    bridge_mh_init();
    /* HOGP 之外再挂一个自定义 GATT 透传服务（TX notify / RX write，见文末） */
    (void)sdk_ble_tunnel_svc_init();

    s_ble_inited = true;
    s_ble_conn   = SDK_BLE_CONN_INVALID;
    return BRIDGE_OK;
}

int sdk_ble_deinit(void)
{
    uint8_t adv = FALSE;

    if (!s_ble_inited) {
        return BRIDGE_OK;
    }
    /* 先撤广播意图，否则状态回调会在 GAPROLE_WAITING 时把广播又打开 */
    bridge_ble_hidsvc_set_adv_intent(false);
    if (s_ble_conn != SDK_BLE_CONN_INVALID) {
        /* 主动断链：标记后随后的 link-terminated 事件不再上报 core（否则 core 会把
         * 它当成意外掉线，重入 port_radio_connect_profile / 误入配对窗口 → 见 Fix 1 说明）。 */
        s_ble_teardown       = true;
        /* 模式切换（关 BLE）：放弃延后的切槽请求，避免断链回调里又被补做并重开广播 */
        s_ble_switch_pending = 0u;
        /* 断开当前连接，交由协议栈回调走 sdk_ble_on_disconnect() */
        (void)GAPRole_TerminateLink(s_ble_conn);
        s_ble_conn = SDK_BLE_CONN_INVALID;
    }
    /* 停止广播（配对窗口结束） */
    (void)GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv);
    return BRIDGE_OK;
}

int sdk_ble_hid_send(uint8_t rid, const uint8_t *d, uint8_t len)
{
    if (s_ble_conn == SDK_BLE_CONN_INVALID) {
        return BRIDGE_ERR_STATE;
    }
    /* 转发路径：不得在此打印日志（规格 11.1）。
     * 载荷不含 Report ID（Report ID 由 Report Reference 描述符告知主机），
     * type 固定为 Input（hiddev.h：HID_REPORT_TYPE_INPUT=1），
     * rid 由 port 层按报文类型给出（见 bridge_ble_hidsvc.h）。 */
    return (HidDev_Report(rid, HID_REPORT_TYPE_INPUT, len, (uint8_t *)d) == SUCCESS)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

bool sdk_ble_is_connected(void)
{
    return (s_ble_conn != SDK_BLE_CONN_INVALID);
}

int sdk_ble_pair_start(uint16_t timeout_s)
{
    uint8_t adv = TRUE;
    (void)timeout_s;    /* 绑定超时由 GAPBondMgr/GAP 参数控制（见 sdk_ble_connect_profile） */

    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }
    s_ble_teardown = false;     /* BLE 重新激活：恢复正常的掉线上报 */
    /* 进入配对窗口 = 重新希望保持可连接：允许状态回调自动维持广播 */
    bridge_ble_hidsvc_set_adv_intent(true);
    return (GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv) == SUCCESS)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

int sdk_ble_pair_stop(void)
{
    uint8_t adv = FALSE;

    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }
    /* 先撤广播意图，否则状态回调会在 GAPROLE_WAITING 时把广播又打开 */
    bridge_ble_hidsvc_set_adv_intent(false);
    /* 停止广播（配对窗口结束） */
    return (GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv) == SUCCESS)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

/* ---------------- 按槽区分的 BLE 设备地址 ----------------
 * 根因：原实现所有 Profile 共用芯片出厂 MAC。PC（Windows）按「设备地址」记忆绑定，
 * 于是切到未配对的槽再广播时，PC 认出旧地址便自动重连 → 模组 RAM 内无对应绑定 →
 * 加密失败立即断开 → WAITING 重开广播 → 再被连……形成「一直连、一直退」死循环。
 *
 * 方案：为每个 Profile 派生**互不相同**的静态随机地址（由芯片 MAC 派生，最高两 bit=11
 * 满足 ADDRTYPE_STATIC 规范，低 6 bit 编码槽号）。不同槽对外表现为不同设备，PC 不会把
 * A 槽的绑定套用到 B 槽，从根上阻断误重连。
 * 约束：GAP_ConfigDeviceAddr 在有活动连接时返回 bleIncorrectMode、GAP 未就绪时返回
 * bleNotReady，故应用地址须在「无连接」且「GAPROLE_STARTED 之后」进行：
 *   - 上电首次应用：GAPROLE_STARTED 回调（sdk_ble_gap_ready）；
 *   - 运行时切槽：断开连接后（sdk_ble_switch_exec）；
 *   - 有连接时的切槽请求：记入 s_ble_switch_pending，断链后补做（sdk_ble_resume_pending）。 */
static void sdk_ble_profile_addr(uint8_t profile, uint8_t addr[6])
{
    sdk_ble_mac(addr);
    /* BD_ADDR 为 LSB-first（addr[5] 为最高字节）。静态随机地址要求最高两 bit=11；
     * 低 6 bit 放槽号（1..4）→ 各槽地址互不相同。 */
    addr[5] = (uint8_t)(0xC0u | (profile & 0x3Fu));
}

static int sdk_ble_apply_addr(uint8_t profile)
{
    uint8_t addr[6];

    if (profile < 1u || profile > BRIDGE_MAX_BLE_PROFILE) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_ble_addr_prof == profile) {
        return BRIDGE_OK;                       /* 幂等：已是该槽地址 */
    }
    if (s_ble_conn != SDK_BLE_CONN_INVALID) {
        return BRIDGE_ERR_STATE;                /* 有活动连接：库会拒绝，延后处理 */
    }
    sdk_ble_profile_addr(profile, addr);
    if (GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, addr) != SUCCESS) {
        return BRIDGE_ERR_IO;                   /* 多为 GAP 未就绪(bleNotReady)，稍后补 */
    }
    s_ble_addr_prof = profile;
    return BRIDGE_OK;
}

/* 执行切槽（前提：无活动连接）。停广播 → 标记活跃 → 应用目标槽专属地址。
 *
 * 关键：**不再清库绑定（GAPBOND_ERASE_ALLBONDS）、不再整段还原 SNV**。
 *   - SDK BLE_SNV_NUM=4，闭源 GAPBondMgr 原生记忆 4 个主机绑定；ERASE_ALLBONDS 会
 *     抹掉库 RAM 内的全部 bond/CCCD，而库不会据此重载（无公开「从 SNV 重载 RAM」API），
 *     导致已绑定主机重连时加密失败 → 反复连接/断开。
 *   - 各槽使用不同的 BLE 设备地址，库按**主机(peer)地址**匹配绑定，切槽只换本机地址
 *     与广播，绑定数据仍在库内完好，无需应用层搬移。 */
static int sdk_ble_switch_exec(uint8_t profile)
{
    /* 1) 先停广播（同时撤意图，避免状态回调在 WAITING 用旧地址重开广播） */
    (void)sdk_ble_pair_stop();

    /* 2) 标记活跃 + 应用该槽地址（若协议栈尚未就绪，GAPROLE_STARTED 会补） */
    (void)bridge_mh_set_active(profile);
    s_ble_addr_prof = 0u;                       /* 切槽后地址必变，强制重新应用 */
    (void)sdk_ble_apply_addr(profile);
    return BRIDGE_OK;
}

int sdk_ble_connect_profile(uint8_t profile)
{
    if (profile < 1u || profile > BRIDGE_MAX_BLE_PROFILE) {
        return BRIDGE_ERR_PARAM;
    }
    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }
    s_ble_teardown = false;     /* BLE 重新激活：恢复正常的掉线上报 */

    /* 目标 Profile == 当前活跃 Profile：库 RAM 内已持有其绑定（协议栈初始化时自 SNV 区
     * 装载，或配对时由库写入），无需切槽，只确保地址已应用并恢复可连接广播。
     * SNVNum=4 使库可同时记住 4 个主机绑定，切换其它槽不会丢失本槽绑定。 */
    if (bridge_mh_active() == profile) {
        (void)bridge_mh_set_active(profile);
        /* 确保该槽地址已应用（幂等；有连接或未就绪时忽略，由 GAPROLE_STARTED 补） */
        (void)sdk_ble_apply_addr(profile);
        return sdk_ble_pair_start((uint16_t)BRIDGE_PAIR_TIMEOUT_S);
    }

    if (s_ble_conn != SDK_BLE_CONN_INVALID) {
        /* 有活动连接：地址只能在无连接时改。先记下目标槽并主动断链，断链后的
         * GAPROLE_WAITING 由 sdk_ble_resume_pending() 补做切槽；s_ble_teardown=true
         * 使本次断链不上报 core（避免其重入重连）。 */
        s_ble_switch_pending = profile;
        s_ble_teardown = true;
        (void)GAPRole_TerminateLink(s_ble_conn);
        return BRIDGE_OK;
    }

    /* 无连接：立即切槽并进入配对 / 可连接窗口 */
    if (sdk_ble_switch_exec(profile) != BRIDGE_OK) {
        return BRIDGE_ERR_IO;
    }
    return sdk_ble_pair_start((uint16_t)BRIDGE_PAIR_TIMEOUT_S);
}

/* 断链后补做被延后的切槽（有连接时无法改地址）。返回 true 表示已接管广播。 */
bool sdk_ble_resume_pending(void)
{
    uint8_t p = s_ble_switch_pending;

    if (p < 1u || p > BRIDGE_MAX_BLE_PROFILE) {
        return false;
    }
    s_ble_switch_pending = 0u;
    if (sdk_ble_switch_exec(p) != BRIDGE_OK) {
        return false;
    }
    (void)sdk_ble_pair_start((uint16_t)BRIDGE_PAIR_TIMEOUT_S);
    /* 切槽属「主动断链」：前面的 sdk_ble_on_disconnect() 因 s_ble_teardown 抑制了上报，
     * 若就此返回，core 的 s_connected 会永远停在 true（真机现象：切槽后 GET_STATUS 一直
     * connected=1、WL=BLE_LINKED，实际已断开并在新槽广播）。此处在切槽完成后补报一次断链，
     * 让 core 刷新 s_connected / release_all / EVT_STATE。安全性：此时 bridge_mh_active()
     * 已是目标槽，core 断链处理里对 BLE 的重连回调 port_radio_connect_profile(cfg->ble_profile)
     * 与目标槽同槽 → 命中同槽短路分支（不改地址、不清库），不会重入切槽。 */
    bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_DISCONNECTED);
    return true;
}

/* GAPROLE_STARTED：协议栈就绪，此刻是设置设备地址的合法时机（厂商 hidkbd.c 同）。
 * 应用当前活跃槽（无活跃槽则用配置默认槽）的专属地址，确保首次广播即用正确地址。 */
void sdk_ble_gap_ready(void)
{
    uint8_t p;

    if (!s_ble_inited) {
        return;
    }
    p = bridge_mh_active();
    if (p < 1u || p > BRIDGE_MAX_BLE_PROFILE) {
        p = bridge_cfg_get()->ble_profile;
    }
    if (p < 1u || p > BRIDGE_MAX_BLE_PROFILE) {
        return;
    }
    (void)sdk_ble_apply_addr(p);
}

int sdk_ble_unpair(uint8_t profile)
{
    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }

    if (profile == 0xFFu) {
        /* 解除全部：清库绑定记录 + 清空全部槽 */
        (void)GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS, 0, NULL);
        (void)bridge_mh_erase_all();
        /* 规格 7.4：解除全部回 MODE_BLE_PAIR，由上层(mode 层)在 UNPAIR 处理后切换 */
        return BRIDGE_OK;
    }

    /* 解除单槽：清该槽「已配对」标志（本工程不记录 peer 地址，库内绑定记录
     * 由 GAPBOND_ERASE_SINGLEBOND 需 peer 参数，暂不单删；该槽切离后不再标记活跃，
     * 主机侧删除设备即彻底断开） */
    if (profile < 1u || profile > BRIDGE_MAX_BLE_PROFILE) {
        return BRIDGE_ERR_PARAM;
    }
    return (bridge_mh_erase(profile) == BRIDGE_MH_OK) ? BRIDGE_OK : BRIDGE_ERR_IO;
}

int sdk_ble_set_conn_interval(uint8_t units_1p25ms)
{
    uint16_t itv = (uint16_t)units_1p25ms;
    return (GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t), &itv) == SUCCESS)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

/* ---------------- 广播名（advertData 动态重建） ----------------
 * 参考 hidkbd.c：广告数据 = flags + appearance + Complete Local Name；
 * 设备名同时写 GGS（GGS_DEVICE_NAME_ATT），主机设备列表显示的就是它。
 * 广播名 = 基准名（CMD_SET_NAME，持久化于 cfg）+ 可选 MAC 后缀（D6）。 */
#define SDK_BLE_NAME_MAX       GAP_DEVICE_NAME_LEN   /* 21 */
#define SDK_BLE_ADV_MAX        B_MAX_ADV_LEN         /* 31（legacy 广播包上限） */
#define SDK_BLE_MAC_SUFFIX_LEN 5u                    /* "-XXXX" */

static uint8_t  s_ble_base_name[SDK_BLE_NAME_MAX];
static uint8_t  s_ble_base_len;
static bool     s_ble_name_ready;
static uint8_t  s_ble_adv[SDK_BLE_ADV_MAX];
static uint16_t s_ble_adv_len;

static char sdk_hex_digit(uint8_t v)
{
    return (v < 10u) ? (char)('0' + (char)v) : (char)('A' + (char)(v - 10u));
}

/* 用基准名（+可选 MAC 后缀）重建 advertData 与 GGS 设备名 */
static void sdk_ble_apply_adv(void)
{
    uint8_t final_name[SDK_BLE_NAME_MAX];
    uint8_t flen = s_ble_base_len;
    uint8_t i    = 0u;

    if (!s_ble_inited) {
        return;
    }
    if (!s_ble_name_ready) {
        /* 尚未收到 CMD_SET_NAME：用编译期默认名兜底 */
        flen = (uint8_t)(sizeof(BRIDGE_BLE_NAME_DEFAULT) - 1u);
        if (flen > SDK_BLE_NAME_MAX) {
            flen = SDK_BLE_NAME_MAX;
        }
        memcpy(final_name, BRIDGE_BLE_NAME_DEFAULT, flen);
    } else {
        if (flen > SDK_BLE_NAME_MAX) {
            flen = SDK_BLE_NAME_MAX;
        }
        memcpy(final_name, s_ble_base_name, flen);
    }

    if (s_adv_name_mac && ((uint8_t)(SDK_BLE_NAME_MAX - flen) >= SDK_BLE_MAC_SUFFIX_LEN)) {
        uint8_t  mac[6];
        uint16_t m;

        sdk_ble_mac(mac);
        m = (uint16_t)(((uint16_t)mac[4] << 8) | (uint16_t)mac[5]);   /* 低 16 位 */
        final_name[flen++] = '-';
        final_name[flen++] = sdk_hex_digit((uint8_t)((m >> 12) & 0xFu));
        final_name[flen++] = sdk_hex_digit((uint8_t)((m >> 8) & 0xFu));
        final_name[flen++] = sdk_hex_digit((uint8_t)((m >> 4) & 0xFu));
        final_name[flen++] = sdk_hex_digit((uint8_t)(m & 0xFu));
    }

    /* [flags][appearance][Complete Local Name]；总长 <= 31 */
    s_ble_adv[i++] = 0x02;
    s_ble_adv[i++] = GAP_ADTYPE_FLAGS;
    s_ble_adv[i++] = GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED;
    s_ble_adv[i++] = 0x03;
    s_ble_adv[i++] = GAP_ADTYPE_APPEARANCE;
    s_ble_adv[i++] = LO_UINT16(GAP_APPEARE_HID_KEYBOARD);
    s_ble_adv[i++] = HI_UINT16(GAP_APPEARE_HID_KEYBOARD);
    s_ble_adv[i++] = (uint8_t)(flen + 1u);
    s_ble_adv[i++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    memcpy(&s_ble_adv[i], final_name, flen);
    i = (uint8_t)(i + flen);
    s_ble_adv_len = i;

    /* 先更新 GAPRole 侧参数，再推给控制器（广播中即时生效） */
    (void)GAPRole_SetParameter(GAPROLE_ADVERT_DATA, s_ble_adv_len, s_ble_adv);
    (void)GAP_UpdateAdvertisingData(INVALID_TASK_ID, TRUE, s_ble_adv_len, s_ble_adv);

    /* GGS 设备名：定长 GAP_DEVICE_NAME_LEN，不足补 0 */
    {
        uint8_t ggs[SDK_BLE_NAME_MAX];

        memset(ggs, 0, sizeof(ggs));
        memcpy(ggs, final_name, flen);
        (void)GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, ggs);
    }
}

int sdk_ble_set_adv_name(const uint8_t *name, uint8_t len)
{
    if (name == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }
    if (len > SDK_BLE_NAME_MAX) {
        len = SDK_BLE_NAME_MAX;     /* GAP_DEVICE_NAME_LEN 上限，超长截断 */
    }
    memcpy(s_ble_base_name, name, len);
    s_ble_base_len  = len;
    s_ble_name_ready = true;

    sdk_ble_apply_adv();
    return BRIDGE_OK;
}

int sdk_ble_set_adv_name_with_mac(bool en)
{
    s_adv_name_mac = en ? true : false;
    sdk_ble_apply_adv();            /* 用已保存的基准名重建 */
    return BRIDGE_OK;
}

/* ---------------- SRRC 定频（D9） ----------------
 * 参考厂商用例 EVT/EXAM/BLE/Direct_Test_Mode/APP/RF_dtm.c：
 *   LL_SingleChannel(ch)      单载波定频发射
 *   LL_SetTxPowerLevel(pwr)   设置功率（LL_TX_POWEER_* 档位）
 *   LL_TestEnd(NULL)          结束测试
 * mode：0=退出，1=进入定频（core 的 bridge_test 只发这两种）。 */
static uint8_t sdk_test_power_level(uint8_t power)
{
    static const uint8_t k_levels[4] = {
        LL_TX_POWEER_0_DBM, LL_TX_POWEER_3_DBM, LL_TX_POWEER_4_DBM, LL_TX_POWEER_6_DBM
    };

    return k_levels[(power < 4u) ? power : 3u];
}

int sdk_ble_test_mode(uint8_t mode, uint8_t channel, uint8_t power)
{
    if (!s_ble_inited) {
        return BRIDGE_ERR_STATE;
    }
    if (mode == 0u) {
        (void)LL_TestEnd(NULL);
        return BRIDGE_OK;
    }
    if (mode != 1u || channel > 39u) {
        return BRIDGE_ERR_PARAM;
    }
    (void)LL_SetTxPowerLevel(sdk_test_power_level(power));
    return (LL_SingleChannel(channel) == SUCCESS) ? BRIDGE_OK : BRIDGE_ERR_IO;
}

uint8_t sdk_ble_protocol_ver(void)
{
    /* 报文协议版本：0.1.0 → 见 bridge_proto.h；此处返回 minor 供 port 层使用 */
    return 0x01u;
}

/* ============================ BLE GATT 透传服务（0xFFF0） ============================
 * 参考例程：EVT/EXAM/BLE/BLE_USB/Profile/ble_usb_service.c。
 * CH58x 协议栈**无** GATT 属性表宏，属性表必须手写 gattAttribute_t[]，
 * handle 由 GATTServApp_RegisterService() 注册时回填。
 *   TX 0xFFF1 notify：键盘 → 主机（sdk_ble_tunnel_send）
 *   RX 0xFFF2 write ：主机 → 键盘（写入后转 bridge_ch58x_radio_tunnel_in）
 * 手机 APP 连上 HOGP 后即可发现本服务做双向透传。 */
#define SDK_TUN_SVC_UUID  0xFFF0u
#define SDK_TUN_TX_UUID   0xFFF1u
#define SDK_TUN_RX_UUID   0xFFF2u
#define SDK_TUN_RX_MAX    128u          /* RX 值缓冲，覆盖单包 MTU-3（<=509） */

/* 属性表下标（与 s_tun_attr[] 顺序一一对应） */
#define SDK_TUN_IDX_TX_VAL  2u
#define SDK_TUN_IDX_RX_VAL  5u

static const uint8_t s_tun_svc_uuid[ATT_BT_UUID_SIZE] = {
    (uint8_t)(SDK_TUN_SVC_UUID & 0xFFu), (uint8_t)(SDK_TUN_SVC_UUID >> 8)
};
static const uint8_t s_tun_tx_uuid[ATT_BT_UUID_SIZE] = {
    (uint8_t)(SDK_TUN_TX_UUID & 0xFFu), (uint8_t)(SDK_TUN_TX_UUID >> 8)
};
static const uint8_t s_tun_rx_uuid[ATT_BT_UUID_SIZE] = {
    (uint8_t)(SDK_TUN_RX_UUID & 0xFFu), (uint8_t)(SDK_TUN_RX_UUID >> 8)
};
static const gattAttrType_t s_tun_svc = { ATT_BT_UUID_SIZE, s_tun_svc_uuid };

static uint8_t       s_tun_tx_props = GATT_PROP_NOTIFY;
static uint8_t       s_tun_rx_props = GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RSP;
static uint8_t       s_tun_tx_val;                       /* notify 特征值占位 */
static uint8_t       s_tun_rx_val[SDK_TUN_RX_MAX];
static gattCharCfg_t s_tun_tx_cccd[GATT_MAX_NUM_CONN];

static gattAttribute_t s_tun_attr[] = {
    { { ATT_BT_UUID_SIZE, primaryServiceUUID }, GATT_PERMIT_READ, 0, (uint8_t *)&s_tun_svc },
    { { ATT_BT_UUID_SIZE, characterUUID },      GATT_PERMIT_READ, 0, &s_tun_tx_props },
    { { ATT_BT_UUID_SIZE, s_tun_tx_uuid },      0,               0, &s_tun_tx_val },
    { { ATT_BT_UUID_SIZE, clientCharCfgUUID },  GATT_PERMIT_READ | GATT_PERMIT_WRITE, 0,
      (uint8_t *)s_tun_tx_cccd },
    { { ATT_BT_UUID_SIZE, characterUUID },      GATT_PERMIT_READ, 0, &s_tun_rx_props },
    { { ATT_BT_UUID_SIZE, s_tun_rx_uuid },      GATT_PERMIT_WRITE, 0, s_tun_rx_val },
};

static bStatus_t sdk_tun_read_cb(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                 uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method)
{
    (void)connHandle;
    (void)offset;
    (void)maxLen;
    (void)method;

    if (pAttr->type.len == ATT_BT_UUID_SIZE &&
        BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]) == GATT_CLIENT_CHAR_CFG_UUID) {
        *pLen = 2u;
        tmos_memcpy(pValue, pAttr->pValue, 2u);
    }
    return SUCCESS;
}

static bStatus_t sdk_tun_write_cb(uint16_t connHandle, gattAttribute_t *pAttr, uint8_t *pValue,
                                  uint16_t len, uint16_t offset, uint8_t method)
{
    (void)method;

    if (pAttr->type.len != ATT_BT_UUID_SIZE) {
        return SUCCESS;
    }
    if (BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]) == GATT_CLIENT_CHAR_CFG_UUID) {
        return GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len, offset,
                                              GATT_CLIENT_CFG_NOTIFY);
    }
    if (pAttr->handle == s_tun_attr[SDK_TUN_IDX_RX_VAL].handle) {
        /* 主机写入的透传数据 → 上报 SK32（键盘侧）*/
        bridge_ch58x_radio_tunnel_in(BRIDGE_TUNNEL_BLE, pValue, (uint8_t)len);
    }
    return SUCCESS;
}

static void sdk_tun_link_cb(uint16_t connHandle, uint8_t changeType)
{
    if (connHandle != LOOPBACK_CONNHANDLE) {
        if ((changeType == LINKDB_STATUS_UPDATE_REMOVED) ||
            ((changeType == LINKDB_STATUS_UPDATE_STATEFLAGS) && (!linkDB_Up(connHandle)))) {
            GATTServApp_InitCharCfg(connHandle, s_tun_tx_cccd);
        }
    }
}

static gattServiceCBs_t s_tun_cbs = { sdk_tun_read_cb, sdk_tun_write_cb, NULL };

static int sdk_ble_tunnel_svc_init(void)
{
    GATTServApp_InitCharCfg(INVALID_CONNHANDLE, s_tun_tx_cccd);
    (void)linkDB_Register(sdk_tun_link_cb);
    return (GATTServApp_RegisterService(s_tun_attr, GATT_NUM_ATTRS(s_tun_attr),
                                        GATT_MAX_ENCRYPT_KEY_SIZE, &s_tun_cbs) == SUCCESS)
               ? BRIDGE_OK
               : BRIDGE_ERR_IO;
}

int sdk_ble_tunnel_send(const uint8_t *d, uint8_t len)
{
    attHandleValueNoti_t noti;
    uint16_t             mtu;
    uint8_t             *p;

    if (d == 0 || len == 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_ble_conn == SDK_BLE_CONN_INVALID) {
        return BRIDGE_ERR_STATE;
    }
    /* 未订阅 notify（CCCD 未置位）时无处可发 */
    if ((GATTServApp_ReadCharCfg(s_ble_conn, s_tun_tx_cccd) & GATT_CLIENT_CFG_NOTIFY) == 0u) {
        return BRIDGE_ERR_STATE;
    }

    mtu = ATT_GetMTU(s_ble_conn);
    if (mtu <= 3u) {
        return BRIDGE_ERR_IO;
    }
    if ((uint16_t)len > (uint16_t)(mtu - 3u)) {
        return BRIDGE_ERR_PARAM;    /* 分片由上层负责（本工程透传单帧 <= 64B） */
    }

    p = (uint8_t *)GATT_bm_alloc(s_ble_conn, ATT_HANDLE_VALUE_NOTI, len, NULL, 0);
    if (p == 0) {
        return BRIDGE_ERR_IO;
    }
    memcpy(p, d, len);

    noti.handle = s_tun_attr[SDK_TUN_IDX_TX_VAL].handle;
    noti.len    = len;
    noti.pValue = p;

    if (GATT_Notification(s_ble_conn, &noti, FALSE) != SUCCESS) {
        GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        return BRIDGE_ERR_IO;
    }
    return BRIDGE_OK;
}

/* ============================ 2.4G（D11） ============================
 * 实施位置：port/ch58x/bridge_rf24_ch58x.c（同样 #include SDK 头的边界文件）。
 * 该文件按厂商 RF_PHY 例程（EVT/EXAM/BLE/RF_PHY）实现 sdk_rf24_* 全套原语：
 *   RF_RoleInit → RF_Config → RF_Tx/RF_Rx/RF_Shut，TMOS 任务上下文投递，
 *   1s 心跳 + 连续 3 次无收判断链。
 * 说明：**不要**在本文件再定义 sdk_rf24_*，否则与上述文件重复符号。
 * 数据面类型前缀（BRIDGE_RADIO_PKT_*）与回调翻译见 bridge_ch58x_callbacks.c。
 * ==================================================================== */

/* ============================ USB（Dongle） ============================
 * 实施位置：port/ch58x/bridge_usb_ch58x.c（同样 #include SDK 头的边界文件）。
 * 该文件按厂商 USB 复合设备例程（EVT/EXAM/USB/Device/CompoundDev）与
 * BLE+USB 共存例程（EVT/EXAM/BLE/BLE_USB/APP/app_usb.c）实现：
 *   USB_DeviceInit + PFIC_EnableIRQ(USB_IRQn) + USB_IRQHandler，
 *   设备/配置/字符串描述符 + 4 张 HID 报表描述符，
 *   EP1 键盘 8B / EP2 辅助键（Report ID：鼠标/系统/多媒体/NKRO）/
 *   EP3 VIA Raw HID 32B（usage page 0xFF60 / usage 0x61），
 *   以及 EP0 SET_REPORT（LED）与 EP3 OUT（Raw HID → 键盘）回调。
 * 说明：**不要**在本文件再定义 sdk_usb_*，否则与上述文件重复符号。
 * 注意：CH58x 上 BLE 与 USB 共用时钟/PLL，USB 初始化必须在
 *       sdk_ble_stack_init() 之后（见 board/main.c 与 bridge_ch58x_init()）。
 * ==================================================================== */

/* ============================ 厂商例程回调入口 ============================ */

void sdk_ble_on_connect(uint16_t conn_handle)
{
    uint8_t p;

    bridge_dbg_mark(0x50);      /* 进入 sdk_ble_on_connect */
    s_ble_conn = conn_handle;

    /* 连接建立：此刻生效的槽 = 当前已应用的 BLE 地址所属槽 s_ble_addr_prof
     * （配对窗口 / 切槽 / GAPROLE_STARTED 均会设置它）。回退顺序：活跃槽 → 配置默认槽。
     * 把该槽标记为「已配对」并置为活跃，保证：
     *   · GET_BLE_SLOTS 正确反映绑定状态；
     *   · 后续「切 2.4G 再切回 BLE」命中同槽短路，不再走多余路径。
     * 仅在状态确有变化时落盘，避免重连风暴写坏 NV。 */
    p = s_ble_addr_prof;
    if (p < 1u || p > BRIDGE_MAX_BLE_PROFILE) {
        p = bridge_mh_active();
    }
    if (p < 1u || p > BRIDGE_MAX_BLE_PROFILE) {
        p = bridge_cfg_get()->ble_profile;
    }
    if (p >= 1u && p <= BRIDGE_MAX_BLE_PROFILE) {
        if (!bridge_mh_has(p) || bridge_mh_active() != p) {
            bridge_dbg_mark(0x51);          /* 即将同步写 NV（Flash 擦写，关总中断） */
            (void)bridge_mh_mark_bound(p);
            bridge_dbg_mark(0x52);          /* NV 写已返回 */
        }
    }
    bridge_dbg_mark(0x53);                  /* 即将上报 core（radio 事件） */
    bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_CONNECTED);
    bridge_dbg_mark(0x54);                  /* sdk_ble_on_connect 返回 */
}

void sdk_ble_on_disconnect(uint16_t conn_handle)
{
    (void)conn_handle;
    s_ble_conn = SDK_BLE_CONN_INVALID;
    /* 主动断链（模式切换/停用 BLE）：不上报 core，避免其把断链当成意外掉线而重连或
     * 误入配对窗口（模式状态由 core 在 mode_transaction 中自行主导）。 */
    if (s_ble_teardown) {
        s_ble_teardown = false;
        return;
    }
    bridge_ch58x_radio_evt(BRIDGE_RADIO_EVT_DISCONNECTED);
}

/* 说明：sdk_ble_on_led_report() / sdk_ble_on_raw_report() / sdk_rf24_on_recv() /
 *      sdk_rf24_on_link() / sdk_usb_on_led_report() 这些**纯翻译**回调不依赖
 *      SDK 头，已移至 bridge_ch58x_callbacks.c 以便纳入主机端验证。
 *      留在此处的只有需要维护 SDK 侧连接句柄状态的两个回调。 */

/* ============================ 中断向量 ============================ */

/* UART1（桥数据面）：只做「读空 FIFO → 喂 core 环形缓冲」，不做任何业务判断。
 * 必须带 __INTERRUPT（= interrupt("WCH-Interrupt-fast")，见 CH58x_common.h）：
 * 启动文件 startup_CH583.S 把 mtvec 配成向量模式（_vector_base | 3），硬件直接
 * 跳进 ISR，需按 WCH 中断 ABI 保存现场并用 mret 返回，普通函数会用 ra 返回而跑飞。
 * 还必须带 __HIGH_CODE 常驻 RAM：睡眠唤醒后 FlashROM 电源尚未恢复
 * （CH58X_LowPower → LowPower_Sleep 未保留 RB_PWR_XROM），此时执行 flash 内代码
 * 不可行（见 CH58x_pwr.c LowPower_Sleep 注释）；ISR 驻留 RAM 才能在唤醒瞬间执行。 */
__INTERRUPT
__HIGH_CODE
void UART1_IRQHandler(void)
{
    bridge_ch58x_uart_isr();
}

/* GPIOA：仅 PA8(RXD1) 用作 Sleep 唤醒源（下降沿 = 起始位）。
 * 唤醒后字节由 UART1 中断/FIFO 处理，这里除清标志外只置「唤醒过」痕迹，
 * 供 port 层续期「接收活动保持清醒」窗口（避免睡眠期内帧首字节被吞）。
 * __HIGH_CODE 理由同上（与 SDK 官方 PM 例程 GPIOA_IRQHandler 一致，
 * 也与本工程 bridge_usb_ch58x.c 的 USB_IRQHandler 写法一致）。 */
__INTERRUPT
__HIGH_CODE
void GPIOA_IRQHandler(void)
{
    if ((R16_PA_INT_IF & (uint16_t)GPIO_Pin_8) != 0u) {
        s_rx_wake_flag = true;
    }
    GPIOA_ClearITFlagBit(GPIO_Pin_8);
}
