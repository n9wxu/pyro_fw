/*
 * CRC-16/XMODEM: x^16+x^12+x^5+1, initial 0 -- the SD card's data CRC (SD
 * Physical Layer Simplified 6.00, PDF page 230), and the high-rate log's
 * record check.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

uint16_t crc16_update(uint16_t crc, const uint8_t *d, uint32_t n);

static inline uint16_t crc16(const uint8_t *d, uint32_t n) {
    return crc16_update(0, d, n);
}

#endif
