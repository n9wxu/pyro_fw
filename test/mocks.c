/*
 * Mock implementations for unit tests.
 */
#include "mocks.h"
#include <string.h>
#include <stdio.h>

/* ── Mock state ───────────────────────────────────────────────────── */

mock_pressure_t mock_pressure = {0};
mock_pyro_t mock_pyro = {0};
char mock_uart_buf[MOCK_UART_BUF_SIZE];
int mock_uart_len = 0;
uint32_t mock_time_ms = 0;
uart_inst_t uart0_inst;
i2c_inst_t i2c1_inst;
const lfs_config lfs_pico_flash_config = {0};

void mock_reset_all(void) {
    memset(&mock_pressure, 0, sizeof(mock_pressure));
    mock_pressure.sensor_type = 2;  /* default: BMP280 */
    mock_pressure.pressure_pa = 101325.0f;
    memset(&mock_pyro, 0, sizeof(mock_pyro));
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    mock_time_ms = 0;
}

/* ── Pressure sensor mocks ────────────────────────────────────────── */

#include "../src/pressure_sensor.h"

pressure_sensor_type_t pressure_sensor_init(void) {
    return (pressure_sensor_type_t)mock_pressure.sensor_type;
}

bool pressure_sensor_read(pressure_reading_t *reading) {
    if (mock_pressure.sensor_type == 0) return false;
    reading->pressure_pa = mock_pressure.pressure_pa;
    reading->temperature_c = mock_pressure.temperature_c;
    return true;
}

const char *pressure_sensor_name(void) {
    switch (mock_pressure.sensor_type) {
        case 1: return "MS5607";
        case 2: return "BMP280";
        default: return "None";
    }
}

/* ── Pyro mocks ───────────────────────────────────────────────────── */

#include "../src/pyro.h"

void pyro_init(void) {}

void pyro_check_continuity(pyro_continuity_t *p1, pyro_continuity_t *p2) {
    p1->raw_adc = mock_pyro.p1_adc;
    p1->good = mock_pyro.p1_good;
    p1->open = mock_pyro.p1_open;
    p1->shorted = false;
    p2->raw_adc = mock_pyro.p2_adc;
    p2->good = mock_pyro.p2_good;
    p2->open = mock_pyro.p2_open;
    p2->shorted = false;
}

void pyro_fire(uint8_t channel) {
    mock_pyro.fire_count++;
    mock_pyro.last_fire_channel = channel;
    mock_pyro.firing = true;
}

void pyro_update(uint32_t now_ms) { (void)now_ms; }

bool pyro_is_firing(void) {
    return mock_pyro.firing;
}

/* ── UART mock ────────────────────────────────────────────────────── */

void uart_puts(uart_inst_t *uart, const char *s) {
    (void)uart;
    int len = strlen(s);
    if (mock_uart_len + len < MOCK_UART_BUF_SIZE) {
        memcpy(mock_uart_buf + mock_uart_len, s, len);
        mock_uart_len += len;
        mock_uart_buf[mock_uart_len] = '\0';
    }
}
