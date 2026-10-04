/*
 * BMP280: detection on the board's sensor bus (BOARD_I2C_INST), and the
 * compensation. Built into every image and empty unless the board fits one
 * (BOARD_HAS_BMP280).
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"
#if BOARD_HAS_BMP280
#include "bmp280_driver.h"
#include "hardware/i2c.h"
#include <stdio.h>

#define I2C_PORT BOARD_I2C_INST

#define BMP280_ADDR_SDO_LOW 0x76
#define BMP280_ADDR_SDO_HIGH 0x77

#define BMP280_REG_ID 0xD0
#define BMP280_REG_CONFIG 0xF5
#define BMP280_REG_CALIB 0x88

#define BMP280_CHIP_ID 0x58
#define BMP280_CONFIG_NO_FILTER 0x00 /* also what a reset leaves */

static uint8_t bmp280_addr = 0;
static uint16_t dig_T1;
static int16_t dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static int32_t t_fine;

/* Every transfer is bounded: the SDK's blocking calls wait forever on a part
 * holding SCL low, and the loop reads this part in flight. The bound is twice
 * the transfer's own time at the board's rate, and a millisecond: MK1B runs
 * this part at 100 kHz, where the calibration's 24 bytes take 2.3 ms. */
static uint bus_timeout_us(size_t len) {
    return (uint)((len + 1u) * 9u * 1000000u / BOARD_BMP280_I2C_HZ * 2u + 1000u);
}

static bool bmp280_write_reg(uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, bmp280_addr, data, 2, false, bus_timeout_us(2)) == 2;
}

static bool bmp280_read_reg(uint8_t reg, uint8_t *data, size_t len) {
    if (i2c_write_timeout_us(I2C_PORT, bmp280_addr, &reg, 1, true, bus_timeout_us(1)) != 1)
        return false;
    return i2c_read_timeout_us(I2C_PORT, bmp280_addr, data, len, false, bus_timeout_us(len)) == (int)len;
}

static bool bmp280_read_calibration(void) {
    uint8_t calib[24];

    if (!bmp280_read_reg(BMP280_REG_CALIB, calib, 24))
        return false;

    dig_T1 = (calib[1] << 8) | calib[0];
    dig_T2 = (calib[3] << 8) | calib[2];
    dig_T3 = (calib[5] << 8) | calib[4];
    dig_P1 = (calib[7] << 8) | calib[6];
    dig_P2 = (calib[9] << 8) | calib[8];
    dig_P3 = (calib[11] << 8) | calib[10];
    dig_P4 = (calib[13] << 8) | calib[12];
    dig_P5 = (calib[15] << 8) | calib[14];
    dig_P6 = (calib[17] << 8) | calib[16];
    dig_P7 = (calib[19] << 8) | calib[18];
    dig_P8 = (calib[21] << 8) | calib[20];
    dig_P9 = (calib[23] << 8) | calib[22];

    return true;
}

uint8_t bmp280_address(void) {
    return bmp280_addr;
}

extern void hal_telemetry_send(const char *sentence);

bool bmp280_detect(void) {
    char buf[64];
    for (uint8_t addr = BMP280_ADDR_SDO_LOW; addr <= BMP280_ADDR_SDO_HIGH; addr++) {
        bmp280_addr = addr;
        snprintf(buf, sizeof(buf), "!BMP280 try addr=0x%02X\r\n", addr);
        hal_telemetry_send(buf);

        uint8_t chip_id;
        if (bmp280_read_reg(BMP280_REG_ID, &chip_id, 1)) {
            snprintf(buf, sizeof(buf), "!BMP280 id=0x%02X (expect 0x58)\r\n", chip_id);
            hal_telemetry_send(buf);

            if (chip_id == BMP280_CHIP_ID) {
                hal_telemetry_send("!BMP280 reading calibration\r\n");
                if (bmp280_read_calibration()) {
                    bmp280_write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_X1_X4_SLEEP);
                    bmp280_write_reg(BMP280_REG_CONFIG, BMP280_CONFIG_NO_FILTER);
                    hal_telemetry_send("!BMP280 detect OK\r\n");
                    return true;
                } else {
                    hal_telemetry_send("!BMP280 calib fail\r\n");
                }
            }
        } else {
            snprintf(buf, sizeof(buf), "!BMP280 no response at 0x%02X\r\n", addr);
            hal_telemetry_send(buf);
        }
    }

    hal_telemetry_send("!BMP280 detect FAIL\r\n");
    return false;
}

/* Bosch's 64-bit integer compensation, datasheet section 3.11.3, page 21. */
bool bmp280_compensate(const uint8_t data[BMP280_DATA_BYTES], bmp280_reading_t *r) {
    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    r->adc_p = (uint32_t)adc_P;
    r->adc_t = (uint32_t)adc_T;

    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 =
        (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    int32_t T = (t_fine * 5 + 128) >> 8;

    int64_t var1_64 = ((int64_t)t_fine) - 128000;
    int64_t var2_64 = var1_64 * var1_64 * (int64_t)dig_P6;
    var2_64 = var2_64 + ((var1_64 * (int64_t)dig_P5) << 17);
    var2_64 = var2_64 + (((int64_t)dig_P4) << 35);
    var1_64 = ((var1_64 * var1_64 * (int64_t)dig_P3) >> 8) + ((var1_64 * (int64_t)dig_P2) << 12);
    var1_64 = (((((int64_t)1) << 47) + var1_64)) * ((int64_t)dig_P1) >> 33;

    if (var1_64 == 0)
        return false;

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2_64) * 3125) / var1_64;
    var1_64 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2_64 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1_64 + var2_64) >> 8) + (((int64_t)dig_P7) << 4);

    r->reading.temperature_c = T / 100.0f;
    r->reading.pressure_pa = p / 256.0f;
    return true;
}

#endif /* BOARD_HAS_BMP280 */
