/*
 * Pressure sensor — REFERENCE BOARD (template).
 *
 * Implements src/pressure_sensor.h. Return value of pressure_sensor_init()
 * selects the sampling path in src/hal_common:
 *     0 = none, 1 = MS5607 (a one-shot conversion each loop), 2 = BMP280 (single read)
 *
 * Type 2 is only compiled when board_pins.h sets BOARD_HAS_BMP280 to 1.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "ms5607_driver.h"
#include "board_pins.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");


static pressure_sensor_type_t detected = PRESSURE_SENSOR_NONE;

pressure_sensor_type_t pressure_sensor_init(void) {
    extern void hal_telemetry_send(const char *sentence);

    /* A sensor stays powered across a CPU reset and can be left holding SDA
     * low mid-transaction. Both real boards clock the bus out before
     * touching the I2C peripheral -- see boards/mk1c/pressure_board.c for
     * the short version and boards/mk1b for the dual-SDA case. */

    i2c_init(BOARD_I2C_INST, BOARD_MS5607_I2C_HZ);
    gpio_set_function(BOARD_PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_PIN_I2C_SDA);
    gpio_pull_up(BOARD_PIN_I2C_SCL);
    sleep_ms(10);

    if (ms5607_detect()) {
        detected = PRESSURE_SENSOR_MS5607;
        hal_telemetry_send("!PRES init OK: MS5607\r\n");
        return detected;
    }

    hal_telemetry_send("!PRES init FAIL: no sensor found\r\n");
    return PRESSURE_SENSOR_NONE;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    if (detected == PRESSURE_SENSOR_MS5607)
        return ms5607_read(reading);
    return false;
}

const char *pressure_sensor_name(void) {
    return (detected == PRESSURE_SENSOR_MS5607) ? "MS5607" : "None";
}
