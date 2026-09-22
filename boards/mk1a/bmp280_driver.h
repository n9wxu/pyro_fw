/*
 * BMP280 pressure sensor driver interface.
 *
 * The BMP280 is configured in normal mode (continuous conversion).
 * Results are always available in output registers — no phased API
 * is needed.  The async pressure state machine just calls bmp280_read()
 * periodically.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BMP280_DRIVER_H
#define BMP280_DRIVER_H

#include "pressure_sensor.h"
#include <stdbool.h>

/* Probe the sensor, read calibration, start normal-mode conversions.
 * Returns true if a BMP280 is detected and configured. */
bool bmp280_detect(void);

/* Read the current output registers and compute Pa/°C.
 * Non-blocking in normal mode — reads from continuously-updated regs.
 * Returns false on I2C error. */
bool bmp280_read(pressure_reading_t *reading);

#endif /* BMP280_DRIVER_H */
