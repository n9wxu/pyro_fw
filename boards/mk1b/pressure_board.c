/*
 * Pressure sensor interface — Pyro MK1B.
 *
 * Two SDA pads share SCL (GPIO7) on i2c1: a BMP280 on GPIO6 and an MS5607 on
 * GPIO10, and a board carries one or the other. The BMP280 pad is probed
 * first, in standard mode because its SDA has no pull-up but the RP2040's
 * own (DD-052); then the MS5607's, at its fastest.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "bmp280_driver.h"
#include "i2c_recover.h"
#include "ms5607_driver.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"

#include "board_pins.h"

_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");
_Static_assert(BOARD_BMP280_I2C_HZ <= BMP280_I2C_MAX_HZ, "faster than the BMP280 allows");

#define I2C_SCL_PIN BOARD_PIN_I2C_SCL
#define BMP280_SDA BOARD_PIN_BMP280_SDA
#define MS5607_SDA BOARD_PIN_MS5607_SDA

/* The pull-ups settle before the first transfer. */
#define PULLUP_SETTLE_MS 10u

extern void hal_telemetry_send(const char *sentence);

/* [DD-053] Each state is a step a loop takes; nothing here waits. */
typedef enum {
    BU_RECOVER,
    BU_BMP280_SETTLE, /* pads configured; then the soft reset */
    BU_BMP280_START,  /* waiting out its start-up; then detect */
    BU_MS5607_SETTLE,
    BU_MS5607_DETECT,
    BU_DONE,
} bringup_state_t;

static struct {
    bringup_state_t state;
    uint32_t due_ms;
    i2c_recover_t recover;
    ms5607_detect_t detect;
} bu;

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

static void configure_i2c_pins(uint sda_pin) {
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(I2C_SCL_PIN);
}

void pressure_sensor_begin(void) {
    hal_telemetry_send("!PRES sensor init start\r\n");
    reset_block(RESETS_RESET_I2C1_BITS);
    unreset_block_wait(RESETS_RESET_I2C1_BITS);
    /* A STOP on both pads: either sensor may be the one left mid-transfer. */
    static const uint8_t sda[] = {BMP280_SDA, MS5607_SDA};
    i2c_recover_begin(&bu.recover, I2C_SCL_PIN, sda, 2);
    detected_sensor = PRESSURE_SENSOR_NONE;
    bu.state = BU_RECOVER;
}

static pressure_sensor_type_t done(pressure_sensor_type_t t) {
    detected_sensor = t;
    hal_telemetry_send(t == PRESSURE_SENSOR_BMP280   ? "!PRES init OK: BMP280\r\n"
                       : t == PRESSURE_SENSOR_MS5607 ? "!PRES init OK: MS5607\r\n"
                                                     : "!PRES init FAIL: no sensor found\r\n");
    bu.state = BU_DONE;
    return t;
}

pressure_sensor_type_t pressure_sensor_step(uint32_t now_ms) {
    switch (bu.state) {
    case BU_RECOVER:
        if (!i2c_recover_step(&bu.recover))
            return PRESSURE_SENSOR_PENDING;
        hal_telemetry_send("!PRES bus recovery done\r\n");
        i2c_init(i2c1, BOARD_BMP280_I2C_HZ);
        hal_telemetry_send("!PRES trying BMP280 (SDA=6)\r\n");
        configure_i2c_pins(BMP280_SDA);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_BMP280_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_BMP280_SETTLE: {
        if ((int32_t)(now_ms - bu.due_ms) < 0)
            return PRESSURE_SENSOR_PENDING;
        /* A soft reset at either address, which a missing sensor NACKs. */
        static const uint8_t reset_cmd[2] = {0xE0, 0xB6};
        i2c_write_blocking(i2c1, 0x76, reset_cmd, 2, false);
        i2c_write_blocking(i2c1, 0x77, reset_cmd, 2, false);
        bu.due_ms = now_ms + BMP280_STARTUP_MS;
        bu.state = BU_BMP280_START;
        return PRESSURE_SENSOR_PENDING;
    }

    case BU_BMP280_START:
        if ((int32_t)(now_ms - bu.due_ms) < 0)
            return PRESSURE_SENSOR_PENDING;
        if (bmp280_detect())
            return done(PRESSURE_SENSOR_BMP280);
        gpio_init(BMP280_SDA); /* back to a plain input: the pad is let go */
        i2c_set_baudrate(i2c1, BOARD_MS5607_I2C_HZ);
        hal_telemetry_send("!PRES trying MS5607 (SDA=10)\r\n");
        configure_i2c_pins(MS5607_SDA);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_MS5607_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_MS5607_SETTLE:
        if ((int32_t)(now_ms - bu.due_ms) < 0)
            return PRESSURE_SENSOR_PENDING;
        ms5607_detect_begin(&bu.detect);
        bu.state = BU_MS5607_DETECT;
        return PRESSURE_SENSOR_PENDING;

    case BU_MS5607_DETECT: {
        ms5607_detect_result_t r = ms5607_detect_step(&bu.detect, now_ms);
        if (r == MS5607_DETECT_PENDING)
            return PRESSURE_SENSOR_PENDING;
        return done(r == MS5607_DETECT_FOUND ? PRESSURE_SENSOR_MS5607 : PRESSURE_SENSOR_NONE);
    }

    case BU_DONE:
    default:
        return detected_sensor;
    }
}

const char *pressure_sensor_name(void) {
    switch (detected_sensor) {
    case PRESSURE_SENSOR_MS5607:
        return "MS5607";
    case PRESSURE_SENSOR_BMP280:
        return "BMP280";
    default:
        return "None";
    }
}
