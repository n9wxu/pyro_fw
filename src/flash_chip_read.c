/*
 * The flash chip's JEDEC ID (9Fh) and whole unique ID (4Bh), read once.
 * See flash_chip.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_chip.h"
#include "flash_timing.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/timer.h"

#include <string.h>

#define CMD_JEDEC_ID 0x9Fu
#define CMD_UNIQUE_ID 0x4Bu
#define UNIQUE_ID_DUMMY 4 /* four dummy bytes before the ID, on every chip here */

static void __no_inline_not_in_flash_func(chip_do_cmd)(void *ctx, uint8_t *buf, size_t n) {
    (void)ctx;
    flash_do_cmd(buf, buf, n);
}

static uint32_t __no_inline_not_in_flash_func(chip_now)(void *ctx) {
    (void)ctx;
    return timer_hw->timerawl;
}

/* Before the scheduler starts core1: flash_do_cmd() takes the chip out of
 * XIP, so nothing else may run from flash while it does. An operation a
 * reset left suspended is run to its end before anything writes [FLT-LOG-11]. */
void flash_chip_read(void) {
    uint8_t tx[1 + UNIQUE_ID_DUMMY + FLASH_CHIP_UID_MAX];
    uint8_t rx[sizeof(tx)];
    uint8_t jedec[3];

    uint32_t irq = save_and_disable_interrupts();
    memset(tx, 0, sizeof(tx));
    tx[0] = CMD_JEDEC_ID;
    flash_do_cmd(tx, rx, 1 + sizeof(jedec));
    memcpy(jedec, rx + 1, sizeof(jedec));
    tx[0] = CMD_UNIQUE_ID;
    flash_do_cmd(tx, rx, sizeof(tx));
    if (flash_timing_for(jedec)) {
        flash_chip_io_t io = {NULL, chip_do_cmd, chip_now};
        flash_chip_finish_suspended(&io, FLASH_CHIP_FINISH_MAX_US);
    }
    restore_interrupts(irq);

    flash_chip_set(jedec, rx + 1 + UNIQUE_ID_DUMMY);
}
