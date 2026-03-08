/*
 * HAL implementation for unit/integration tests.
 * Wraps mock state variables for test control.
 * SPDX-License-Identifier: MIT
 */
#include "../src/hal.h"
#include <string.h>
#include <stdio.h>

/* ── Mock state (shared with test files) ──────────────────────────── */

typedef struct {
    float pressure_pa;
    float temperature_c;
    int sensor_type;  /* 0=none, 1=ms5607, 2=bmp280 */
} mock_pressure_t;

typedef struct {
    uint16_t p1_adc;
    uint16_t p2_adc;
    bool p1_good;
    bool p2_good;
    bool p1_open;
    bool p2_open;
    bool firing;
    int fire_count;
    uint8_t last_fire_channel;
} mock_pyro_t;

#define MOCK_UART_BUF_SIZE 4096

mock_pressure_t mock_pressure = {0};
mock_pyro_t mock_pyro = {0};
char mock_uart_buf[MOCK_UART_BUF_SIZE];
int mock_uart_len = 0;
uint32_t mock_time_ms = 0;

void mock_reset_all(void) {
    memset(&mock_pressure, 0, sizeof(mock_pressure));
    mock_pressure.sensor_type = 2;
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

/* ── HAL implementation ───────────────────────────────────────────── */

uint32_t hal_time_ms(void) { return mock_time_ms; }

int hal_pressure_init(void) { return mock_pressure.sensor_type; }

bool hal_pressure_read(hal_pressure_t *out) {
    if (mock_pressure.sensor_type == 0) return false;
    out->pressure_pa = mock_pressure.pressure_pa;
    out->temperature_c = mock_pressure.temperature_c;
    return true;
}

void hal_pyro_init(void) {}

void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2) {
    p1->raw_adc = mock_pyro.p1_adc; p1->good = mock_pyro.p1_good;
    p1->open = mock_pyro.p1_open;   p1->shorted = false;
    p2->raw_adc = mock_pyro.p2_adc; p2->good = mock_pyro.p2_good;
    p2->open = mock_pyro.p2_open;   p2->shorted = false;
}

void hal_pyro_fire(uint8_t channel) {
    mock_pyro.fire_count++;
    mock_pyro.last_fire_channel = channel;
    mock_pyro.firing = true;
}

void hal_pyro_update(uint32_t now_ms) { (void)now_ms; }
bool hal_pyro_is_firing(void) { return mock_pyro.firing; }

void hal_buzzer_init(void) {}
void hal_buzzer_tone_on(void) {}
void hal_buzzer_tone_off(void) {}

void hal_telemetry_send(const char *sentence) {
    int len = strlen(sentence);
    if (mock_uart_len + len < MOCK_UART_BUF_SIZE) {
        memcpy(mock_uart_buf + mock_uart_len, sentence, len);
        mock_uart_len += len;
        mock_uart_buf[mock_uart_len] = '\0';
    }
}

int hal_fs_mount(void) { return 0; }
void hal_fs_unmount(void) {}
int hal_fs_read_file(const char *path, char *buf, int max_len) {
    (void)path; (void)buf; (void)max_len;
    return -2;  /* file not found — triggers default config creation */
}
int hal_fs_write_file(const char *path, const char *data, int len) {
    (void)path; (void)data; (void)len;
    return 0;
}

void hal_platform_init(void) {}
void hal_platform_service(void) {}
void hal_firmware_commit(void) {}
