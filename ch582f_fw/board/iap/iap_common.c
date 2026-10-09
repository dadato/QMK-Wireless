/* IAP 共享原语实现（键盘 IAP 与 Dongle USB IAP 共用）—— 移植自 WCH EVT/EXAM/BLE/BackupUpgrade_IAP
 *
 * 与 WCH 原版差异：
 *   1. 标志位从 0x77000 改到 0x77200（NV 预留槽 8..11 的整页页首，页对齐）；
 *   2. 搬运长度取自标志记录（App 写入时落盘），不再盲拷 216K；
 *   3. 不做 printf（PRINT 依赖 UART1，本工程 UART1 为数据面）。
 */
#include "iap_common.h"
#include "bridge_ota_layout.h"
#include "CH58x_common.h"

/* 搬运/复验用中转缓冲：**必须 static 且 4 字节对齐**（FLASH_ROM_WRITE 要求）。
 * IAP 的 .stack 仅 512B，故不可放栈上（键盘 IAP 旧版用栈上 1KB 数组，靠
 * 栈溢出进空闲 RAM 侥幸可用；此处显式改为静态缓冲，行为不变且更稳）。 */
static __attribute__((aligned(4))) uint8_t s_iap_buf[1024];

/* ============================ DataFlash 标志记录 ============================ */

static void iap_rec_read(uint8_t *rec)
{
    EEPROM_READ(BRIDGE_OTA_FLAG_REL, rec, BRIDGE_OTA_REC_LEN);
}

void iap_flag_read(uint8_t *flag, uint32_t *len, uint16_t *crc)
{
    uint8_t rec[BRIDGE_OTA_REC_LEN] = {0};

    iap_rec_read(rec);
    if (flag != 0) {
        *flag = rec[0];
    }
    if (len != 0) {
        *len = (uint32_t)rec[1] | ((uint32_t)rec[2] << 8) |
               ((uint32_t)rec[3] << 16) | ((uint32_t)rec[4] << 24);
    }
    if (crc != 0) {
        *crc = (uint16_t)((uint16_t)rec[5] | ((uint16_t)rec[6] << 8));
    }
}

void iap_flag_write(uint8_t flag, uint32_t len, uint16_t crc)
{
    uint8_t rec[BRIDGE_OTA_REC_LEN];

    rec[0] = flag;
    rec[1] = (uint8_t)(len & 0xFFu);
    rec[2] = (uint8_t)((len >> 8) & 0xFFu);
    rec[3] = (uint8_t)((len >> 16) & 0xFFu);
    rec[4] = (uint8_t)((len >> 24) & 0xFFu);
    rec[5] = (uint8_t)(crc & 0xFFu);
    rec[6] = (uint8_t)(crc >> 8);
    rec[7] = 0u;

    /* 该 256B 页（槽 8..11）整页为 NV 预留区，可直接擦写；不伤 BLE SNV */
    EEPROM_ERASE(BRIDGE_OTA_FLAG_REL, BRIDGE_OTA_FLAG_PAGE);
    EEPROM_WRITE(BRIDGE_OTA_FLAG_REL, rec, BRIDGE_OTA_REC_LEN);
}

void iap_flag_enter_reset(uint8_t flag)
{
    iap_flag_write(flag, 0u, 0u);
    DelayMs(10u);
    SYS_ResetExecute();
    /* 不返回 */
}

/* ============================ CRC / 擦除 / 搬运 ============================ */

uint16_t iap_crc16_update(uint16_t crc, const uint8_t *d, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (uint8_t b = 0u; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t iap_crc16(const uint8_t *d, uint32_t len)
{
    return iap_crc16_update(0xFFFFu, d, len);
}

void iap_erase(uint32_t addr, uint32_t len)
{
    while (len > 0u) {
        FLASH_ROM_ERASE(addr, 4096u);
        addr += 4096u;
        len = (len > 4096u) ? (len - 4096u) : 0u;
    }
}

uint8_t iap_stage_write(uint32_t *erased, uint8_t *buf, uint32_t off, uint8_t len)
{
    uint32_t end = off + (uint32_t)len;

    while (*erased < ((end + 4095u) & ~4095u)) {      /* 按需擦 4K 页 */
        if (FLASH_ROM_ERASE(BRIDGE_OTA_STAGE_ADDR + *erased, 4096u) != 0u) {
            return 1u;
        }
        *erased += 4096u;
    }
    if (FLASH_ROM_WRITE(BRIDGE_OTA_STAGE_ADDR + off, buf, len) != 0u) {
        return 1u;
    }
    return 0u;
}

bool iap_copy_stage_to_app(void)
{
    uint8_t  flag = 0u;
    uint32_t len  = 0u;
    uint16_t crc  = 0u;
    uint16_t run  = 0xFFFFu;
    uint32_t done;
    uint32_t copy_len;

    iap_flag_read(&flag, &len, &crc);
    if (len == 0u || len > BRIDGE_OTA_STAGE_MAX) {
        return false;   /* 记录异常：不搬运 */
    }

    /* 搬运前复验暂存区 CRC（防 Flash 静默写坏）：不符则不碰 APP_A，回旧 App */
    for (done = 0u; done < len; done += sizeof(s_iap_buf)) {
        uint32_t n = (len - done > sizeof(s_iap_buf)) ? sizeof(s_iap_buf)
                                                      : (len - done);
        FLASH_ROM_READ(BRIDGE_OTA_STAGE_ADDR + done, s_iap_buf, n);
        run = iap_crc16_update(run, s_iap_buf, n);
    }
    if (run != crc) {
        return false;
    }

    copy_len = (len + 1023u) & ~1023u;      /* 1KB 向上取整 */
    iap_erase(BRIDGE_OTA_APP_ADDR, copy_len);

    for (done = 0u; done < copy_len; done += sizeof(s_iap_buf)) {
        uint32_t n = (copy_len - done > sizeof(s_iap_buf)) ? sizeof(s_iap_buf)
                                                           : (copy_len - done);
        FLASH_ROM_READ(BRIDGE_OTA_STAGE_ADDR + done, s_iap_buf, n);
        FLASH_ROM_WRITE(BRIDGE_OTA_APP_ADDR + done, s_iap_buf, n);
    }

    iap_flag_write(BRIDGE_OTA_FLAG_A, len, crc);   /* 置 A 后断电也安全（Stage 已可弃） */
    iap_erase(BRIDGE_OTA_STAGE_ADDR, copy_len);    /* 清暂存（启动一次性耗时） */
    return true;
}

/* ============================ App 有效性 / 跳转 ============================ */

bool iap_app_valid(void)
{
    uint32_t w[4];
    uint32_t i;
    bool all_ff = true;
    bool all_00 = true;
    uint32_t low2;

    FLASH_ROM_READ(BRIDGE_OTA_APP_ADDR, (uint8_t *)w, sizeof(w));
    for (i = 0u; i < 4u; i++) {
        if (w[i] != 0xFFFFFFFFu) {
            all_ff = false;
        }
        if (w[i] != 0x00000000u) {
            all_00 = false;
        }
    }
    if (all_ff || all_00) {
        return false;
    }

    low2 = w[0] & 0x3u;
    if (low2 == 0x3u && (w[0] & 0x7Fu) != 0x6Fu) {
        return false;   /* 32 位指令却非 JAL：肯定是坏镜像 */
    }
    return true;
}

void iap_jump_app(void)
{
    ((void (*)(void))((uint32_t *)BRIDGE_OTA_APP_ADDR))();
    for (;;) {
        __asm__ volatile("nop");    /* 理论不可达 */
    }
}
