/*
 * SPDX-License-Identifier: MIT
 */
#include "i2c_recover.h"
#include "hardware/gpio.h"

#define CLOCK_EDGES 18u /* nine pulses */

static void release(uint8_t pin) {
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void i2c_recover_begin(i2c_recover_t *r, uint8_t scl, const uint8_t *sda, uint8_t n_sda) {
    r->scl = scl;
    r->n_sda = n_sda > I2C_RECOVER_MAX_SDA ? I2C_RECOVER_MAX_SDA : n_sda;
    for (uint8_t i = 0; i < r->n_sda; i++) {
        r->sda[i] = sda[i];
        gpio_init(sda[i]);
        release(sda[i]);
    }
    gpio_init(scl);
    gpio_put(scl, 1);
    gpio_set_dir(scl, GPIO_OUT);
    r->step = 0;
}

bool i2c_recover_step(i2c_recover_t *r) {
    uint8_t s = ++r->step;
    if (s <= CLOCK_EDGES) {
        gpio_put(r->scl, (s & 1u) == 0u);
        return false;
    }
    if (s == CLOCK_EDGES + 1u) {
        /* SDA low with SCL high, so that releasing it is a STOP. */
        for (uint8_t i = 0; i < r->n_sda; i++) {
            gpio_put(r->sda[i], 0);
            gpio_set_dir(r->sda[i], GPIO_OUT);
        }
        return false;
    }
    if (s == CLOCK_EDGES + 2u) {
        for (uint8_t i = 0; i < r->n_sda; i++)
            gpio_put(r->sda[i], 1);
        return false;
    }
    for (uint8_t i = 0; i < r->n_sda; i++)
        release(r->sda[i]);
    release(r->scl);
    r->step = CLOCK_EDGES + 3u;
    return true;
}
