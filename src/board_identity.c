/*
 * Board identity. See board_identity.h for why this exists and why the MAC
 * lives in its own file rather than in config.ini.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_identity.h"
#include "hal.h"
#include "mac_random.h"
#include "pico/unique_id.h"
#include "hardware/adc.h"
#include "hardware/structs/rosc.h"
#include "hardware/timer.h"
#include <stdio.h>
#include <string.h>

#define SERIAL_PATH "serial.txt"
#define MAC_LEN MAC_BYTES

/* 2048 ADC conversions at 2 us each, with a ring-oscillator bit beside each:
 * about 5 ms, once, on the boot that draws the MAC. */
#define ENTROPY_SAMPLES 2048

static uint8_t mac[MAC_LEN];
static char serial[BOARD_SERIAL_MAX];
static char hw_id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
static bool drawn;   /* from the RNG, not assigned */
static bool unsaved; /* drawn this boot, not yet in /serial.txt */

/* The temperature sensor's conversions, whose low bits are noise, and the
 * ring oscillator's random bit, which is random only while the system runs
 * from the crystal -- true once the SDK's clocks are up. adc_init() here is
 * repeated by the board's pyro_init(), which owns the ADC afterwards. */
static void pool_hardware(mac_pool_t *p) {
    bool rosc = (rosc_hw->status & ROSC_STATUS_ENABLED_BITS) != 0u;
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    for (int i = 0; i < ENTROPY_SAMPLES; i++) {
        uint32_t s = adc_read();
        if (rosc)
            s |= (rosc_hw->randombit & 1u) << 12;
        mac_pool_add(p, s ^ (time_us_32() << 13));
    }
    adc_set_temp_sensor_enabled(false);
}

void board_identity_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t id64 = 0;
    for (unsigned i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        snprintf(hw_id + i * 2, 3, "%02X", id.id[i]);
        id64 = (id64 << 8) | id.id[i];
    }

    char buf[MAC_FILE_MAX + 8];
    int n = hal_fs_read_file(SERIAL_PATH, buf, (int)sizeof(buf) - 1);
    bool have = false;
    if (n > 0) {
        buf[n] = '\0';
        /* All or nothing: a half-applied identity is how two boards end up
         * on one subnet. */
        have = mac_file_parse(buf, mac, &drawn);
    }
    if (!have) {
        /* The flash id seeds the pool only so two boards whose sources both
         * fail still differ where their flash ids do; MK1C's do not. */
        mac_pool_t pool;
        mac_pool_init(&pool, id64 ^ time_us_64());
        pool_hardware(&pool);
        mac_pool_draw(&pool, mac);
        drawn = true;
        unsaved = true;
    }

    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool board_identity_unsaved(void) {
    return unsaved;
}

void board_identity_save(void) {
    if (!unsaved)
        return;
    char buf[MAC_FILE_MAX];
    int n = mac_file_format(mac, true, buf, (int)sizeof(buf));
    if (n > 0 && hal_fs_write_file(SERIAL_PATH, buf, n) == 0)
        unsaved = false;
}

const char *board_mac_source(void) {
    return drawn ? "rng" : "assigned";
}

const char *board_serial(void) {
    return serial;
}

const uint8_t *board_mac(void) {
    return mac;
}

bool board_serial_assigned(void) {
    return !drawn;
}

const char *board_hw_id(void) {
    return hw_id;
}

uint8_t board_subnet_octet(void) {
    return mac[MAC_LEN - 1];
}
