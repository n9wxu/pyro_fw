/*
 * HAL implementation for simulation (host or WASM).
 *
 * Pressure comes from the physics engine. Pyro fires feed back
 * into the physics. Buzzer tone state is exported for audio.
 * Telemetry is captured in a buffer. Filesystem is in-memory.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/hal.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Simulation state (accessible from main_sim.c) ────────────────── */

/* Time */
static uint32_t sim_time = 0;

/* Pressure — set by physics engine */
static float sim_pressure_pa = 101325.0f;
static int sim_sensor_type = 2;

/* Pyro */
static int sim_pyro_fire_count = 0;
static uint8_t sim_pyro_last_channel = 0;
static bool sim_pyro_firing = false;

/* Continuity — set by user */
static hal_continuity_t sim_cont1 = {50, true, false, false};
static hal_continuity_t sim_cont2 = {50, true, false, false};

/* Buzzer */
static bool sim_buzzer_on = false;

/* Telemetry capture */
#define SIM_TELEM_BUF 8192
static char sim_telem_buf[SIM_TELEM_BUF];
static int sim_telem_len = 0;

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

/* ── Accessors for main_sim.c ─────────────────────────────────────── */

void sim_set_time(uint32_t ms)          { sim_time = ms; }
void sim_set_pressure(float pa)         { sim_pressure_pa = pa; }
void sim_set_sensor_type(int type)      { sim_sensor_type = type; }
void sim_set_continuity(int ch, uint16_t adc, bool good, bool open) {
    hal_continuity_t *c = (ch == 1) ? &sim_cont1 : &sim_cont2;
    c->raw_adc = adc; c->good = good; c->open = open; c->shorted = false;
}
void sim_clear_pyro_firing(void)        { sim_pyro_firing = false; }
int  sim_get_pyro_fire_count(void)      { return sim_pyro_fire_count; }
uint8_t sim_get_pyro_last_channel(void) { return sim_pyro_last_channel; }
bool sim_get_buzzer_state(void)         { return sim_buzzer_on; }
const char *sim_get_telemetry(void)     { return sim_telem_buf; }
int  sim_get_telemetry_len(void)        { return sim_telem_len; }
void sim_clear_telemetry(void)          { sim_telem_len = 0; sim_telem_buf[0] = '\0'; }

void sim_reset(void) {
    sim_time = 0;
    sim_pressure_pa = 101325.0f;
    sim_sensor_type = 2;
    sim_pyro_fire_count = 0;
    sim_pyro_last_channel = 0;
    sim_pyro_firing = false;
    sim_cont1 = (hal_continuity_t){50, true, false, false};
    sim_cont2 = (hal_continuity_t){50, true, false, false};
    sim_buzzer_on = false;
    sim_telem_len = 0;
    sim_telem_buf[0] = '\0';
    memset(sim_files, 0, sizeof(sim_files));
}

/* ── HAL implementation ───────────────────────────────────────────── */

uint32_t hal_time_ms(void) { return sim_time; }

int hal_pressure_init(void) { return sim_sensor_type; }

bool hal_pressure_read(hal_pressure_t *out) {
    if (sim_sensor_type == 0) return false;
    out->pressure_pa = sim_pressure_pa;
    out->temperature_c = 25.0f;
    return true;
}

void hal_pyro_init(void) {}

void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2) {
    *p1 = sim_cont1;
    *p2 = sim_cont2;
}

void hal_pyro_fire(uint8_t channel) {
    sim_pyro_fire_count++;
    sim_pyro_last_channel = channel;
    sim_pyro_firing = true;
}

void hal_pyro_update(uint32_t now_ms) { (void)now_ms; }
bool hal_pyro_is_firing(void) { return sim_pyro_firing; }

void hal_buzzer_init(void) { sim_buzzer_on = false; }
void hal_buzzer_tone_on(void)  { sim_buzzer_on = true; }
void hal_buzzer_tone_off(void) { sim_buzzer_on = false; }

void hal_telemetry_send(const char *sentence) {
    int len = strlen(sentence);
    if (sim_telem_len + len < SIM_TELEM_BUF) {
        memcpy(sim_telem_buf + sim_telem_len, sentence, len);
        sim_telem_len += len;
        sim_telem_buf[sim_telem_len] = '\0';
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
    return -2; /* not found */
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    /* Find existing or empty slot */
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
