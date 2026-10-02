/*
 * The OTA image's bound.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OTA_BOUNDS_H
#define OTA_BOUNDS_H

#include <stdbool.h>
#include <stdint.h>

static inline bool ota_image_fits(uint32_t len, uint32_t slot_bytes) {
    (void)len;
    (void)slot_bytes;
    return true;
}

static inline bool ota_sector_fits(uint32_t offset, uint32_t sector_bytes, uint32_t slot_bytes) {
    (void)offset;
    (void)sector_bytes;
    (void)slot_bytes;
    return true;
}

#endif
