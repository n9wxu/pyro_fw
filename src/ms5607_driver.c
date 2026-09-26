/*
 * MS5607-02BA03 pressure sensor driver
 * I2C address: 0x76 or 0x77
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_driver.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include <stdio.h>

#define I2C_PORT i2c1

#define MS5607_ADDR_CSB_LOW 0x76
#define MS5607_ADDR_CSB_HIGH 0x77

#define MS5607_CMD_RESET 0x1E
#define MS5607_CMD_CONV_D1 0x48 // Pressure conversion (OSR=4096)
#define MS5607_CMD_CONV_D2 0x58 // Temperature conversion (OSR=4096)
#define MS5607_CMD_ADC_READ 0x00
#define MS5607_CMD_PROM_READ 0xA0 // Base address for PROM

static uint8_t ms5607_addr = 0;
static uint16_t prom[8];

static bool ms5607_write_cmd(uint8_t cmd) {
    return i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, false) == 1;
}

static bool ms5607_read_adc_internal(uint32_t *value) {
    uint8_t cmd = MS5607_CMD_ADC_READ;
    uint8_t data[3];
    if (i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, true) != 1)
        return false;
    if (i2c_read_blocking(I2C_PORT, ms5607_addr, data, 3, false) != 3)
        return false;
    *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    return true;
}

static bool ms5607_read_raw(uint32_t *value) {
    return ms5607_read_adc_internal(value);
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

bool ms5607_detect(void) {
    // Try both possible addresses
    for (uint8_t addr = MS5607_ADDR_CSB_LOW; addr <= MS5607_ADDR_CSB_HIGH; addr++) {
        ms5607_addr = addr;

        // Reset
        if (!ms5607_write_cmd(MS5607_CMD_RESET))
            continue;

        sleep_ms(10);

        // Read PROM
        if (ms5607_read_prom()) {
            // Verify PROM CRC (simplified check - just verify non-zero)
            if (prom[0] != 0 && prom[0] != 0xFFFF)
                return true;
        }
    }

    return false;
}

/* Synchronous (blocking) read — used at init/detect time only. */
bool ms5607_read(pressure_reading_t *reading) {
    uint32_t d1, d2;
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D1))
        return false;
    uint64_t began = time_us_64();
    sleep_ms(MS5607_CONV_MS);
    if (!ms5607_read_raw(&d1))
        return false;
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D2))
        return false;
    sleep_ms(MS5607_CONV_MS);
    if (!ms5607_read_raw(&d2))
        return false;
    reading->time_us = began + MS5607_HALF_CONV_US;
    return ms5607_compensate(d1, d2, reading);
}
