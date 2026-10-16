#include "bridge_ota.h"
#include "bridge_crc16.h"
#include "bridge_port.h"

static bool     s_active;
static uint32_t s_total;
static uint32_t s_written;
static uint16_t s_crc_exp;
static uint16_t s_crc_run;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int bridge_ota_begin(const uint8_t *payload, uint8_t len)
{
    uint32_t total;

    if (payload == 0 || len != 6u) {
        return BRIDGE_ERR_PARAM;
    }
    total = rd_u32(payload);
    /* 长度：非 0、不超暂存区、4 对齐（保证擦写对齐；发送方以 0xFF 补齐） */
    if (total == 0u || total > BRIDGE_FW_MAX_LEN || (total & 3u) != 0u) {
        return BRIDGE_ERR_PARAM;
    }

    if (s_active) {
        bridge_ota_abort();                 /* 重入：放弃上一次未完成传输 */
    }
    if (port_fw_begin(total) != BRIDGE_OK) {
        return BRIDGE_ERR_UNSUPPORTED;      /* Dongle 侧 / 端口层不可用 */
    }

    s_total   = total;
    s_written = 0u;
    s_crc_run = 0xFFFFu;
    s_crc_exp = (uint16_t)((uint16_t)payload[4] | ((uint16_t)payload[5] << 8));
    s_active  = true;
    return BRIDGE_OK;
}

int bridge_ota_data(const uint8_t *payload, uint8_t len)
{
    uint32_t off;
    uint8_t  dlen;
    uint16_t i;
    int      rc;

    if (!s_active) {
        return BRIDGE_ERR_STATE;
    }
    if (payload == 0 || len < 5u) {
        return BRIDGE_ERR_PARAM;
    }
    off  = rd_u32(payload);
    dlen = (uint8_t)(len - 4u);
    if (dlen == 0u || dlen > (uint8_t)BRIDGE_FW_CHUNK_MAX || (dlen & 3u) != 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (off + (uint32_t)dlen <= s_written) {
        /* 重复帧（上位机因 ACK 丢失而重传）：幂等直接确认，不重复累计 CRC */
        return BRIDGE_OK;
    }
    if (off != s_written) {
        return BRIDGE_ERR_PARAM;            /* 其余乱序一律拒绝 */
    }
    if ((uint32_t)dlen > (s_total - s_written)) {
        return BRIDGE_ERR_PARAM;            /* 超出声明总长 */
    }

    rc = port_fw_write(off, &payload[4], dlen);
    if (rc != BRIDGE_OK) {
        return rc;
    }

    for (i = 0u; i < dlen; i++) {
        s_crc_run = bridge_crc16_update(s_crc_run, payload[4u + i]);
    }
    s_written += dlen;
    return BRIDGE_OK;
}

int bridge_ota_end(const uint8_t *payload, uint8_t len)
{
    (void)payload;

    if (!s_active) {
        return BRIDGE_ERR_STATE;
    }
    if (len != 0u) {
        return BRIDGE_ERR_PARAM;
    }
    if (s_written != s_total) {
        return BRIDGE_ERR_STATE;            /* 数据不完整 */
    }
    if (s_crc_run != s_crc_exp) {
        return BRIDGE_ERR_CRC;
    }
    return BRIDGE_OK;
}

void bridge_ota_apply(void)
{
    /* 置 IAP 标志并复位；目标侧 SDK 实现不复返。测试桩返回后清理状态。 */
    port_fw_finish(s_crc_run);
    s_active = false;
}

void bridge_ota_abort(void)
{
    if (s_active) {
        port_fw_abort();
    }
    s_active = false;
}

bool bridge_ota_active(void)
{
    return s_active;
}
