/*
 * Pressure sensor interface — Pyro MK1C.
 *
 * MK1C carries a single MS5607 (U2) on i2c1, SDA=GPIO6 / SCL=GPIO7, with
 * 4k7 pull-ups fitted (R3, R5). PS is tied to +3.3V (I2C mode) and CSB is
 * tied to GND. No BMP280 is fitted, so none of the MK1B dual-SDA probing
 * applies — see boards/mk1b/pressure_board.c for that board's version.
 *
 * The 9-pulse bus recovery preamble is kept. It is not about sensor
 * selection: the sensor stays powered across a CPU reset and can be left
 * mid-transaction holding SDA low, which is what commit 5019cf2 fixed on
 * MK1B. That failure mode is identical here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "i2c_recover.h"
#include "ms5607_driver.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"

#include "board_pins.h"

_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");

/* MK1C: one I2C bus, one sensor, no pin switching. */
#define I2C_SDA_PIN BOARD_PIN_I2C_SDA
#define I2C_SCL_PIN BOARD_PIN_I2C_SCL

/* The pull-ups settle before the first transfer. */
#define PULLUP_SETTLE_MS 10u

extern void hal_telemetry_send(const char *sentence);

/* [DD-053] Each state is a step a loop takes; nothing here waits. */
typedef enum { BU_RECOVER, BU_SETTLE, BU_DETECT, BU_DONE } bringup_state_t;

static struct {
    bringup_state_t state;
    uint32_t due_ms;
    i2c_recover_t recover;
    ms5607_detect_t detect;
} bu;

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

void pressure_sensor_begin(void) {
    hal_telemetry_send("!PRES sensor init start (MK1C)\r\n");
    reset_block(RESETS_RESET_I2C1_BITS);
    unreset_block_wait(RESETS_RESET_I2C1_BITS);
    static const uint8_t sda[] = {I2C_SDA_PIN};
    i2c_recover_begin(&bu.recover, I2C_SCL_PIN, sda, 1);
    detected_sensor = PRESSURE_SENSOR_NONE;
    bu.state = BU_RECOVER;
}

pressure_sensor_type_t pressure_sensor_step(uint32_t now_ms) {
    switch (bu.state) {
    case BU_RECOVER:
        if (!i2c_recover_step(&bu.recover))
            return PRESSURE_SENSOR_PENDING;
        hal_telemetry_send("!PRES bus recovery done\r\n");
        i2c_init(i2c1, BOARD_MS5607_I2C_HZ);
        gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
        gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
        gpio_pull_up(I2C_SDA_PIN);
        gpio_pull_up(I2C_SCL_PIN);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_SETTLE:
        if ((int32_t)(now_ms - bu.due_ms) < 0)
            return PRESSURE_SENSOR_PENDING;
        hal_telemetry_send("!PRES trying MS5607 (i2c1 SDA=6 SCL=7)\r\n");
        ms5607_detect_begin(&bu.detect);
        bu.state = BU_DETECT;
        return PRESSURE_SENSOR_PENDING;

    case BU_DETECT: {
        ms5607_detect_result_t r = ms5607_detect_step(&bu.detect, now_ms);
        if (r == MS5607_DETECT_PENDING)
            return PRESSURE_SENSOR_PENDING;
        detected_sensor = (r == MS5607_DETECT_FOUND) ? PRESSURE_SENSOR_MS5607 : PRESSURE_SENSOR_NONE;
        hal_telemetry_send(detected_sensor ? "!PRES init OK: MS5607\r\n" : "!PRES init FAIL: no sensor found\r\n");
        bu.state = BU_DONE;
        return detected_sensor;
    }

    case BU_DONE:
    default:
        return detected_sensor;
    }
}

const char *pressure_sensor_name(void) {
    return (detected_sensor == PRESSURE_SENSOR_MS5607) ? "MS5607" : "None";
}
