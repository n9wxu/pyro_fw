/*
 * Pressure sensor interface — Pyro MK1A.
 *
 * One BMP280 (U4) on i2c0, SDA=GPIO20 / SCL=GPIO21, with 4k7 pull-ups fitted
 * (R1, R2). No MS5607, so none of the MK1B dual-SDA probing applies.
 *
 * The 9-pulse bus recovery preamble is kept. It is not about sensor
 * selection: the sensor stays powered across a CPU reset and can be left
 * mid-transaction holding SDA low, which is what commit 5019cf2 fixed on
 * MK1B. That failure mode is identical here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "bmp280_driver.h"
#include "i2c_recover.h"
#include "board_pins.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"

_Static_assert(BOARD_BMP280_I2C_HZ <= BMP280_I2C_MAX_HZ, "faster than the BMP280 allows");

#define I2C_SDA_PIN BOARD_PIN_I2C_SDA
#define I2C_SCL_PIN BOARD_PIN_I2C_SCL

/* The pull-ups settle before the first transfer. */
#define PULLUP_SETTLE_MS 10u

extern void hal_telemetry_send(const char *sentence);

/* [DD-053] Each state is a step a loop takes; nothing here waits. */
typedef enum { BU_RECOVER, BU_SETTLE, BU_DONE } bringup_state_t;

static struct {
    bringup_state_t state;
    uint32_t due_ms;
    i2c_recover_t recover;
} bu;

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

void pressure_sensor_begin(void) {
    hal_telemetry_send("!PRES sensor init start (MK1A)\r\n");
    reset_block(RESETS_RESET_I2C0_BITS);
    unreset_block_wait(RESETS_RESET_I2C0_BITS);
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
        i2c_init(BOARD_I2C_INST, BOARD_BMP280_I2C_HZ);
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
        hal_telemetry_send("!PRES trying BMP280 (i2c0 SDA=20 SCL=21)\r\n");
        detected_sensor = bmp280_detect() ? PRESSURE_SENSOR_BMP280 : PRESSURE_SENSOR_NONE;
        hal_telemetry_send(detected_sensor ? "!PRES init OK: BMP280\r\n" : "!PRES init FAIL: no sensor found\r\n");
        bu.state = BU_DONE;
        return detected_sensor;

    case BU_DONE:
    default:
        return detected_sensor;
    }
}

const char *pressure_sensor_name(void) {
    return (detected_sensor == PRESSURE_SENSOR_BMP280) ? "BMP280" : "None";
}
