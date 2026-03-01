/*
 * Unified pressure sensor interface with auto-detection
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "pressure_sensor.h"
#include "hardware/i2c.h"
#include <stdio.h>

// External driver functions
extern bool ms5607_detect(void);
extern bool ms5607_read(pressure_reading_t *reading);
extern bool bmp280_detect(void);
extern bool bmp280_read(pressure_reading_t *reading);

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

pressure_sensor_type_t pressure_sensor_init(void) {
    // Try MS5607 first
    if (ms5607_detect()) {
        detected_sensor = PRESSURE_SENSOR_MS5607;
        printf("Detected pressure sensor: MS5607-02BA03\n");
        return detected_sensor;
    }
    
    // Try BMP280
    if (bmp280_detect()) {
        detected_sensor = PRESSURE_SENSOR_BMP280;
        printf("Detected pressure sensor: BMP280\n");
        return detected_sensor;
    }
    
    printf("No pressure sensor detected\n");
    return PRESSURE_SENSOR_NONE;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    switch (detected_sensor) {
        case PRESSURE_SENSOR_MS5607:
            return ms5607_read(reading);
        
        case PRESSURE_SENSOR_BMP280:
            return bmp280_read(reading);
        
        default:
            return false;
    }
}

const char* pressure_sensor_name(void) {
    switch (detected_sensor) {
        case PRESSURE_SENSOR_MS5607:
            return "MS5607-02BA03";
        
        case PRESSURE_SENSOR_BMP280:
            return "BMP280";
        
        default:
            return "None";
    }
}
