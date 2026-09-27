/*
 * src/pressure_sensor.h for a board with one pressure sensor on one I2C bus.
 * The sensor is whichever board_pins.h gives a bus speed for: MS5607 or
 * BMP280. A board with two on one bus, as MK1B has, writes its own
 * pressure_board.c.
 *
 * Each step of the bring-up is a loop iteration [DD-053]: clock the bus free,
 * hand it to the I2C block, let the pull-ups settle, detect.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "board_pins.h"
#include "board_support.h"
#include "i2c_recover.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/resets.h"
#include <stdio.h>

#if defined(BOARD_MS5607_I2C_HZ) && defined(BOARD_BMP280_I2C_HZ)
#error "two sensors on one bus: write a pressure_board.c, as boards/mk1b does"
#elif defined(BOARD_MS5607_I2C_HZ)
#include "ms5607_driver.h"
#define SENSOR_IS_MS5607 1
#define SENSOR PRESSURE_SENSOR_MS5607
#define SENSOR_NAME "MS5607"
#define SENSOR_I2C_HZ BOARD_MS5607_I2C_HZ
_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");
#elif defined(BOARD_BMP280_I2C_HZ)
#include "bmp280_driver.h"
#define SENSOR_IS_MS5607 0
#define SENSOR PRESSURE_SENSOR_BMP280
#define SENSOR_NAME "BMP280"
#define SENSOR_I2C_HZ BOARD_BMP280_I2C_HZ
_Static_assert(BOARD_BMP280_I2C_HZ <= BMP280_I2C_MAX_HZ, "faster than the BMP280 allows");
#else
#error "board_pins.h gives no sensor a bus speed: BOARD_MS5607_I2C_HZ or BOARD_BMP280_I2C_HZ"
#endif

#define PULLUP_SETTLE_MS 10u

extern void hal_telemetry_send(const char *sentence);

typedef enum { RECOVER_BUS, SETTLE_PULLUPS, DETECT, DONE } bringup_step_t;

static struct {
    bringup_step_t step;
    uint32_t due_ms;
    i2c_recover_t recover;
#if SENSOR_IS_MS5607
    ms5607_detect_t detect;
#endif
    pressure_sensor_type_t found;
} bringup;

static uint32_t i2c_reset_bits(void) {
    return BOARD_I2C_INST == i2c0 ? RESETS_RESET_I2C0_BITS : RESETS_RESET_I2C1_BITS;
}

static void hand_bus_to_i2c(void) {
    i2c_init(BOARD_I2C_INST, SENSOR_I2C_HZ);
    gpio_set_function(BOARD_PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_PIN_I2C_SDA);
    gpio_pull_up(BOARD_PIN_I2C_SCL);
}

static pressure_sensor_type_t finish(bool found) {
    bringup.found = found ? SENSOR : PRESSURE_SENSOR_NONE;
    bringup.step = DONE;
    hal_telemetry_send(found ? "!PRES init OK: " SENSOR_NAME "\r\n" : "!PRES init FAIL: no sensor found\r\n");
    return bringup.found;
}

void pressure_sensor_begin(void) {
    hal_telemetry_send("!PRES sensor init start (" BOARD_SHORT_STR ")\r\n");
    reset_block(i2c_reset_bits());
    unreset_block_wait(i2c_reset_bits());
    static const uint8_t sda[] = {BOARD_PIN_I2C_SDA};
    i2c_recover_begin(&bringup.recover, BOARD_PIN_I2C_SCL, sda, 1);
    bringup.found = PRESSURE_SENSOR_NONE;
    bringup.step = RECOVER_BUS;
}

pressure_sensor_type_t pressure_sensor_step(uint32_t now_ms) {
    switch (bringup.step) {
    case RECOVER_BUS:
        if (!i2c_recover_step(&bringup.recover))
            return PRESSURE_SENSOR_PENDING;
        hal_telemetry_send("!PRES bus recovery done\r\n");
        hand_bus_to_i2c();
        bringup.due_ms = now_ms + PULLUP_SETTLE_MS;
        bringup.step = SETTLE_PULLUPS;
        return PRESSURE_SENSOR_PENDING;

    case SETTLE_PULLUPS: {
        if (!deadline_reached(now_ms, bringup.due_ms))
            return PRESSURE_SENSOR_PENDING;
        char line[64];
        snprintf(line, sizeof(line), "!PRES trying " SENSOR_NAME " (SDA=%u SCL=%u)\r\n", BOARD_PIN_I2C_SDA,
                 BOARD_PIN_I2C_SCL);
        hal_telemetry_send(line);
#if SENSOR_IS_MS5607
        ms5607_detect_begin(&bringup.detect);
        bringup.step = DETECT;
        return PRESSURE_SENSOR_PENDING;
#else
        return finish(bmp280_detect());
#endif
    }

    case DETECT: {
#if SENSOR_IS_MS5607
        ms5607_detect_result_t r = ms5607_detect_step(&bringup.detect, now_ms);
        if (r == MS5607_DETECT_PENDING)
            return PRESSURE_SENSOR_PENDING;
        return finish(r == MS5607_DETECT_FOUND);
#else
        return bringup.found;
#endif
    }

    case DONE:
    default:
        return bringup.found;
    }
}

const char *pressure_sensor_name(void) {
    return bringup.found == SENSOR ? SENSOR_NAME : "None";
}
