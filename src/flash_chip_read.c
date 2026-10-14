/*
 * The flash chip's JEDEC ID (9Fh) and whole unique ID (4Bh), read once.
 * See flash_chip.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_chip.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <string.h>

#define CMD_JEDEC_ID 0x9Fu
#define CMD_UNIQUE_ID 0x4Bu
#define UNIQUE_ID_DUMMY 4 /* four dummy bytes before the ID, on every chip here */

/* Before the scheduler starts core1: flash_do_cmd() takes the chip out of
 * XIP, so nothing else may run from flash while it does. */
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
    restore_interrupts(irq);

    flash_chip_set(jedec, rx + 1 + UNIQUE_ID_DUMMY);
}
