/* CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF, 不反转, 无 XOR 输出
 * 校验向量："123456789" -> 0x29B1
 */
#ifndef BRIDGE_CRC16_H
#define BRIDGE_CRC16_H

#include <stdint.h>

uint16_t bridge_crc16_ccitt(const uint8_t *data, uint16_t len);
uint16_t bridge_crc16_update(uint16_t crc, uint8_t byte);

#endif /* BRIDGE_CRC16_H */
