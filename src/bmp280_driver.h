/*
 * BMP280 pressure sensor: forced conversions, one after another [DD-067,
 * DD-093], so every reading is a conversion of its own and its time is known.
 *
 * In flight the pressure collector owns the part (pressure_collector.h):
 * BMP280_PART is what it needs to know of it. The arithmetic on a cycle's
 * bytes is here, and runs in the sensor task.
 *
 * Figures are from docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BMP280_DRIVER_H
#define BMP280_DRIVER_H

#include "pressure_collector.h"
#include "pressure_sensor.h"
#include <stdbool.h>
#include <stdint.h>

/* [DD-052] The fastest the BMP280 allows the RP2040: fast mode. It lists
 * standard, fast and high-speed modes, not fast-mode plus (docs/datasheets/,
 * BMP280 pages 27 and 31), and the RP2040 has no high-speed mode (RP2040
 * section 4.3.2). A board runs at this or, where its PCB cannot, slower:
 * BOARD_BMP280_I2C_HZ. */
#define BMP280_I2C_MAX_HZ 400000u

/* From power-on or a soft reset to the first transfer (datasheet page 8). */
#define BMP280_STARTUP_MS 2u

/* x4 pressure and x1 temperature take 11.5 ms typical, 13.3 ms at most
 * (Table 13, page 18). A read issued earlier returns the conversion before
 * (page 25, "measuring"). */
#define BMP280_MEAS_MAX_US 13300u

/* [SNS-PRES-08] From the command to the middle of the pressure's own
 * measurement. Table 13's rows add 2 ms typical a pressure oversample, so
 * x4's pressure is the last 8 ms of the 11.5. */
#define BMP280_P_MID_US 7500u

#define BMP280_REG_RESET 0xE0u
#define BMP280_SOFT_RESET 0xB6u /* page 24 */
#define BMP280_REG_CTRL_MEAS 0xF4u
/* Temperature x1, pressure x4: asleep, and one forced conversion (page 25). */
#define BMP280_CTRL_MEAS_X1_X4_SLEEP 0x2Cu
#define BMP280_CTRL_MEAS_X1_X4_FORCED 0x2Du
/* Pressure then temperature, three bytes each, in one burst (page 20). */
#define BMP280_REG_DATA 0xF7u
#define BMP280_DATA_BYTES 6

static const collector_part_t BMP280_PART = {
    .steps = 1,
    .step = {{{BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_X1_X4_FORCED},
              2,
              BMP280_REG_DATA,
              BMP280_DATA_BYTES,
              BMP280_MEAS_MAX_US,
              BMP280_P_MID_US}},
    .reset = {BMP280_REG_RESET, BMP280_SOFT_RESET},
    .reset_len = 2,
    .reset_us = BMP280_STARTUP_MS * 1000u,
};

/* Probe the sensor and read its calibration, leaving it asleep. True if a
 * BMP280 answered and is configured. */
bool bmp280_detect(void);

/* The address bmp280_detect() found the sensor at. */
uint8_t bmp280_address(void);

typedef struct {
    pressure_reading_t reading; /* its time is the caller's to set */
    uint32_t adc_p, adc_t;      /* the raw codes, for the pressure trace */
} bmp280_reading_t;

/* Bosch's compensation of one cycle's bytes, with this part's calibration.
 * False: the calibration is blank. */
bool bmp280_compensate(const uint8_t data[BMP280_DATA_BYTES], bmp280_reading_t *r);

#endif /* BMP280_DRIVER_H */
