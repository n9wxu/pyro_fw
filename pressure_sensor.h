/*
 * Unified pressure sensor interface for Pyro MK1B
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef PRESSURE_SENSOR_H
#define PRESSURE_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PRESSURE_SENSOR_NONE = 0,
    PRESSURE_SENSOR_MS5607,
    PRESSURE_SENSOR_BMP280
} pressure_sensor_type_t;

typedef struct {
    float pressure_pa;
    float temperature_c;
} pressure_reading_t;

// Initialize and detect sensor
pressure_sensor_type_t pressure_sensor_init(void);

// Read pressure and temperature
bool pressure_sensor_read(pressure_reading_t *reading);

// Get sensor name string
const char* pressure_sensor_name(void);

#endif
