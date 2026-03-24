/*
 * MS5607-02BA03 pressure sensor driver interface.
 *
 * Two APIs are provided:
 *
 * Synchronous (blocking) — used at boot/init time only:
 *   ms5607_detect()  — probe both I2C addresses, read PROM
 *   ms5607_read()    — full conversion with sleep_ms waits
 *
 * Phased (non-blocking) — used by the async pressure state machine:
 *   ms5607_start_d1()     — write D1 conversion command (~25µs I2C)
 *   ms5607_start_d2()     — write D2 conversion command (~25µs I2C)
 *   ms5607_read_raw()     — read ADC result after conversion (~75µs I2C)
 *   ms5607_compensate()   — compute Pa/°C from raw D1+D2 (~3µs math)
 *
 * Caller is responsible for the 10ms wait between start and read.
 * MS5607_CONV_MS gives the minimum wait for OSR 4096.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MS5607_DRIVER_H
#define MS5607_DRIVER_H

#include "pressure_sensor.h"
#include <stdbool.h>
#include <stdint.h>

/* Minimum conversion time (ms) at OSR 4096 — use in state machine */
#define MS5607_CONV_MS 10

/* ── Synchronous API (blocking, init time only) ─────────────────── */

bool ms5607_detect(void);
bool ms5607_read(pressure_reading_t *reading);

/* ── Phased API (non-blocking, for async state machine) ─────────── */

/* Start pressure (D1) ADC conversion. Returns false on I2C error. */
bool ms5607_start_d1(void);

/* Start temperature (D2) ADC conversion. Returns false on I2C error. */
bool ms5607_start_d2(void);

/* Read the last ADC result (call MS5607_CONV_MS after start).
 * Returns false on I2C error; *value is undefined on error. */
bool ms5607_read_raw(uint32_t *value);

/* Compute compensated pressure and temperature from raw D1/D2.
 * Always returns true (pure arithmetic, no I2C). */
bool ms5607_compensate(uint32_t d1, uint32_t d2, pressure_reading_t *out);

#endif /* MS5607_DRIVER_H */
