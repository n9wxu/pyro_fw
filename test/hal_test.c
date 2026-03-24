/*
 * HAL implementation for unit/integration tests.
 * Wraps mock state variables for test control.
 * SPDX-License-Identifier: MIT
 */
#include "../src/hal.h"
#include "../src/config.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>

/* ── Mock state (shared with test files via mocks.h) ─────────────── */

mock_pressure_t mock_pressure = {0};
mock_pyro_t mock_pyro = {0};
char mock_uart_buf[MOCK_UART_BUF_SIZE];
int mock_uart_len = 0;
uint32_t mock_time_ms = 0;

/* XIP stall simulation */
uint32_t mock_xip_stall_ms = 0;
uint32_t mock_xip_total_stall_ms = 0;
int mock_xip_stall_count = 0;

/* Serial command mock queue [GND-TEST-01..04] */
char mock_serial_queue[MOCK_SERIAL_QUEUE_DEPTH][MOCK_SERIAL_LINE_MAX];
int mock_serial_queue_count = 0;

static void xip_stall(void) {
    if (mock_xip_stall_ms > 0) {
        mock_time_ms += mock_xip_stall_ms;
        mock_xip_total_stall_ms += mock_xip_stall_ms;
        mock_xip_stall_count++;
    }
}

/* In-memory filesystem */
#define SIM_FS_MAX_FILES 4
#define SIM_FS_MAX_SIZE 65536
typedef struct {
    char path[32];
    char data[SIM_FS_MAX_SIZE];
    int len;
    bool used;
} sim_file_t;
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
    mock_xip_stall_ms = 0;
    mock_xip_total_stall_ms = 0;
    mock_xip_stall_count = 0;
    mock_serial_queue_count = 0;
    mock_buzzer_tone_on_count = 0;
    mock_buzzer_tone_off_count = 0;
    memset(sim_files, 0, sizeof(sim_files));
}

/* Enqueue a serial command line for hal_serial_readline() to return */
void mock_serial_enqueue(const char *cmd) {
    if (mock_serial_queue_count < MOCK_SERIAL_QUEUE_DEPTH) {
        strncpy(mock_serial_queue[mock_serial_queue_count], cmd, MOCK_SERIAL_LINE_MAX - 1);
        mock_serial_queue[mock_serial_queue_count][MOCK_SERIAL_LINE_MAX - 1] = '\0';
        mock_serial_queue_count++;
    }
}

/* ── HAL implementation ───────────────────────────────────────────── */

uint32_t hal_time_ms(void) {
    return mock_time_ms;
}

int hal_pressure_init(void) {
    return mock_pressure.sensor_type;
}

bool hal_pressure_read(hal_pressure_t *out) {
    if (mock_pressure.sensor_type == 0)
        return false;
    out->pressure_pa = mock_pressure.pressure_pa;
    out->temperature_c = mock_pressure.temperature_c;
    return true;
}

/* ── Pressure FIFO stubs (polled mode only in tests) ──────────────── */

bool hal_pressure_fifo_start(uint8_t rate_hz) {
    (void)rate_hz;
    return false; /* test HAL uses polled mode */
}
bool hal_pressure_fifo_get(hal_pressure_batch_t *batch) {
    (void)batch;
    return false;
}
void hal_pressure_fifo_release(void) {}
bool hal_pressure_fifo_active(void) {
    return false;
}

void hal_pyro_init(void) {}

void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2) {
    p1->raw_adc = mock_pyro.p1_adc;
    p1->good = mock_pyro.p1_good;
    p1->open = mock_pyro.p1_open;
    p1->shorted = false;
    p2->raw_adc = mock_pyro.p2_adc;
    p2->good = mock_pyro.p2_good;
    p2->open = mock_pyro.p2_open;
    p2->shorted = false;
}

void hal_pyro_fire(uint8_t channel) {
    mock_pyro.fire_count++;
    mock_pyro.last_fire_channel = channel;
    mock_pyro.firing = true;
}

void hal_pyro_update(uint32_t now_ms) {
    (void)now_ms;
}
bool hal_pyro_is_firing(void) {
    return mock_pyro.firing;
}
bool hal_pyro_fault(uint8_t channel) {
    (void)channel;
    return mock_pyro.fault;
}

int mock_buzzer_tone_on_count = 0;
int mock_buzzer_tone_off_count = 0;

void hal_buzzer_init(void) {}
void hal_buzzer_tone_on(void) {
    mock_buzzer_tone_on_count++;
}
void hal_buzzer_tone_off(void) {
    mock_buzzer_tone_off_count++;
}

/* ── Buzzer async task (test) ─────────────────────────────────────── */
/* Store the buzzer task pointer so hal_tasks_tick() can drive it.
 * This allows integration tests to advance mock_time_ms and verify
 * the complete tone-on/off sequence without any main-loop involvement. */

static async_task_t *test_buzzer_task = NULL;

void hal_buzzer_task_register(async_task_t *task) {
    test_buzzer_task = task;
}

void hal_telemetry_send(const char *sentence) {
    int len = strlen(sentence);
    if (mock_uart_len + len < MOCK_UART_BUF_SIZE) {
        memcpy(mock_uart_buf + mock_uart_len, sentence, len);
        mock_uart_len += len;
        mock_uart_buf[mock_uart_len] = '\0';
    }
}

int hal_fs_mount(void) {
    return 0;
}
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
    xip_stall(); /* simulate flash erase+write XIP stall */
    int slot = -1;
    for (int i = 0; i < SIM_FS_MAX_FILES; i++) {
        if (sim_files[i].used && strcmp(sim_files[i].path, path) == 0) {
            slot = i;
            break;
        }
        if (!sim_files[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0 || len > SIM_FS_MAX_SIZE)
        return -1;
    strncpy(sim_files[slot].path, path, 31);
    memcpy(sim_files[slot].data, data, len);
    sim_files[slot].len = len;
    sim_files[slot].used = true;
    return 0;
}

/* ── Config (v2) ──────────────────────────────────────────────────── */

int hal_config_load(config_t *cfg) {
    config_set_defaults(cfg);
    /* Check if config.ini is stored in the mock filesystem */
    char buf[512];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        config_parse_ini(buf, cfg);
        return 0;
    }
    /* No file — write defaults so next boot finds them */
    const char *def = config_default_ini();
    hal_fs_write_file("config.ini", def, (int)strlen(def));
    return -1;
}

int hal_config_save(const config_t *cfg) {
    char buf[512];
    int n = config_serialize_ini(cfg, buf, (int)sizeof(buf));
    if (n <= 0)
        return -1;
    return hal_fs_write_file("config.ini", buf, n);
}

/* ── Serial readline (v2) ─────────────────────────────────────────── */

bool hal_serial_readline(char *buf, int max_len) {
    if (mock_serial_queue_count == 0)
        return false;
    strncpy(buf, mock_serial_queue[0], max_len - 1);
    buf[max_len - 1] = '\0';
    /* Shift queue left */
    for (int i = 0; i < mock_serial_queue_count - 1; i++)
        memcpy(mock_serial_queue[i], mock_serial_queue[i + 1], MOCK_SERIAL_LINE_MAX);
    mock_serial_queue_count--;
    return true;
}

/* ── Sleep (v2, no-op in test) ────────────────────────────────────── */

void hal_tasks_tick(uint32_t now_ms) {
    /* Drive the buzzer async task so integration tests can step through
     * tone-on/off sequences by advancing mock_time_ms. */
    if (test_buzzer_task && test_buzzer_task->tick && (int32_t)(now_ms - test_buzzer_task->next_due_ms) >= 0) {
        test_buzzer_task->tick(test_buzzer_task, now_ms);
    }
}

void hal_sleep_until_event(void) {
    /* No-op: test loop is tick-driven */
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
    if (test_file.open)
        return NULL;
    int slot = -1;
    for (int i = 0; i < SIM_FS_MAX_FILES; i++) {
        if (sim_files[i].used && strcmp(sim_files[i].path, path) == 0) {
            slot = i;
            break;
        }
        if (!sim_files[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return NULL;
    if (!append)
        sim_files[slot].len = 0;
    strncpy(sim_files[slot].path, path, 31);
    sim_files[slot].used = true;
    test_file.slot = slot;
    test_file.open = true;
    return &test_file;
}

int hal_fs_write(hal_file_t *f, const char *data, int len) {
    xip_stall(); /* simulate flash page write XIP stall */
    if (!f || !f->open)
        return -1;
    sim_file_t *sf = &sim_files[f->slot];
    int space = SIM_FS_MAX_SIZE - sf->len;
    int n = (len < space) ? len : space;
    if (n > 0) {
        memcpy(sf->data + sf->len, data, n);
        sf->len += n;
    }
    return n;
}

void hal_fs_close(hal_file_t *f) {
    if (f) {
        xip_stall(); /* simulate littlefs metadata commit */
        f->open = false;
    }
}
