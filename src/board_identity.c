/*
 * Board identity. See board_identity.h for why this exists and why the MAC
 * lives in its own file rather than in config.ini.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_identity.h"
#include "flash_chip.h"
#include "hal.h"
#include "mac_random.h"
#include "pico/unique_id.h"
#include <stdio.h>
#include <string.h>

#define SERIAL_PATH "serial.txt"
#define MAC_LEN MAC_BYTES

static uint8_t mac[MAC_LEN];
static char serial[BOARD_SERIAL_MAX];
static char hw_id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
static bool assigned; /* through /api/serial, not derived */

void board_identity_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (unsigned i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++)
        snprintf(hw_id + i * 2, 3, "%02X", id.id[i]);

    char buf[MAC_FILE_MAX + 8];
    int n = hal_fs_read_file(SERIAL_PATH, buf, (int)sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
    uint8_t uid[FLASH_CHIP_UID_MAX];
    int len = flash_chip_uid(uid);
    assigned = mac_choose(n > 0 ? buf : NULL, uid, len, mac);

    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *board_mac_source(void) {
    return assigned ? "assigned" : "flash";
}

const char *board_serial(void) {
    return serial;
}

const uint8_t *board_mac(void) {
    return mac;
}

bool board_serial_assigned(void) {
    return assigned;
}

const char *board_hw_id(void) {
    return hw_id;
}

uint8_t board_subnet_octet(void) {
    return mac[MAC_LEN - 1];
}
