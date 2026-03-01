/*
 * MS5607-02BA03 pressure sensor driver
 * I2C address: 0x76 or 0x77
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "pressure_sensor.h"
#include "hardware/i2c.h"
#include <stdio.h>

#define MS5607_ADDR_CSB_LOW   0x76
#define MS5607_ADDR_CSB_HIGH  0x77

#define MS5607_CMD_RESET      0x1E
#define MS5607_CMD_CONV_D1    0x48  // Pressure conversion (OSR=4096)
#define MS5607_CMD_CONV_D2    0x58  // Temperature conversion (OSR=4096)
#define MS5607_CMD_ADC_READ   0x00
#define MS5607_CMD_PROM_READ  0xA0  // Base address for PROM

static uint8_t ms5607_addr = 0;
static uint16_t prom[8];

static bool ms5607_write_cmd(uint8_t cmd) {
    return i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, false) == 1;
}

static bool ms5607_read_adc(uint32_t *value) {
    uint8_t cmd = MS5607_CMD_ADC_READ;
    uint8_t data[3];
    
    if (i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, true) != 1)
        return false;
    
    if (i2c_read_blocking(I2C_PORT, ms5607_addr, data, 3, false) != 3)
        return false;
    
    *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    return true;
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

bool ms5607_read(pressure_reading_t *reading) {
    uint32_t d1, d2;
    
    // Read pressure
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D1))
        return false;
    sleep_ms(10);  // Wait for conversion
    if (!ms5607_read_adc(&d1))
        return false;
    
    // Read temperature
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D2))
        return false;
    sleep_ms(10);
    if (!ms5607_read_adc(&d2))
        return false;
    
    // Calculate temperature
    int32_t dT = d2 - ((uint32_t)prom[5] << 8);
    int32_t temp = 2000 + (((int64_t)dT * prom[6]) >> 23);
    
    // Calculate pressure
    int64_t off = ((int64_t)prom[2] << 17) + (((int64_t)prom[4] * dT) >> 6);
    int64_t sens = ((int64_t)prom[1] << 16) + (((int64_t)prom[3] * dT) >> 7);
    int32_t p = (((d1 * sens) >> 21) - off) >> 15;
    
    reading->temperature_c = temp / 100.0f;
    reading->pressure_pa = p;
    
    return true;
}
