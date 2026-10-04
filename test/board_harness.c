/*
 * See board_harness.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_harness.h"
#include "unity.h"
#include "mocks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/beep_store.h"
#include "../src/board_selftest.h"
#include "../src/buzzer.h"
#include "../src/flight_events.h"
#include "../src/flight_resume.h"
#include "../src/hal.h"
#include "../src/pad_claim.h"
#include "../src/pressure_estimator.h"

/* ── The buzzer, as asked ─────────────────────────────────────────── */

int harness_spec_plays, harness_usb_ok_plays, harness_altitude_plays, harness_stops;
beep_spec_t harness_last_spec;
int32_t harness_last_altitude;
gt_sound_t harness_gt_sounds[32];
int harness_gt_sound_n;

void buzzer_init(void) {}
void buzzer_play_spec(const beep_spec_t *spec, uint16_t gap_ms, uint8_t repeat_count) {
    (void)gap_ms;
    (void)repeat_count;
    harness_last_spec = *spec;
    harness_spec_plays++;
}
void buzzer_play_altitude(int32_t altitude) {
    harness_last_altitude = altitude;
    harness_altitude_plays++;
}
void buzzer_play_usb_ok(void) {
    harness_usb_ok_plays++;
}
void buzzer_play_ground_test(gt_sound_t sound) {
    if (harness_gt_sound_n < 32)
        harness_gt_sounds[harness_gt_sound_n++] = sound;
}
void buzzer_stop(void) {
    harness_stops++;
}
bool buzzer_is_active(void) {
    return false;
}

bool harness_last_said(beep_reason_t reason) {
    beep_spec_t want = beep_for(reason);
    return harness_spec_plays > 0 && want.kind == harness_last_spec.kind && want.d1 == harness_last_spec.d1 &&
           want.d2 == harness_last_spec.d2;
}

/* ── Power and the loop ───────────────────────────────────────────── */

flight_context_t ctx;
uint32_t (*loop_lag_ms)(uint32_t t);
static bool powered, usb_at_power_on;

void boot_like_hardware(uint32_t seed) {
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    powered = false;
    usb_at_power_on = false;
    harness_spec_plays = harness_usb_ok_plays = harness_altitude_plays = harness_gt_sound_n = harness_stops = 0;
    memset(&harness_last_spec, 0, sizeof(harness_last_spec));
    loop_lag_ms = NULL;
    mock_pressure.pressure_pa = 101325.0f;
    mock_noise_rms_pa = SENSOR_RMS_PA;
    mock_noise_seed = seed;
    mock_stall_seed = seed * 7919u + 1u;
}

void harness_config(const char *ini) {
    TEST_ASSERT_EQUAL(0, hal_fs_write_file("config.ini", ini, (int)strlen(ini)));
}

void harness_board_stamp(const char *board) {
    TEST_ASSERT_EQUAL(0, hal_fs_write_file("board.txt", board, (int)strlen(board)));
}

void harness_give_to_script(uint8_t channel) {
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(mock_pyro_pads(channel) & ~mock_pyro_pads((uint8_t)(3 - channel)), PAD_LUA));
    hal_pyro_init();
}

void harness_usb(bool attached) {
    if (powered)
        flight_set_usb_attached(&ctx, attached, mock_time_ms);
    else
        usb_at_power_on = attached;
}

/* A follower of the flight's events starts with the board. */
static uint32_t followed;

static void power_on(void) {
    board_selftest_init();
    followed = 0;
    flight_init(&ctx);
    hal_pyro_claim_channels(mock_pyro_pads);
    flight_set_usb_attached(&ctx, usb_at_power_on, mock_time_ms);
    powered = true;
}

void harness_restart(reset_cause_t cause) {
    mock_power_cycle();
    mock_reset_cause = cause;
    memset(&ctx, 0, sizeof(ctx));
    powered = false;
}

/* As the script's host follows them on a board. */
static void follow_events(void) {
    uint8_t event;
    while (flight_next_event(&ctx, &followed, &event)) {
        size_t n = strlen(mock_lua_events);
        snprintf(mock_lua_events + n, sizeof(mock_lua_events) - n, "%s;", flight_event_name(event));
    }
}

void tick(uint32_t t) {
    mock_time_ms = t;
    if (!powered)
        power_on();
    hal_tasks_tick(t);
    if (mock_core0_stalled(t))
        return;
    uint32_t now = t + (loop_lag_ms ? loop_lag_ms(t) : 0u);
    ctx.current_state = dispatch_state(&ctx, now);
    flight_update_outputs(&ctx, now);
    flight_storage_service(&ctx, now);
    follow_events();
}

uint32_t run_to_pad(uint32_t *t) {
    for (uint32_t limit = *t + 20000u; *t < limit; (*t)++) {
        tick(*t);
        if (ctx.current_state == PAD_IDLE)
            return *t;
    }
    TEST_FAIL_MESSAGE("never reached PAD_IDLE");
    return 0;
}

bool booting(void) {
    return ctx.current_state == BOOT_SETTLE || ctx.current_state == BOOT_SENSOR;
}

/* ── The pad record ───────────────────────────────────────────────── */

void write_pad_record(int32_t ground_pa) {
    pad_record_t r;
    pad_record_fill(&r, ground_pa, (uint32_t)(PEST_NOISE_FLOOR_PA * 1000.0f));
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_RECORD_PATH, (const char *)&r, (int)sizeof(r)));
}

bool pad_record_stored(void) {
    pad_record_t r;
    int n = mock_fs_peek(PAD_RECORD_PATH, (char *)&r, (int)sizeof(r));
    return n == (int)sizeof(r) && pad_record_valid(&r);
}

/* ── The flight log ───────────────────────────────────────────────── */

static char log_csv[262144];

static int scan_log(const char *event, float *first_s) {
    int n = test_flight_log_csv(log_csv, (int)sizeof(log_csv));
    int count = 0;
    *first_s = -1.0f;
    for (char *line = log_csv; line < log_csv + n;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *last_comma = NULL;
        for (size_t i = 0; i < len; i++)
            if (line[i] == ',')
                last_comma = line + i;
        size_t ev_len = strlen(event);
        if (last_comma && line[0] != '#' && (size_t)(line + len - last_comma - 1) >= ev_len &&
            strncmp(last_comma + 1, event, ev_len) == 0 &&
            (last_comma[1 + ev_len] == '\n' || last_comma[1 + ev_len] == '\r' || last_comma[1 + ev_len] == '\0')) {
            if (count == 0)
                *first_s = (float)atol(line) / 1000.0f;
            count++;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return count;
}

int harness_log_events(const char *event) {
    float t;
    return scan_log(event, &t);
}

float harness_log_event_time(const char *event) {
    float t;
    scan_log(event, &t);
    return t;
}
