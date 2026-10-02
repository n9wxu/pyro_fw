/*
 * MS5607-02BA03 detection and compensation; the one-shot is ms5607_oneshot.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_driver.h"
#include "board_pins.h"
#include "hardware/i2c.h"

#define I2C_PORT BOARD_I2C_INST

/* The address's LSB is CSB's complement (datasheet page 7). */
#define MS5607_ADDR_CSB_VDD 0x76
#define MS5607_ADDR_CSB_GND 0x77

#define MS5607_CMD_RESET 0x1E
#define MS5607_CMD_PROM_READ 0xA0 /* word n at 0xA0 + 2n */

/* Every transfer is bounded: the SDK's blocking calls wait forever on a part
 * holding SCL low, and detection also runs after a reset in flight. */
#define MS5607_DETECT_TIMEOUT_US 2000u

static uint8_t ms5607_addr = 0;
static uint16_t prom[8];

static bool ms5607_write_cmd(uint8_t cmd) {
    return i2c_write_timeout_us(I2C_PORT, ms5607_addr, &cmd, 1, false, MS5607_DETECT_TIMEOUT_US) == 1;
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

        if (i2c_write_timeout_us(I2C_PORT, ms5607_addr, &cmd, 1, true, MS5607_DETECT_TIMEOUT_US) != 1)
            return false;

        if (i2c_read_timeout_us(I2C_PORT, ms5607_addr, data, 2, false, MS5607_DETECT_TIMEOUT_US) != 2)
            return false;

        prom[i] = ((uint16_t)data[0] << 8) | data[1];
    }
    return true;
}

/* The CRC passes an all-zero PROM, which is what a blank one reads; no C1-C6
 * of a real part is 0 or 0xFFFF. */
static bool prom_valid(void) {
    if (!ms5607_prom_crc_ok(prom))
        return false;
    for (int i = 1; i <= 6; i++)
        if (prom[i] == 0 || prom[i] == 0xFFFFu)
            return false;
    return true;
}

void ms5607_detect_begin(ms5607_detect_t *d) {
    d->addr = MS5607_ADDR_CSB_VDD;
    d->tries = 0;
    d->reloading = false;
}

/* [DD-053] Reset, then the PROM a deadline later, at each address in turn. A
 * PROM read that fails its CRC is read again after another reset. */
ms5607_detect_result_t ms5607_detect_step(ms5607_detect_t *d, uint32_t now_ms) {
    while (d->addr <= MS5607_ADDR_CSB_GND) {
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
            bool read = ms5607_read_prom();
            if (read && prom_valid())
                return MS5607_DETECT_FOUND;
            if (read && ++d->tries < MS5607_PROM_TRIES)
                return MS5607_DETECT_PENDING;
        }
        d->tries = 0;
        d->addr++;
    }
    return MS5607_DETECT_ABSENT;
}
