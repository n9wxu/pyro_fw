/*
 * HAL implementation for unit/integration tests.
 * Wraps mock state variables for test control.
 * SPDX-License-Identifier: MIT
 */
#include "../src/hal.h"
#include "../src/pyro_release.h"
#include "../src/pad_claim.h"
#include "../src/config.h"
#include "../src/device_status.h"
#include "../src/pressure_processing.h"
#include "../src/board_id.h"
#include "../src/flight_events.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

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

/* Streaming file handle — declared here so mock_reset_all() can reset it */
struct hal_file {
    int slot;
    bool open;
};
static struct hal_file test_file;

/* In-flight log state — declared here so mock_reset_all() can reset it */
static hal_file_t *test_log_file = NULL;
static bool test_log_active = false;

/* ── Sensor model ─────────────────────────────────────────────────── */

uint32_t mock_sample_interval_ms = 20;
bool mock_sensor_stuck = false;
float mock_noise_rms_pa = 0.0f;
uint32_t mock_noise_seed = 1;
int32_t mock_glitch_pa = 0;
int mock_glitch_samples = 0;
bool mock_stall_model = false;
uint32_t mock_stall_seed = 1;
uint32_t mock_stall_count, mock_stall_total_ms, mock_stall_min_ms, mock_stall_max_ms;
uint32_t mock_stamp_lag_min_ms, mock_stamp_lag_max_ms;
uint32_t mock_pres_rejects;

static struct {
    uint32_t noise_rng, stall_rng;
    bool noise_seeded, stall_seeded;
    bool spare_valid;
    float spare;
    /* The true pressure at each tick, so a reading can be taken at its
     * conversion and handed over later, as the hardware does. */
    struct {
        uint32_t t;
        float pa;
    } hist[256];
    unsigned hist_n, hist_head;
    uint32_t stall_until, next_stall_at;
    int phase;
    uint32_t d1_ms, due_ms;
} sm;

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float uniform01(uint32_t *s) {
    return ((float)(xorshift32(s) >> 8) + 0.5f) / 16777216.0f;
}

static float gaussian(void) {
    if (!sm.noise_seeded) {
        sm.noise_rng = mock_noise_seed;
        sm.noise_seeded = true;
    }
    if (sm.spare_valid) {
        sm.spare_valid = false;
        return sm.spare;
    }
    float r = sqrtf(-2.0f * logf(uniform01(&sm.noise_rng)));
    float th = 6.2831853f * uniform01(&sm.noise_rng);
    sm.spare = r * sinf(th);
    sm.spare_valid = true;
    return r * cosf(th);
}

/* What the HAL does with a reading: truncate to whole pascals, and discard one
 * no atmosphere can produce [SNS-PRES-06]. */
static void feed_reading(float true_pa, uint32_t stamp_ms) {
    /* A stuck sensor answers with its last reading, to the pascal. */
    if (mock_sensor_stuck) {
        pp_feed(pp_last_raw_pa(), stamp_ms);
        return;
    }
    float p = true_pa;
    if (mock_noise_rms_pa > 0.0f)
        p += mock_noise_rms_pa * gaussian();
    if (mock_glitch_samples > 0) {
        p += (float)mock_glitch_pa;
        mock_glitch_samples--;
    }
    if (p < 1000.0f || p > 120000.0f) {
        mock_pres_rejects++;
        return;
    }
    pp_feed((int32_t)p, stamp_ms);
}

static void hist_push(uint32_t t, float pa) {
    unsigned last = (sm.hist_head + 255u) % 256u;
    if (sm.hist_n > 0 && sm.hist[last].t == t) {
        sm.hist[last].pa = pa;
        return;
    }
    sm.hist[sm.hist_head].t = t;
    sm.hist[sm.hist_head].pa = pa;
    sm.hist_head = (sm.hist_head + 1u) % 256u;
    if (sm.hist_n < 256u)
        sm.hist_n++;
}

/* The newest pressure at or before t. */
static float hist_at(uint32_t t) {
    float pa = mock_pressure.pressure_pa;
    for (unsigned i = 0; i < sm.hist_n; i++) {
        unsigned k = (sm.hist_head + 255u - i) % 256u;
        pa = sm.hist[k].pa;
        if ((int32_t)(t - sm.hist[k].t) >= 0)
            break;
    }
    return pa;
}

/* 1.3 stalls a second, each about 56 ms long, leaves about 713 ms between them. */
static uint32_t stall_gap_ms(void) {
    return (uint32_t)(-713.0f * logf(uniform01(&sm.stall_rng)));
}

bool mock_core0_stalled(uint32_t now_ms) {
    return mock_stall_model && (int32_t)(now_ms - sm.stall_until) < 0;
}

/* MS5607 at 50 Hz: D1 commanded, D2 commanded 10 ms later, D2 read 10 ms after
 * that. The reading is the pressure at the middle of D1's conversion and is
 * stamped with the loop time of the read, as hal_common.c stamps it. */
static void stall_model_tick(uint32_t now) {
    hist_push(now, mock_pressure.pressure_pa);
    if (!sm.stall_seeded) {
        sm.stall_rng = mock_stall_seed;
        sm.stall_seeded = true;
        sm.next_stall_at = now + stall_gap_ms();
    }
    if (mock_core0_stalled(now))
        return;
    if ((int32_t)(now - sm.next_stall_at) >= 0) {
        uint32_t len = 40u + xorshift32(&sm.stall_rng) % 34u;
        sm.stall_until = now + len;
        sm.next_stall_at = sm.stall_until + stall_gap_ms();
        mock_stall_count++;
        mock_stall_total_ms += len;
        if (mock_stall_min_ms == 0 || len < mock_stall_min_ms)
            mock_stall_min_ms = len;
        if (len > mock_stall_max_ms)
            mock_stall_max_ms = len;
        return;
    }
    switch (sm.phase) {
    case 0:
        sm.d1_ms = now;
        sm.due_ms = now + 10u;
        sm.phase = 1;
        break;
    case 1:
        if ((int32_t)(now - sm.due_ms) >= 0) {
            sm.due_ms = now + 10u;
            sm.phase = 2;
        }
        break;
    default:
        if ((int32_t)(now - sm.due_ms) >= 0) {
            uint32_t conv = sm.d1_ms + 4u;
            uint32_t lag = now - conv;
            if (mock_stamp_lag_min_ms == 0 || lag < mock_stamp_lag_min_ms)
                mock_stamp_lag_min_ms = lag;
            if (lag > mock_stamp_lag_max_ms)
                mock_stamp_lag_max_ms = lag;
            feed_reading(hist_at(conv), now);
            sm.phase = 0;
        }
        break;
    }
}

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
static uint32_t last_pp_feed_ms = 0;
uint32_t mock_fs_write_count = 0;

void mock_reset_all(void) {
    mock_fs_write_count = 0;
    memset(&sim_files, 0, sizeof(sim_files));
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
    /* Reset log state and streaming file handle so each test starts clean
     * regardless of whether the previous test reached LANDED. */
    test_log_active = false;
    test_log_file = NULL;
    test_file.open = false;
    memset(sim_files, 0, sizeof(sim_files));
    last_pp_feed_ms = 0;
    mock_sample_interval_ms = 20;
    mock_sensor_stuck = false;
    mock_noise_rms_pa = 0.0f;
    mock_noise_seed = 1;
    mock_glitch_pa = 0;
    mock_glitch_samples = 0;
    mock_stall_model = false;
    mock_stall_seed = 1;
    mock_stall_count = mock_stall_total_ms = mock_stall_min_ms = mock_stall_max_ms = 0;
    mock_stamp_lag_min_ms = mock_stamp_lag_max_ms = 0;
    mock_pres_rejects = 0;
    memset(&sm, 0, sizeof(sm));
    /* Every test starts with no pad claimed and both channels the flight
     * software's; a test that wants a release gives Lua the pads first. */
    pad_claim_reset();
    hal_pyro_init();
    hal_pyro_claim_channels(mock_pyro_pads);
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

/* Routed through pyro_release.c rather than around it, so the release
 * behaviour the hardware HAL gets is the behaviour these tests exercise. The
 * mocked table is the module's own; what follows is the "real" one. */
static void test_fire(uint8_t channel) {
    if (mock_pyro.refuse_fire) {
        mock_pyro.refused_count++;
        return;
    }
    mock_pyro.fire_count++;
    mock_pyro.last_fire_channel = channel;
    mock_pyro.firing = true;
}

static void test_get(uint8_t channel, hal_continuity_t *out) {
    if (channel == 1) {
        out->raw_adc = mock_pyro.p1_adc;
        out->good = mock_pyro.p1_good;
        out->open = mock_pyro.p1_open;
        out->shorted = false;
    } else if (channel == 2) {
        out->raw_adc = mock_pyro.p2_adc;
        out->good = mock_pyro.p2_good;
        out->open = mock_pyro.p2_open;
        out->shorted = false;
    }
}

static bool test_fault(uint8_t channel) {
    (void)channel;
    return mock_pyro.fault;
}

static const pyro_ch_ops_t test_pyro_ops = {test_fire, test_get, test_fault};

char mock_pyro_last_note[64];
int mock_pyro_notes;

static void test_report(uint8_t channel, const char *what) {
    mock_pyro_notes++;
    snprintf(mock_pyro_last_note, sizeof(mock_pyro_last_note), "pyro%u %s: released to Lua", (unsigned)channel, what);
}

void hal_pyro_init(void) {
    pyro_release_init(&test_pyro_ops, test_report);
    mock_pyro_notes = 0;
    mock_pyro_last_note[0] = '\0';
}

/* The host tests have no board capability table, so they say which pads a
 * channel switches: MK1A's numbering, its own element plus the common. The
 * point is that the claim decides, not the numbers. */
uint32_t mock_pyro_pads(uint8_t channel) {
    if (channel == 1)
        return PAD(9) | PAD(10);
    if (channel == 2)
        return PAD(11) | PAD(10);
    return PAD_NONE;
}

int hal_pyro_claim_channels(uint32_t (*pads_of)(uint8_t channel)) {
    return pyro_release_claim(pads_of);
}

void hal_pyro_sample(void) {
    if (pyro_release_all()) {
        return;
    }
    mock_pyro.sample_count++;
}

void hal_pyro_get(uint8_t channel, hal_continuity_t *out) {
    pyro_ch(channel)->get(channel, out);
}

void hal_pyro_fire(uint8_t channel) {
    pyro_ch(channel)->fire(channel);
}

void hal_pyro_update(uint32_t now_ms) {
    (void)now_ms;
}
/* Host tests drive the recovery matrix through brownout_assess() directly;
 * this only has to exist and be settable. */
reset_cause_t mock_reset_cause = RESET_POWER_EVENT;
reset_cause_t hal_reset_cause(void) {
    return mock_reset_cause;
}

bool hal_pyro_is_firing(void) {
    return mock_pyro.firing;
}
bool hal_pyro_fault(uint8_t channel) {
    return pyro_ch(channel)->fault(channel);
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

/* The host tests have a real filesystem behind them, so the mount cannot fail. Present because
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
    return -2;
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    xip_stall(); /* simulate flash erase+write XIP stall */
    mock_fs_write_count++;
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
    /* Feed pressure samples at ~50Hz (every 20ms) into pressure_processing.
     * Matches real BMP280/MS5607 sample rate. The pp ring (32 entries)
     * stays shallow when detectors dispatch at ≥10ms intervals. */
    if (mock_stall_model) {
        if (mock_pressure.sensor_type > 0)
            stall_model_tick(now_ms);
        if (mock_core0_stalled(now_ms))
            return;
    } else if (mock_pressure.sensor_type > 0 &&
               (last_pp_feed_ms == 0 || (now_ms - last_pp_feed_ms) >= mock_sample_interval_ms)) {
        feed_reading(mock_pressure.pressure_pa, now_ms);
        last_pp_feed_ms = now_ms;
    }
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

/* ── In-flight data logging [v2-9] ───────────────────────────────── */

void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa) {
    if (test_log_active)
        return;
    test_log_file = hal_fs_open("flight_log.csv", false);
    if (!test_log_file)
        return;
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "# " PYRO_BOARD_NAME " Flight Data\n# ID: %.8s\n# Name: %.8s\n"
                     "# Pyro1: %s %u\n# Pyro2: %s %u\n"
                     "# Units: %s\n# Ground Pa: %ld\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
                     cfg->id, cfg->name, config_mode_name(cfg->pyro1_mode), cfg->pyro1_value,
                     config_mode_name(cfg->pyro2_mode), cfg->pyro2_value,
                     cfg->units == 2   ? "ft"
                     : cfg->units == 1 ? "m"
                                       : "cm",
                     (long)ground_pressure_pa);
    hal_fs_write(test_log_file, hdr, n);
    test_log_active = true;
}

void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event) {
    if (!test_log_active || !test_log_file)
        return;
    char line[80];
    int n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)time_ms, (long)pressure_pa,
                     (long)altitude_cm, state, under_thrust, flight_event_name(event));
    hal_fs_write(test_log_file, line, n);
}

void hal_log_stop(void) {
    if (!test_log_active)
        return;
    if (test_log_file) {
        hal_fs_close(test_log_file);
        test_log_file = NULL;
    }
    test_log_active = false;
}

bool hal_log_active(void) {
    return test_log_active;
}
