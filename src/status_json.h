/*
 * /api/status, in two halves: core0 captures a snapshot in one pass, and
 * status_json() renders it anywhere. The renderer reads nothing but the
 * snapshot, which is what lets core1 run it (DD-061).
 *
 * Every string pointer names a constant; anything that can change is copied.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATUS_STAGES 9
#define STATUS_STAGE1_PARTS 4
#define STATUS_FAULTS_MAX 16

/* The longest rendering, with every field at its widest. */
#define STATUS_JSON_MAX 3072

typedef struct {
    const char *state;
    int32_t alt_cm, max_alt_cm, vspeed_cms, pressure_pa;
    bool pyro_cont[2];
    uint16_t pyro_adc[2];
    bool pyro_fired[2];
    bool armed;
    uint32_t flight_ms, uptime_ms;
    const char *fw_version;
    const char *pyro_mode[2];
    uint16_t pyro_value[2];
    uint8_t units;
    bool log_high_rate;
    char rocket_id[9], rocket_name[9];
    const char *sensor, *board;
    int32_t pyro_bus_q, pyro_bus_adc, pyro_vbat_adc; /* -1: none on this board */

    uint32_t loop_max_us, loop_overruns, loop_late_max_us, loop_count;
    uint32_t stage_max_us[STATUS_STAGES];
    uint32_t stage1_parts_us[STATUS_STAGE1_PARTS]; /* TinyUSB, lwIP, HTTP transport, mDNS */
    uint32_t http_units[2], http_unit_max_us[2];   /* core0, core1 */
    uint32_t flash_opens, flash_skips, flash_refusals, log_dropped;
    uint32_t flash_erases, flash_programs, flash_deferrals;

    char pins_reason[96];
    bool pyro_released[2];
    bool bridge;
    uint8_t bridge_ch, bridge_common;
    uint32_t pyro_mocked;
    bool pyro_real[2];
    bool sensor_ok, fs_ok;
    const char *faults[STATUS_FAULTS_MAX];
    uint8_t n_faults;
    uint8_t reset_cause;
    const char *recovery;
    /* How the last boot ended: the watchdog, and the stage or crumb core0
     * was in (src/main_hardware.c has the map); -1: none stamped. */
    bool prev_watchdog;
    int32_t prev_stage;
    uint32_t prev_stage_ms;
    bool pyro_refused[2];
    uint8_t pyro1_refires;
    bool main_forced;
    uint32_t pres_waits, pres_rejects;
    int32_t raw_pa, pad_speed_cms;
    bool ground_degraded;
    uint32_t ground_reseeds;
    uint32_t sample_interval_us[2], stamp_lag_max_us, fit_sigma_mpa;
    bool mach_lock;
    uint32_t mach_flag_ms;
    bool peak_lower_bound;
    bool usb_attached, test_mode, buzzer_active;

    const char *beep;      /* the outcome's key */
    const char *beep_kind; /* how it sounds: a kind, or a code of d1[-d2] beeps */
    bool beep_is_code;
    uint8_t beep_d1, beep_d2;

    char serial[16];
    bool serial_assigned;
    char hw_id[17];
    uint8_t subnet;
} status_snap_t;

/* Returns the length written, NUL-terminated, or -1 when it does not fit:
 * truncated JSON parses as nothing. */
int status_json(const status_snap_t *s, char *buf, size_t cap);

/* JSON string escaping for status_json and the server's other renderers:
 * quote, backslash and newline, dropping the rest of the control range.
 * Truncates rather than overflowing. */
void json_escape(char *out, int out_sz, const char *in, int len);

#endif
