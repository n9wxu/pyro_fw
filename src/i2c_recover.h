/*
 * Frees an I2C bus a CPU reset left mid-transfer: nine SCL clocks, then a
 * STOP on each SDA, one edge a step [DD-053]. A sensor stays powered across
 * the reset and can be holding SDA low; clocking it through the rest of its
 * byte lets go. Both lines are plain GPIOs throughout, and inputs with
 * pull-ups at the end, before the I2C peripheral takes them.
 *
 * I2C has no lowest clock rate, so a step a loop is as good as any.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef I2C_RECOVER_H
#define I2C_RECOVER_H

#include <stdbool.h>
#include <stdint.h>

#define I2C_RECOVER_MAX_SDA 2

typedef struct {
    uint8_t scl;
    uint8_t sda[I2C_RECOVER_MAX_SDA];
    uint8_t n_sda;
    uint8_t step;
} i2c_recover_t;

void i2c_recover_begin(i2c_recover_t *r, uint8_t scl, const uint8_t *sda, uint8_t n_sda);

/* True once the lines are free. */
bool i2c_recover_step(i2c_recover_t *r);

#endif /* I2C_RECOVER_H */
