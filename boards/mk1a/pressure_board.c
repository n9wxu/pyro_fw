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
#include "board_pins.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"
#include "pico/stdlib.h"

#define I2C_SDA_PIN BOARD_PIN_I2C_SDA
#define I2C_SCL_PIN BOARD_PIN_I2C_SCL

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

/* Clock out any slave left mid-transaction after a CPU reset. SCL is driven
 * as a plain GPIO for this; the I2C peripheral is not involved yet. */
static void i2c_bus_recover(void) {
    reset_block(RESETS_RESET_I2C0_BITS);
    unreset_block_wait(RESETS_RESET_I2C0_BITS);

    gpio_init(I2C_SCL_PIN);
    gpio_set_dir(I2C_SCL_PIN, GPIO_OUT);
    gpio_put(I2C_SCL_PIN, 1);

    gpio_init(I2C_SDA_PIN);
    gpio_set_dir(I2C_SDA_PIN, GPIO_IN);
    gpio_pull_up(I2C_SDA_PIN);

    sleep_ms(1);

    for (int i = 0; i < 9; i++) {
        gpio_put(I2C_SCL_PIN, 0);
        sleep_us(10);
        gpio_put(I2C_SCL_PIN, 1);
        sleep_us(10);
    }

    /* STOP condition: SDA low->high while SCL is high */
    gpio_set_dir(I2C_SDA_PIN, GPIO_OUT);
    gpio_put(I2C_SDA_PIN, 0);
    sleep_us(10);
    gpio_put(I2C_SDA_PIN, 1);
    sleep_us(10);

    /* Hand both lines back as inputs before the peripheral takes them */
    gpio_set_dir(I2C_SCL_PIN, GPIO_IN);
    gpio_pull_up(I2C_SCL_PIN);
    gpio_set_dir(I2C_SDA_PIN, GPIO_IN);
    gpio_pull_up(I2C_SDA_PIN);

    sleep_ms(1);
}

pressure_sensor_type_t pressure_sensor_init(void) {
    extern void hal_telemetry_send(const char *sentence);

    hal_telemetry_send("!PRES sensor init start (MK1A)\r\n");

    i2c_bus_recover();
    hal_telemetry_send("!PRES bus recovery done\r\n");

    i2c_init(BOARD_I2C_INST, 100000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    sleep_ms(10); /* let the pull-ups settle before the first transfer */

    hal_telemetry_send("!PRES trying BMP280 (i2c0 SDA=20 SCL=21)\r\n");
    if (bmp280_detect()) {
        detected_sensor = PRESSURE_SENSOR_BMP280;
        hal_telemetry_send("!PRES init OK: BMP280\r\n");
        return detected_sensor;
    }

    hal_telemetry_send("!PRES init FAIL: no sensor found\r\n");
    return PRESSURE_SENSOR_NONE;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    if (detected_sensor == PRESSURE_SENSOR_BMP280)
        return bmp280_read(reading);
    return false;
}

const char *pressure_sensor_name(void) {
    return (detected_sensor == PRESSURE_SENSOR_BMP280) ? "BMP280" : "None";
}
