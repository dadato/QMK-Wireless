#include "bridge_crc16.h"

uint16_t bridge_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t bridge_crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    if (data == 0) {
        return crc;
    }
    for (uint16_t i = 0; i < len; i++) {
        crc = bridge_crc16_update(crc, data[i]);
    }
    return crc;
}
