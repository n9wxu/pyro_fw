#include "pressure_sensor.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <stdio.h>

extern bool ms5607_detect(void);
extern bool ms5607_read(pressure_reading_t *reading);
extern bool bmp280_detect(void);
extern bool bmp280_read(pressure_reading_t *reading);

#define I2C_SCL_PIN 7
#define BMP280_SDA 6
#define MS5607_SDA 10

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

static void configure_i2c_pins(uint sda_pin) {
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(I2C_SCL_PIN);
}

static void release_i2c_pin(uint pin) {
    gpio_init(pin); /* reset to SIO input, removes I2C function */
}

pressure_sensor_type_t pressure_sensor_init(void) {
    extern void hal_telemetry_send(const char *sentence);

    hal_telemetry_send("!PRES sensor init start\r\n");
    i2c_init(i2c1, 100000);
    sleep_ms(10); /* Allow I2C bus to stabilize after init */

    hal_telemetry_send("!PRES trying BMP280 (SDA=6)\r\n");
    configure_i2c_pins(BMP280_SDA);
    sleep_ms(10); /* Allow GPIO/pull-ups to stabilize */
    if (bmp280_detect()) {
        detected_sensor = PRESSURE_SENSOR_BMP280;
        hal_telemetry_send("!PRES init OK: BMP280\r\n");
        return detected_sensor;
    }
    release_i2c_pin(BMP280_SDA);

    hal_telemetry_send("!PRES trying MS5607 (SDA=10)\r\n");
    configure_i2c_pins(MS5607_SDA);
    if (ms5607_detect()) {
        detected_sensor = PRESSURE_SENSOR_MS5607;
        hal_telemetry_send("!PRES init OK: MS5607\r\n");
        return detected_sensor;
    }

    hal_telemetry_send("!PRES init FAIL: no sensor found\r\n");
    return PRESSURE_SENSOR_NONE;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    switch (detected_sensor) {
    case PRESSURE_SENSOR_MS5607:
        return ms5607_read(reading);
    case PRESSURE_SENSOR_BMP280:
        return bmp280_read(reading);
    default:
        return false;
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
