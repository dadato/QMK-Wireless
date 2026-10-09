/* IAP 共享原语（键盘 IAP 与 Dongle USB IAP 共用）
 *
 * 分区与 DataFlash 标志记录布局见 port/ch58x/bridge_ota_layout.h。
 * 本文件只依赖 WCH SDK（CH58x_common.h），不依赖工程 core/port 层，
 * 以便两个 IAP 镜像作为独立程序编译（make images）。
 */
#ifndef IAP_COMMON_H
#define IAP_COMMON_H

#include <stdint.h>
#include <stdbool.h>

/* ---- DataFlash 标志记录（BRIDGE_OTA_FLAG_REL，8B） ---- */
void iap_flag_read(uint8_t *flag, uint32_t *len, uint16_t *crc);
void iap_flag_write(uint8_t flag, uint32_t len, uint16_t crc);
/* 写记录（len/crc 置 0）→ 延时 → SYS_ResetExecute()；**不复返**。
 * 用于 App 侧「进 USB OTA」与 IAP 侧「放弃/超时回 App」。 */
void iap_flag_enter_reset(uint8_t flag);

/* ---- CRC-16/CCITT-FALSE（init 0xFFFF / poly 0x1021）增量 ---- */
uint16_t iap_crc16_update(uint16_t crc, const uint8_t *d, uint32_t len);
uint16_t iap_crc16(const uint8_t *d, uint32_t len);

/* ---- 4K 粒度擦除 [addr, addr+len) ---- */
void iap_erase(uint32_t addr, uint32_t len);

/* ---- 顺序写入 Stage：把 buf[0..len) 写到 Stage+off（按需擦 4K 页）----
 * *erased 为调用方持有的「已擦除字节数」游标（以 Stage 起点为基准），跨调用保持；
 * buf 必须 RAM 且 4 字节对齐（FLASH_ROM_WRITE 要求）。返回 0 成功，非 0 失败。 */
uint8_t iap_stage_write(uint32_t *erased, uint8_t *buf, uint32_t off, uint8_t len);

/* ---- 按记录长度把 Stage 暂存搬到 APP_A ----
 * 先复验暂存区 CRC（不符返回 false，**不碰 APP_A**）；成功：擦 A → 1KB 分块
 * 搬运 → 写 flag=A → 擦 Stage，返回 true。 */
bool iap_copy_stage_to_app(void);

/* ---- App 有效性护栏（跳转前最后一道，见 iap_main.c 历史事故注释） ---- */
bool iap_app_valid(void);

/* ---- 跳转 App（BRIDGE_OTA_APP_ADDR = 0x2000），**不复返** ---- */
void iap_jump_app(void);

#endif /* IAP_COMMON_H */
