/*
 * What the flash chip says of itself, read once at boot (hal_common's
 * flash_chip_read.c): its JEDEC ID, whose third byte is the log2 of its
 * size, and its whole unique ID.
 *
 * The SDK reads 8 bytes of the unique ID. XTX's and BYTe's are 16, and the
 * 8 the SDK keeps are the half two chips can share: both MK1Cs read
 * 41503459373331FF (DD-098).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_CHIP_H
#define FLASH_CHIP_H

#include <stddef.h>
#include <stdint.h>

#define FLASH_CHIP_UID_MAX 16

typedef enum {
    FLASH_FIT_UNKNOWN = 0, /* the chip does not say its size: no evidence */
    FLASH_FIT_OK,
    FLASH_FIT_TOO_SMALL, /* the image's flash map runs past the chip's end */
} flash_fit_t;

/* Bytes, or 0 when the third byte is not a size. */
uint32_t flash_chip_capacity(const uint8_t jedec[3]);

/* The unique ID's length in bytes, by the maker's datasheet; the SDK's 8
 * for a maker not known. */
int flash_chip_uid_len(const uint8_t jedec[3]);

flash_fit_t flash_chip_fit(uint32_t chip_bytes, uint32_t image_bytes);

/* The boot read's result. uid holds FLASH_CHIP_UID_MAX bytes, of which the
 * maker's length are kept. */
void flash_chip_set(const uint8_t jedec[3], const uint8_t uid[FLASH_CHIP_UID_MAX]);

uint32_t flash_chip_bytes(void);

/* The unique ID's bytes, at the maker's length, which is returned. */
int flash_chip_uid(uint8_t out[FLASH_CHIP_UID_MAX]);

/* The hardware read, on the target: once, before the scheduler. */
void flash_chip_read(void);

/* Upper-case hex, NUL-terminated: 6 digits for the JEDEC ID, 16 or 32 for
 * the unique ID. */
void flash_chip_jedec_hex(char *out, size_t cap);
void flash_chip_uid_hex(char *out, size_t cap);

#endif
