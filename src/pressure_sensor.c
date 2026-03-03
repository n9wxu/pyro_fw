#include "pressure_sensor.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include <stdio.h>

extern bool ms5607_detect(void);
extern bool ms5607_read(pressure_reading_t *reading);
extern bool bmp280_detect(void);
extern bool bmp280_read(pressure_reading_t *reading);

#define I2C_SCL_PIN  7
#define BMP280_SDA   6
#define MS5607_SDA   10

static pressure_sensor_type_t detected_sensor = PRESSURE_SENSOR_NONE;

static void configure_i2c_pins(uint sda_pin) {
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(I2C_SCL_PIN);
}

pressure_sensor_type_t pressure_sensor_init(void) {
    i2c_init(i2c1, 400000);

    configure_i2c_pins(BMP280_SDA);
    if (bmp280_detect()) {
        detected_sensor = PRESSURE_SENSOR_BMP280;
        return detected_sensor;
    }

    configure_i2c_pins(MS5607_SDA);
    if (ms5607_detect()) {
        detected_sensor = PRESSURE_SENSOR_MS5607;
        return detected_sensor;
    }

    return PRESSURE_SENSOR_NONE;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    switch (detected_sensor) {
        case PRESSURE_SENSOR_MS5607: return ms5607_read(reading);
        case PRESSURE_SENSOR_BMP280: return bmp280_read(reading);
        default: return false;
    }
}

const char *pressure_sensor_name(void) {
    switch (detected_sensor) {
        case PRESSURE_SENSOR_MS5607: return "MS5607";
        case PRESSURE_SENSOR_BMP280: return "BMP280";
        default: return "None";
    }
}
