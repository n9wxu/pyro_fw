/*
 * src/pressure_sensor.h for MK1B: two SDA pads on one SCL, a BMP280's and an
 * MS5607's, and a board carries one or the other. See
 * THEORY_OF_OPERATION.md "Pressure sensor".
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "board_pins.h"
#include "board_support.h"
#include "bmp280_driver.h"
#include "i2c_recover.h"
#include "ms5607_driver.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/resets.h"

_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");
_Static_assert(BOARD_BMP280_I2C_HZ <= BMP280_I2C_MAX_HZ, "faster than the BMP280 allows");

#define I2C_SCL_PIN BOARD_PIN_I2C_SCL
#define BMP280_SDA BOARD_PIN_BMP280_SDA
#define MS5607_SDA BOARD_PIN_MS5607_SDA

#define PULLUP_SETTLE_MS 10u

/* BMP280 datasheet page 24: 0xB6 to register 0xE0 is a soft reset. */
#define BMP280_REG_RESET 0xE0
#define BMP280_SOFT_RESET 0xB6
#define BMP280_ADDR_SDO_LOW 0x76
#define BMP280_ADDR_SDO_HIGH 0x77

extern void hal_telemetry_send(const char *sentence);

/* Each state is a step a loop takes [DD-053]. */
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
    uint32_t i2c_block = BOARD_I2C_INST == i2c0 ? RESETS_RESET_I2C0_BITS : RESETS_RESET_I2C1_BITS;
    reset_block(i2c_block);
    unreset_block_wait(i2c_block);
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
        i2c_init(BOARD_I2C_INST, BOARD_BMP280_I2C_HZ);
        hal_telemetry_send("!PRES trying BMP280 (SDA=6)\r\n");
        configure_i2c_pins(BMP280_SDA);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_BMP280_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_BMP280_SETTLE: {
        if (!deadline_reached(now_ms, bu.due_ms))
            return PRESSURE_SENSOR_PENDING;
        /* At either address: a missing sensor NACKs, and a held bus times
         * out, since this also runs after a reset in flight. */
        static const uint8_t reset_cmd[2] = {BMP280_REG_RESET, BMP280_SOFT_RESET};
        i2c_write_timeout_us(BOARD_I2C_INST, BMP280_ADDR_SDO_LOW, reset_cmd, 2, false, 2000u);
        i2c_write_timeout_us(BOARD_I2C_INST, BMP280_ADDR_SDO_HIGH, reset_cmd, 2, false, 2000u);
        bu.due_ms = now_ms + BMP280_STARTUP_MS;
        bu.state = BU_BMP280_START;
        return PRESSURE_SENSOR_PENDING;
    }

    case BU_BMP280_START:
        if (!deadline_reached(now_ms, bu.due_ms))
            return PRESSURE_SENSOR_PENDING;
        if (bmp280_detect())
            return done(PRESSURE_SENSOR_BMP280);
        gpio_init(BMP280_SDA); /* back to a plain input: the pad is let go */
        i2c_set_baudrate(BOARD_I2C_INST, BOARD_MS5607_I2C_HZ);
        hal_telemetry_send("!PRES trying MS5607 (SDA=10)\r\n");
        configure_i2c_pins(MS5607_SDA);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_MS5607_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_MS5607_SETTLE:
        if (!deadline_reached(now_ms, bu.due_ms))
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
