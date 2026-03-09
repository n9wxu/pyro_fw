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

/* In-memory filesystem */
#define SIM_FS_MAX_FILES 4
#define SIM_FS_MAX_SIZE 65536
typedef struct { char path[32]; char data[SIM_FS_MAX_SIZE]; int len; bool used; } sim_file_t;
static sim_file_t sim_files[SIM_FS_MAX_FILES];

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
    memset(sim_files, 0, sizeof(sim_files));
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
    for (int i = 0; i < SIM_FS_MAX_FILES; i++) {
        if (sim_files[i].used && strcmp(sim_files[i].path, path) == 0) {
            int n = sim_files[i].len < max_len ? sim_files[i].len : max_len;
            memcpy(buf, sim_files[i].data, n);
            return n;
        }
    }
    return -2;
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    int slot = -1;
    for (int i = 0; i < SIM_FS_MAX_FILES; i++) {
        if (sim_files[i].used && strcmp(sim_files[i].path, path) == 0) { slot = i; break; }
        if (!sim_files[i].used && slot < 0) slot = i;
    }
    if (slot < 0 || len > SIM_FS_MAX_SIZE) return -1;
    strncpy(sim_files[slot].path, path, 31);
    memcpy(sim_files[slot].data, data, len);
    sim_files[slot].len = len;
    sim_files[slot].used = true;
    return 0;
}

void hal_platform_init(void) {}
void hal_platform_service(void) {}
void hal_firmware_commit(void) {}

/* ── Streaming file writes (test) ─────────────────────────────────── */

struct hal_file {
    int slot;
    bool open;
};

static struct hal_file test_file;

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (test_file.open) return NULL;
    int slot = -1;
    for (int i = 0; i < SIM_FS_MAX_FILES; i++) {
        if (sim_files[i].used && strcmp(sim_files[i].path, path) == 0) { slot = i; break; }
        if (!sim_files[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return NULL;
    if (!append) sim_files[slot].len = 0;
    strncpy(sim_files[slot].path, path, 31);
    sim_files[slot].used = true;
    test_file.slot = slot;
    test_file.open = true;
    return &test_file;
}

int hal_fs_write(hal_file_t *f, const char *data, int len) {
    if (!f || !f->open) return -1;
    sim_file_t *sf = &sim_files[f->slot];
    int space = SIM_FS_MAX_SIZE - sf->len;
    int n = (len < space) ? len : space;
    if (n > 0) { memcpy(sf->data + sf->len, data, n); sf->len += n; }
    return n;
}

void hal_fs_close(hal_file_t *f) {
    if (f) f->open = false;
}
