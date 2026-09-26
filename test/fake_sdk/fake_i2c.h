/*
 * The fake I2C bus behind test/fake_sdk. A device answers only while both of
 * its lines are in the I2C function, which is how MK1B's two SDA pads share
 * one SCL. A transfer nobody answers is a NACK.
 */
#ifndef FAKE_I2C_H
#define FAKE_I2C_H

#include "fake_sdk.h"

typedef struct fake_i2c_dev {
    int bus; /* 0 or 1 */
    uint8_t addr;
    uint8_t sda, scl;
    /* Each returns the bytes moved, or PICO_ERROR_GENERIC for a NACK. */
    int (*write)(struct fake_i2c_dev *d, const uint8_t *src, size_t len);
    int (*read)(struct fake_i2c_dev *d, uint8_t *dst, size_t len);
    uint hz_last; /* the bus rate at its last answered transfer */
    int transfers;
} fake_i2c_dev_t;

extern uint fake_i2c_hz[2];

void fake_i2c_reset(void);
void fake_i2c_attach(fake_i2c_dev_t *d);

#endif /* FAKE_I2C_H */
