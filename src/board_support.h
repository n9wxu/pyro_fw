/*
 * What every board package's pyro and sensor code uses: a deadline the loop
 * checks instead of a wait [DD-053], a filtered ADC read, and outputs driven
 * to their safe level.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_SUPPORT_H
#define BOARD_SUPPORT_H

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool deadline_reached(uint32_t now_ms, uint32_t due_ms) {
    return (int32_t)(now_ms - due_ms) >= 0;
}

static inline uint16_t adc_read_raw(uint8_t channel) {
    adc_select_input(channel);
    return (uint16_t)adc_read();
}

/* The sense taps are RC filtered; the median drops a conversion that lands
 * on a switching edge elsewhere on the board. */
static inline uint16_t adc_read_median3(uint8_t channel) {
    uint16_t x = adc_read_raw(channel), y = adc_read_raw(channel), z = adc_read_raw(channel);
    if (x > y) {
        uint16_t t = x;
        x = y;
        y = t;
    }
    if (y > z)
        y = (x > z) ? x : z;
    return y;
}

/* The level is written before the direction, so no pad glitches high. */
static inline void drive_low(const uint8_t *pins, size_t n) {
    for (size_t i = 0; i < n; i++) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 0);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }
}

#define DRIVE_ALL_LOW(pins) drive_low((pins), sizeof(pins) / sizeof((pins)[0]))

#endif
