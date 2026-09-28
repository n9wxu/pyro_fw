/*
 * BMP280 pressure sensor, at the loop's rate [DD-067]: the loop commands one
 * forced conversion each iteration and takes it at the next, so every reading
 * is a conversion of its own and its time is known. The part sleeps between.
 *
 * Figures are from docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BMP280_DRIVER_H
#define BMP280_DRIVER_H

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
 * (Table 13, page 18): each conversion is done before the next loop. */
#define BMP280_MEAS_MAX_US 13300u

/* [SNS-PRES-08] From the command to the middle of the pressure's own
 * measurement. Table 13's rows add 2 ms typical a pressure oversample, so
 * x4's pressure is the last 8 ms of the 11.5. */
#define BMP280_P_MID_US 7500u

/* Probe the sensor and read its calibration, leaving it asleep with the
 * loop's settings. True if a BMP280 answered and is configured. */
bool bmp280_detect(void);

/* BUSY: the last conversion is still running, so nothing was taken or
 * commanded. BUS: the part did not answer. */
typedef enum { BMP280_STARTED, BMP280_BUSY, BMP280_BUS } bmp280_start_t;

typedef struct {
    pressure_reading_t reading;
    uint32_t adc_p, adc_t; /* the raw codes, for the pressure trace */
    bool flashed;          /* [DD-068] a flash erase or program ran during it */
} bmp280_reading_t;

/* Once a loop: takes the conversion the last call commanded into *r, then
 * commands the next. False when nothing was taken. */
bool bmp280_cycle(bmp280_reading_t *r, bmp280_start_t *started);

#endif /* BMP280_DRIVER_H */
