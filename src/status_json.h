/*
 * /api/status, in two halves: a snapshot taken at one instant of the flight,
 * and status_json(), which renders nothing but the snapshot [WEB-API-11].
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

/* The longest rendering, with every field at its widest.
 *
 * test_SJ_02 measures it and fails if it grows past this, which is how the
 * board_id and board_selftest keys were caught: the bound has to move
 * deliberately, and it has to stay under HTTP_WORK_SIZE, which the
 * _Static_assert in http_server.c checks. */
#define STATUS_JSON_MAX 3500

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
    const char *log_rate; /* 1hz, events or full */
    char rocket_id[9], rocket_name[9];
    const char *sensor, *board;
    /* board is the display name ("Pyro MK1A"); board_id is the short token
     * ("mk1a") that names things -- release assets, image descriptors, build
     * directories. A consumer choosing a firmware image must key off the
     * token, never off the display string. */
    const char *board_id;
    /* The board self-test, as a code and not a string: it has exactly three
     * values, and a char* here would let the widest-rendering bound below
     * assume an arbitrary one.
     *   0 unknown -- no stamp yet, nothing to compare
     *   1 pass    -- the stamp agrees with the compiled-in board
     *   2 fail    -- the stamp names another board; this image is not
     *                committing and will roll back */
    uint8_t board_selftest;
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
    const char *resume;
    /* How the last boot ended: the watchdog, and the stage or crumb core0
     * was in (src/main_hardware.c has the map); -1: none stamped. */
    bool prev_watchdog;
    int32_t prev_stage;
    uint32_t prev_stage_ms;
    uint16_t pyro_pulses[2]; /* [PYR-FIRE-01] every pulse, the first included */
    bool pyro_fault[2];
    bool emergency_fire;                      /* [FLT-EMRG-04] */
    uint16_t refire_interval_ms, fire_gap_ms; /* in force [PYR-BOARD-03] */
    bool pyro_limited;                        /* a configured value was outside the board's range [PYR-BOARD-02] */
    /* The pressure collector [SNS-COL-04, SNS-COL-05]: transfers that failed, in
     * all and by cause, bus recoveries, and cycles pushed out of its queue. */
    uint32_t pres_rejects, pres_bus[4], pres_recoveries, pres_dropped;
    uint32_t pres_flashed; /* [DD-068] readings a flash operation disturbed, not fed on */
    int32_t raw_pa, pad_speed_cms;
    bool ground_degraded;
    uint32_t ground_reseeds;
    uint32_t sample_interval_us[2], stamp_lag_max_us, noise_mpa;
    const char *estimator; /* the one obeyed [SNS-EST-06] */
    bool estimator_explains;
    bool peak_lower_bound;
    bool usb_attached, test_mode, buzzer_active;

    const char *beep;      /* the outcome's key */
    const char *beep_kind; /* how it sounds: a kind, or a code of d1[-d2] beeps */
    bool beep_is_code;
    uint8_t beep_d1, beep_d2;

    char serial[16];
    bool serial_assigned;
    char mac_source[12]; /* "rng" or "assigned" [DD-072] */
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
