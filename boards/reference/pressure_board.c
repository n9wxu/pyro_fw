/*
 * Pressure sensor — REFERENCE BOARD (template).
 *
 * Implements src/pressure_sensor.h. What pressure_sensor_step() finally
 * returns selects the sampling path in src/hal_common:
 *     0 = none, 1 = MS5607 (a one-shot conversion each loop), 2 = BMP280 (single read)
 *
 * Type 2 is only compiled when board_pins.h sets BOARD_HAS_BMP280 to 1.
 *
 * A sensor stays powered across a CPU reset and can be left holding SDA low
 * mid-transaction, so the bus is clocked free before the I2C peripheral takes
 * it. boards/mk1b has the dual-SDA case.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_sensor.h"
#include "i2c_recover.h"
#include "ms5607_driver.h"
#include "board_pins.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"

_Static_assert(BOARD_MS5607_I2C_HZ <= MS5607_I2C_MAX_HZ, "faster than the MS5607 allows");

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

static pressure_sensor_type_t detected = PRESSURE_SENSOR_NONE;

void pressure_sensor_begin(void) {
    uint32_t bits = (BOARD_I2C_INST == i2c0) ? RESETS_RESET_I2C0_BITS : RESETS_RESET_I2C1_BITS;
    reset_block(bits);
    unreset_block_wait(bits);
    static const uint8_t sda[] = {BOARD_PIN_I2C_SDA};
    i2c_recover_begin(&bu.recover, BOARD_PIN_I2C_SCL, sda, 1);
    detected = PRESSURE_SENSOR_NONE;
    bu.state = BU_RECOVER;
}

pressure_sensor_type_t pressure_sensor_step(uint32_t now_ms) {
    switch (bu.state) {
    case BU_RECOVER:
        if (!i2c_recover_step(&bu.recover))
            return PRESSURE_SENSOR_PENDING;
        i2c_init(BOARD_I2C_INST, BOARD_MS5607_I2C_HZ);
        gpio_set_function(BOARD_PIN_I2C_SDA, GPIO_FUNC_I2C);
        gpio_set_function(BOARD_PIN_I2C_SCL, GPIO_FUNC_I2C);
        gpio_pull_up(BOARD_PIN_I2C_SDA);
        gpio_pull_up(BOARD_PIN_I2C_SCL);
        bu.due_ms = now_ms + PULLUP_SETTLE_MS;
        bu.state = BU_SETTLE;
        return PRESSURE_SENSOR_PENDING;

    case BU_SETTLE:
        if ((int32_t)(now_ms - bu.due_ms) < 0)
            return PRESSURE_SENSOR_PENDING;
        ms5607_detect_begin(&bu.detect);
        bu.state = BU_DETECT;
        return PRESSURE_SENSOR_PENDING;

    case BU_DETECT: {
        ms5607_detect_result_t r = ms5607_detect_step(&bu.detect, now_ms);
        if (r == MS5607_DETECT_PENDING)
            return PRESSURE_SENSOR_PENDING;
        detected = (r == MS5607_DETECT_FOUND) ? PRESSURE_SENSOR_MS5607 : PRESSURE_SENSOR_NONE;
        hal_telemetry_send(detected ? "!PRES init OK: MS5607\r\n" : "!PRES init FAIL: no sensor found\r\n");
        bu.state = BU_DONE;
        return detected;
    }

    case BU_DONE:
    default:
        return detected;
    }
}

const char *pressure_sensor_name(void) {
    return (detected == PRESSURE_SENSOR_MS5607) ? "MS5607" : "None";
}
