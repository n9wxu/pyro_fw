/*
 * Apogee from the raw readings alone [FLT-APO-09, FLT-APO-10, DD-109].
 *
 * The median of the last three readings' heights, against the highest such
 * median since launch: apogee is the median standing 2 m below that top for
 * 0.5 s, once the board has been 30 m above the pad. No model, no Mach
 * number.
 *
 * Beside it, the speed lockout: the speed the medians show, which must have
 * stayed under 50 m/s for the last 3 s before the verdict may stand in for
 * the model filter's.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SIMPLE_APOGEE_H
#define SIMPLE_APOGEE_H

#include <stdbool.h>
#include <stdint.h>

#define SIMPLE_MEDIAN_N 3
#define SIMPLE_SPEED_N 5 /* heights 100 ms apart: a speed over 0.4 to 0.5 s */

typedef struct {
    float raw_m[SIMPLE_MEDIAN_N];
    uint8_t raw_count, raw_next;

    float median_m;
    bool have_top;
    float top_m;
    uint32_t top_ms;
    bool high_enough;
    bool risen;
    uint32_t risen_since_ms;

    float height_m[SIMPLE_SPEED_N];
    uint32_t height_ms[SIMPLE_SPEED_N];
    uint8_t heights, height_next;
    bool have_speed;
    float speed_ms; /* up is positive */
    bool slow;
    uint32_t slow_since_ms;
    bool released;
} simple_apogee_t;

void simple_apogee_start(simple_apogee_t *f);
/* Fed the height above the pad of every reading the sensor is believed on. */
void simple_apogee_reading(simple_apogee_t *f, float raw_height_m, uint32_t t_ms);

/* The rise has been held. */
bool simple_apogee_met(const simple_apogee_t *f, uint32_t now_ms);
/* The speed has been under 50 m/s for the last 3 s. */
bool simple_apogee_released(const simple_apogee_t *f);

/* The top as the medians saw it, 0 before any, and the last time they stood
 * there. */
float simple_apogee_top_m(const simple_apogee_t *f);
uint32_t simple_apogee_top_ms(const simple_apogee_t *f);
float simple_apogee_height_m(const simple_apogee_t *f);
float simple_apogee_speed_ms(const simple_apogee_t *f);

#endif
