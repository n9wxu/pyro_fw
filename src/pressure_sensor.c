#include "pressure_sensor.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"
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

    /* I2C bus recovery: manually clock out any stuck transactions.
     * After CPU reset, sensor chips remain powered and may be stuck
     * mid-transaction with SDA held low. Standard recovery is to
     * clock out 9 SCL pulses to reset the slave device's I2C state. */

    /* Hard reset I2C1 peripheral using RP2040 reset controller */
    reset_block(RESETS_RESET_I2C1_BITS);
    unreset_block_wait(RESETS_RESET_I2C1_BITS);

    /* Configure SCL as GPIO output to manually clock the bus */
    gpio_init(I2C_SCL_PIN);
    gpio_set_dir(I2C_SCL_PIN, GPIO_OUT);
    gpio_put(I2C_SCL_PIN, 1);

    /* Configure both SDA pins as GPIO inputs with pull-ups */
    gpio_init(BMP280_SDA);
    gpio_set_dir(BMP280_SDA, GPIO_IN);
    gpio_pull_up(BMP280_SDA);

    gpio_init(MS5607_SDA);
    gpio_set_dir(MS5607_SDA, GPIO_IN);
    gpio_pull_up(MS5607_SDA);

    sleep_ms(1);

    /* Clock out 9 pulses on SCL to reset any stuck I2C slave state */
    for (int i = 0; i < 9; i++) {
        gpio_put(I2C_SCL_PIN, 0);
        sleep_us(10);
        gpio_put(I2C_SCL_PIN, 1);
        sleep_us(10);
    }

    /* Send I2C STOP condition (SDA low->high while SCL is high) */
    gpio_init(BMP280_SDA);
    gpio_set_dir(BMP280_SDA, GPIO_OUT);
    gpio_put(BMP280_SDA, 0);
    sleep_us(10);
    gpio_put(BMP280_SDA, 1);
    sleep_us(10);

    /* Reset SCL back to input mode to avoid glitches during I2C init */
    gpio_set_dir(I2C_SCL_PIN, GPIO_IN);
    gpio_pull_up(I2C_SCL_PIN);

    /* Reset SDA back to input mode */
    gpio_set_dir(BMP280_SDA, GPIO_IN);
    gpio_pull_up(BMP280_SDA);

    sleep_ms(1);

    hal_telemetry_send("!PRES bus recovery done\r\n");

    /* Now initialize I2C peripheral normally */
    i2c_init(i2c1, 100000);
    sleep_ms(10);

    /* Try to send software reset to BMP280 before detection.
     * Write 0xB6 to register 0xE0 triggers complete power-on-reset.
     * This resets the sensor's internal state machine. */
    hal_telemetry_send("!PRES trying BMP280 (SDA=6)\r\n");
    configure_i2c_pins(BMP280_SDA);
    sleep_ms(10); /* Allow GPIO/pull-ups to stabilize */

    /* Attempt BMP280 software reset (may fail if sensor not present) */
    uint8_t reset_cmd[2] = {0xE0, 0xB6};                 /* Register 0xE0, reset value 0xB6 */
    i2c_write_blocking(i2c1, 0x76, reset_cmd, 2, false); /* Try addr 0x76 */
    i2c_write_blocking(i2c1, 0x77, reset_cmd, 2, false); /* Try addr 0x77 */
    sleep_ms(10);                                        /* Wait for reset to complete (datasheet: 2ms typical) */
    hal_telemetry_send("!PRES sent BMP280 reset command\r\n");
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
