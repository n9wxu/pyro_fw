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
#include "../src/pressure_processing.h"
#include "../src/config.h"
#include "../src/flight_events.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Simulation state (accessible from main_sim.c) ────────────────── */

/* Time */
static uint32_t sim_time = 0;

/* Pressure — set by physics engine */
static float sim_pressure_pa = 101325.0f;
static int sim_sensor_type = 2;
static uint32_t sim_last_feed_ms;
static bool sim_fed;

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

void sim_set_time(uint32_t ms) {
    sim_time = ms;
}
void sim_set_pressure(float pa) {
    sim_pressure_pa = pa;
}
void sim_set_sensor_type(int type) {
    sim_sensor_type = type;
}
void sim_set_continuity(int ch, uint16_t adc, bool good, bool open) {
    hal_continuity_t *c = (ch == 1) ? &sim_cont1 : &sim_cont2;
    c->raw_adc = adc;
    c->good = good;
    c->open = open;
    c->shorted = false;
}
void sim_clear_pyro_firing(void) {
    sim_pyro_firing = false;
}
int sim_get_pyro_fire_count(void) {
    return sim_pyro_fire_count;
}
uint8_t sim_get_pyro_last_channel(void) {
    return sim_pyro_last_channel;
}
bool sim_get_buzzer_state(void) {
    return sim_buzzer_on;
}
const char *sim_get_telemetry(void) {
    return sim_telem_buf;
}
int sim_get_telemetry_len(void) {
    return sim_telem_len;
}
void sim_clear_telemetry(void) {
    sim_telem_len = 0;
    sim_telem_buf[0] = '\0';
}

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
    sim_fed = false;
}

/* ── HAL implementation ───────────────────────────────────────────── */

/* The simulator is always a fresh start: there is no power event to recover
 * from, so brownout recovery never engages here. A sim that reported a power
 * event would go looking for a flight in a marker file that does not exist. */
reset_cause_t hal_reset_cause(void) {
    return RESET_SOFTWARE;
}

uint32_t hal_time_ms(void) {
    return sim_time;
}

int hal_pressure_init(void) {
    return sim_sensor_type;
}

/* ── Pyro ─────────────────────────────────────────────────────────
 *
 * Two implementations, chosen at build time.
 *
 * By default the pyro side is a FIXTURE: continuity is whatever a test
 * last wrote with sim_set_continuity(), and a fire is a counter. That is
 * the right thing for the flight-logic tests, which care about when the
 * state machine decides to fire and not about what the board does when it
 * does.
 *
 * The boards/sim_mk1a, sim_mk1b and sim_mk1c packages define
 * PYRO_SIM_BOARD_PYRO instead. They compile the REAL board file from
 * boards/<name>/pyro_board.c against sim/hw/ and sim/plant/, so the sense
 * thresholds, the settle timing and the firing sequence all run against a
 * modelled board. Their glue supplies hal_pyro_* from pyro.h, so the
 * fixture below must not also define it. */
#ifndef PYRO_SIM_BOARD_PYRO

void hal_pyro_init(void) {}

/* The simulator has no pin assignment to release from, so both channels
 * are always the flight software's. Present because hal.h asks for it:
 * a HAL that silently omits an entry point is one that links until
 * something calls it. */
int hal_pyro_claim_channels(uint32_t (*pads_of)(uint8_t channel)) {
    (void)pads_of;
    return 2; /* both channels */
}

void hal_pyro_sample(void) {
}

void hal_pyro_get(uint8_t channel, hal_continuity_t *out) {
    if (channel == 1)
        *out = sim_cont1;
    else if (channel == 2)
        *out = sim_cont2;
}

void hal_pyro_fire(uint8_t channel) {
    sim_pyro_fire_count++;
    sim_pyro_last_channel = channel;
    sim_pyro_firing = true;
}

void hal_pyro_update(uint32_t now_ms) {
    (void)now_ms;
}
bool hal_pyro_is_firing(void) {
    return sim_pyro_firing;
}
bool hal_pyro_fault(uint8_t channel) {
    (void)channel;
    return false;
}

#endif /* !PYRO_SIM_BOARD_PYRO */

/* Record a fire for the simulation's own bookkeeping, which is what the
 * JS wrapper reads as sim.pyroFireCount and sim.lastFireChannel.
 *
 * With the fixture above, hal_pyro_fire() calls this directly and the
 * count means "the flight software commanded a fire". With a modelled
 * board it is called from the plant's ignition latch instead, so the
 * count means "a match actually took its ignition energy" -- which is
 * the event a chute deployment should follow, and not always the same
 * one. A commanded fire into an open channel does not deploy anything. */
void sim_note_pyro_fire(uint8_t channel) {
    sim_pyro_fire_count++;
    sim_pyro_last_channel = channel;
    sim_pyro_firing = true;
}

void hal_buzzer_init(void) {
    sim_buzzer_on = false;
}
void hal_buzzer_tone_on(void) {
    sim_buzzer_on = true;
}
void hal_buzzer_tone_off(void) {
    sim_buzzer_on = false;
}

/* ── Buzzer async task (sim) ──────────────────────────────────────── */
/* Store the buzzer task pointer so hal_tasks_tick() can drive it.
 * sim_get_buzzer_state() reads the GPIO state set by tone_on/off,
 * which is correct because the buzzer task calls those directly. */

static async_task_t *sim_buzzer_task = NULL;

void hal_buzzer_task_register(async_task_t *task) {
    sim_buzzer_task = task;
}

void hal_telemetry_send(const char *sentence) {
    int len = strlen(sentence);
    if (sim_telem_len + len < SIM_TELEM_BUF) {
        memcpy(sim_telem_buf + sim_telem_len, sentence, len);
        sim_telem_len += len;
        sim_telem_buf[sim_telem_len] = '\0';
    }
}

/* The simulator's filesystem is memory, so the mount cannot fail. Present because
 * hal.h asks for it. */
bool hal_fs_healthy(void) {
    return true;
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
    return -2; /* not found */
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    /* Find existing or empty slot */
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
    char buf[512];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        config_parse_ini(buf, cfg);
        return 0;
    }
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

/* ── Serial readline (sim: no serial input) ──────────────────────── */

bool hal_serial_readline(char *buf, int max_len) {
    (void)buf;
    (void)max_len;
    return false; /* simulation has no serial input */
}

/* ── Async task runner (sim) ─────────────────────────────────────── */

/* The sensor's rate on the hardware (FLT-RATE-01, DD-001). Fed every tick
 * instead, the speed becomes a difference of pressures one millisecond apart
 * -- quantisation noise that no descent phase can settle on. */
#define SIM_SAMPLE_MS 20

void hal_tasks_tick(uint32_t now_ms) {
    /* Feed pressure_processing so detectors read altitude via pp_read(). */
    if (sim_sensor_type > 0 && (!sim_fed || now_ms - sim_last_feed_ms >= SIM_SAMPLE_MS)) {
        pp_feed((int32_t)sim_pressure_pa, now_ms);
        sim_last_feed_ms = now_ms;
        sim_fed = true;
    }

    /* Drive the buzzer task so the sim produces correct buzzer audio. */
    if (sim_buzzer_task && sim_buzzer_task->tick && (int32_t)(now_ms - sim_buzzer_task->next_due_ms) >= 0) {
        sim_buzzer_task->tick(sim_buzzer_task, now_ms);
    }
}

/* ── Sleep (sim: no-op) ───────────────────────────────────────────── */

void hal_sleep_until_event(void) {
    /* No-op: sim loop is driven by physics engine */
}

void hal_platform_init(void) {}
void hal_platform_service(void) {}
void hal_firmware_commit(void) {}

/* ── In-flight data logging [v2-9] (sim: write to flight_sim.csv) ── */

static FILE *sim_log_file = NULL;
static bool sim_log_running = false;

void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa) {
    if (sim_log_running)
        return;
    sim_log_file = fopen("flight_sim.csv", "w");
    if (!sim_log_file)
        return;
    fprintf(sim_log_file,
            "# Pyro MK1B Flight Data\n# ID: %.8s\n# Name: %.8s\n"
            "# Pyro1: %s %u\n# Pyro2: %s %u\n"
            "# Units: %s\n# Ground Pa: %ld\n"
            "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
            cfg->id, cfg->name, config_mode_name(cfg->pyro1_mode), cfg->pyro1_value, config_mode_name(cfg->pyro2_mode),
            cfg->pyro2_value,
            cfg->units == 2   ? "ft"
            : cfg->units == 1 ? "m"
                              : "cm",
            (long)ground_pressure_pa);
    sim_log_running = true;
}

void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event) {
    if (!sim_log_running || !sim_log_file)
        return;
    fprintf(sim_log_file, "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)time_ms, (long)pressure_pa, (long)altitude_cm, state,
            under_thrust, flight_event_name(event));
}

void hal_log_stop(void) {
    if (!sim_log_running)
        return;
    if (sim_log_file) {
        fclose(sim_log_file);
        sim_log_file = NULL;
    }
    sim_log_running = false;
}

bool hal_log_active(void) {
    return sim_log_running;
}

/* ── Streaming file writes ────────────────────────────────────────── */

struct hal_file {
    int slot;
    bool open;
};

static struct hal_file sim_file_handle;

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (sim_file_handle.open)
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
    sim_file_handle.slot = slot;
    sim_file_handle.open = true;
    return &sim_file_handle;
}

int hal_fs_write(hal_file_t *f, const char *data, int len) {
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
    if (f)
        f->open = false;
}
