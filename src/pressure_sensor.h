/*
 * Unified pressure sensor interface, implemented per board in
 * boards/<name>/pressure_board.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_SENSOR_H
#define PRESSURE_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PRESSURE_SENSOR_NONE = 0,
    PRESSURE_SENSOR_MS5607,
    PRESSURE_SENSOR_BMP280,
    PRESSURE_SENSOR_PENDING, /* still being brought up */
} pressure_sensor_type_t;

typedef struct {
    float pressure_pa;
    float temperature_c;
    /* [SNS-PRES-08] When the pressure was measured, from the hardware timer,
     * set by the driver. The temperature may be older: the sensor's thermal
     * mass keeps it slow. */
    uint64_t time_us;
} pressure_reading_t;

/* [DD-053] Bringing the sensor up waits on nothing. begin() starts it; step(),
 * once a loop, takes it through bus recovery, settles and the sensor's reset,
 * each a deadline a later loop meets, and returns PENDING until it knows
 * which sensor answered, if any. */
void pressure_sensor_begin(void);
pressure_sensor_type_t pressure_sensor_step(uint32_t now_ms);

const char *pressure_sensor_name(void);

#endif
