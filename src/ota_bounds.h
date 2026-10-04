/*
 * The OTA image's bound [OTA-06]: the download slot, past which lies
 * littlefs. Checked against Content-Length before a byte is written, and
 * again for each sector, so no length the client claims reaches past it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OTA_BOUNDS_H
#define OTA_BOUNDS_H

#include <stdbool.h>
#include <stdint.h>

static inline bool ota_image_fits(uint32_t len, uint32_t slot_bytes) {
    return len > 0 && len <= slot_bytes;
}

static inline bool ota_sector_fits(uint32_t offset, uint32_t sector_bytes, uint32_t slot_bytes) {
    return offset <= slot_bytes && sector_bytes <= slot_bytes - offset;
}

#endif
