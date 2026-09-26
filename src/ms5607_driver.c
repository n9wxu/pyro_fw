/*
 * MS5607-02BA03 pressure sensor driver
 * I2C address: 0x76 or 0x77
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_driver.h"
#include "hardware/i2c.h"

#define I2C_PORT i2c1

#define MS5607_ADDR_CSB_LOW 0x76
#define MS5607_ADDR_CSB_HIGH 0x77

#define MS5607_CMD_RESET 0x1E
#define MS5607_CMD_PROM_READ 0xA0 // Base address for PROM

static uint8_t ms5607_addr = 0;
static uint16_t prom[8];

static bool ms5607_write_cmd(uint8_t cmd) {
    return i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, false) == 1;
}

bool ms5607_compensate(uint32_t d1, uint32_t d2, pressure_reading_t *out) {
    ms5607_compensate_prom(prom, d1, d2, out);
    return true;
}

uint8_t ms5607_address(void) {
    return ms5607_addr;
}

static bool ms5607_read_prom(void) {
    for (int i = 0; i < 8; i++) {
        uint8_t cmd = MS5607_CMD_PROM_READ + (i * 2);
        uint8_t data[2];

        if (i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, true) != 1)
            return false;

        if (i2c_read_blocking(I2C_PORT, ms5607_addr, data, 2, false) != 2)
            return false;

        prom[i] = ((uint16_t)data[0] << 8) | data[1];
    }
    return true;
}

void ms5607_detect_begin(ms5607_detect_t *d) {
    d->addr = MS5607_ADDR_CSB_LOW;
    d->reloading = false;
}

/* [DD-053] Reset, then the PROM a deadline later, at each address in turn. */
ms5607_detect_result_t ms5607_detect_step(ms5607_detect_t *d, uint32_t now_ms) {
    while (d->addr <= MS5607_ADDR_CSB_HIGH) {
        ms5607_addr = d->addr;
        if (!d->reloading) {
            if (ms5607_write_cmd(MS5607_CMD_RESET)) {
                d->reloading = true;
                d->due_ms = now_ms + MS5607_RESET_MS;
                return MS5607_DETECT_PENDING;
            }
        } else {
            if ((int32_t)(now_ms - d->due_ms) < 0)
                return MS5607_DETECT_PENDING;
            d->reloading = false;
            /* A blank PROM reads all zeros or all ones. */
            if (ms5607_read_prom() && prom[0] != 0 && prom[0] != 0xFFFF)
                return MS5607_DETECT_FOUND;
        }
        d->addr++;
    }
    return MS5607_DETECT_ABSENT;
}
