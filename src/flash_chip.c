/*
 * Flash chip identity. See flash_chip.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_chip.h"

#include <stdio.h>
#include <string.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico.h"
#define IN_RAM(f) __no_inline_not_in_flash_func(f)
#else
#define IN_RAM(f) f
#endif

/* 64 kB to 32 MB. Outside it the byte is not a size: 00 and FF are no chip,
 * and Adesto's AT25SF128A answers 1F 89 01. */
#define CAPACITY_LOG2_MIN 0x10u
#define CAPACITY_LOG2_MAX 0x19u

#define MAKER_XTX 0x0Bu  /* XT25F128F-W_Rev0.6_2022-02-26.pdf, p. 23: 128 bits */
#define MAKER_BYTE 0x68u /* BY25Q64ES_Rev2.9_2024-10-29.pdf, p. 45: 128 bits */
#define UID_LEN_SDK 8    /* Winbond's 64 bits (W25Q16JV p. 46), and the SDK's read */

static uint8_t jedec_id[3];
static uint8_t uid_bytes[FLASH_CHIP_UID_MAX];

uint32_t flash_chip_capacity(const uint8_t jedec[3]) {
    if (!jedec || jedec[2] < CAPACITY_LOG2_MIN || jedec[2] > CAPACITY_LOG2_MAX)
        return 0;
    return 1u << jedec[2];
}

int flash_chip_uid_len(const uint8_t jedec[3]) {
    if (jedec && (jedec[0] == MAKER_XTX || jedec[0] == MAKER_BYTE))
        return FLASH_CHIP_UID_MAX;
    return UID_LEN_SDK;
}

flash_fit_t flash_chip_fit(uint32_t chip_bytes, uint32_t image_bytes) {
    if (chip_bytes == 0)
        return FLASH_FIT_UNKNOWN;
    return chip_bytes < image_bytes ? FLASH_FIT_TOO_SMALL : FLASH_FIT_OK;
}

void flash_chip_set(const uint8_t jedec[3], const uint8_t uid[FLASH_CHIP_UID_MAX]) {
    memcpy(jedec_id, jedec, sizeof(jedec_id));
    memcpy(uid_bytes, uid, sizeof(uid_bytes));
}

uint32_t flash_chip_bytes(void) {
    return flash_chip_capacity(jedec_id);
}

void flash_chip_jedec(uint8_t out[3]) {
    memcpy(out, jedec_id, sizeof(jedec_id));
}

int flash_chip_uid(uint8_t out[FLASH_CHIP_UID_MAX]) {
    int n = flash_chip_uid_len(jedec_id);
    memcpy(out, uid_bytes, (size_t)n);
    return n;
}

static void hex(char *out, size_t cap, const uint8_t *b, int n) {
    if (cap == 0)
        return;
    out[0] = '\0';
    for (int i = 0; i < n && (size_t)(i * 2 + 2) < cap; i++)
        snprintf(out + i * 2, 3, "%02X", b[i]);
}

void flash_chip_jedec_hex(char *out, size_t cap) {
    hex(out, cap, jedec_id, (int)sizeof(jedec_id));
}

void flash_chip_uid_hex(char *out, size_t cap) {
    hex(out, cap, uid_bytes, flash_chip_uid_len(jedec_id));
}

static uint8_t IN_RAM(status)(const flash_chip_io_t *io, uint8_t cmd) {
    uint8_t buf[2] = {cmd, 0};
    io->cmd(io->ctx, buf, 2);
    return buf[1];
}

int IN_RAM(flash_chip_finish_suspended)(const flash_chip_io_t *io, uint32_t max_us) {
    if (!(status(io, FLASH_CMD_READ_STATUS2) & FLASH_STATUS2_SUSPENDED))
        return 0;
    uint8_t resume = FLASH_CMD_RESUME;
    io->cmd(io->ctx, &resume, 1);
    uint32_t t0 = io->now_us(io->ctx);
    while (status(io, FLASH_CMD_READ_STATUS) & FLASH_STATUS_BUSY) {
        if (io->now_us(io->ctx) - t0 > max_us)
            return -1;
    }
    return 1;
}
