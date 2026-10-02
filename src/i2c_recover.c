/*
 * Bus clear, one edge a step [DD-053]. See i2c_recover.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "i2c_recover.h"
#include "hardware/gpio.h"

/* UM10204 Rev.7 §3.1.16 "Bus clear": nine clocks free a target holding SDA
 * low. The STOP after them leaves the bus idle for the first START. */
#define RECOVERY_SCL_PULSES 9u
#define CLOCK_EDGES (2u * RECOVERY_SCL_PULSES)
#define STEP_SDA_LOW (CLOCK_EDGES + 1u)
#define STEP_STOP (CLOCK_EDGES + 2u)
#define STEP_DONE (CLOCK_EDGES + 3u)

/* Open drain (UM10204 §3.1.1): a line is pulled low or let go to its
 * pull-up, never driven high, so nothing fights a target holding it low. */
static void od_low(uint8_t pin) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
}

static void od_release(uint8_t pin) {
    gpio_set_dir(pin, GPIO_IN);
}

static void od_init(uint8_t pin) {
    gpio_init(pin);
    gpio_pull_up(pin);
    od_release(pin);
}

void i2c_recover_begin(i2c_recover_t *r, uint8_t scl, const uint8_t *sda, uint8_t n_sda) {
    r->scl = scl;
    r->n_sda = n_sda > I2C_RECOVER_MAX_SDA ? I2C_RECOVER_MAX_SDA : n_sda;
    for (uint8_t i = 0; i < r->n_sda; i++) {
        r->sda[i] = sda[i];
        od_init(sda[i]);
    }
    od_init(scl);
    r->step = 0;
}

bool i2c_recover_step(i2c_recover_t *r) {
    uint8_t s = ++r->step;
    if (s <= CLOCK_EDGES) {
        if (s & 1u)
            od_low(r->scl);
        else
            od_release(r->scl);
        return false;
    }
    if (s == STEP_SDA_LOW) {
        /* With SCL high, so that letting SDA go is a STOP. */
        for (uint8_t i = 0; i < r->n_sda; i++)
            od_low(r->sda[i]);
        return false;
    }
    if (s == STEP_STOP) {
        for (uint8_t i = 0; i < r->n_sda; i++)
            od_release(r->sda[i]);
        return false;
    }
    r->step = STEP_DONE;
    return true;
}
