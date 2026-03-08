/*
 * Buzzer driver: non-blocking beep sequencer.
 *
 * Two modes:
 *   1. Status code: 10 chirps → pause → code × 2 → stop
 *   2. Altitude beep-out: long pause → long beep → digits → repeat forever
 *
 * Uses hal_buzzer_tone_on/off() for hardware abstraction.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "buzzer.h"

/* Status code timing */
#define CHIRP_ON_MS       30
#define CHIRP_GAP_MS      30
#define CHIRP_COUNT       10
#define STARTUP_PAUSE_MS  500
#define BEEP_ON_MS        100
#define BEEP_GAP_MS       200
#define DIGIT_GAP_MS      300
#define CODE_GAP_MS       500

/* Altitude beep-out timing */
#define ALT_LONG_PAUSE_MS 2000
#define ALT_LONG_BEEP_MS  500
#define ALT_SHORT_PAUSE_MS 300
#define ALT_BEEP_ON_MS    100
#define ALT_BEEP_GAP_MS   200
#define ALT_DIGIT_GAP_MS  400

typedef enum {
    BZ_IDLE,
    BZ_CHIRP_ON, BZ_CHIRP_GAP, BZ_STARTUP_PAUSE,
    BZ_BEEP_ON, BZ_BEEP_GAP, BZ_DIGIT_GAP, BZ_CODE_GAP,
    BZ_ALT_LONG_PAUSE, BZ_ALT_LONG_BEEP, BZ_ALT_SHORT_PAUSE,
    BZ_ALT_BEEP_ON, BZ_ALT_BEEP_GAP, BZ_ALT_DIGIT_GAP,
} buzzer_phase_t;

static buzzer_phase_t phase = BZ_IDLE;
static uint32_t phase_start = 0;

static uint8_t current_code = 0;
static uint8_t current_digit = 0;
static uint8_t beeps_remaining = 0;
static uint8_t chirps_remaining = 0;
static uint8_t repeats_remaining = 0;

static uint8_t alt_digits[6];
static uint8_t alt_num_digits = 0;
static uint8_t alt_digit_idx = 0;
static uint8_t alt_beeps_remaining = 0;

void buzzer_init(void) {
    hal_buzzer_init();
}

void buzzer_tone_on(void)  { hal_buzzer_tone_on(); }
void buzzer_tone_off(void) { hal_buzzer_tone_off(); }

/* ── Status code mode ─────────────────────────────────────────────── */

static void start_code_play(uint32_t now_ms) {
    current_digit = 0;
    beeps_remaining = BEEP_DIGIT1(current_code);
    buzzer_tone_on();
    phase = BZ_BEEP_ON;
    phase_start = now_ms;
}

void buzzer_set_code(uint8_t code, bool repeat) {
    current_code = code;
    repeats_remaining = repeat ? 1 : 0;
    chirps_remaining = CHIRP_COUNT;
    buzzer_tone_on();
    phase = BZ_CHIRP_ON;
    phase_start = 0;
}

/* ── Altitude beep-out mode ───────────────────────────────────────── */

void buzzer_set_altitude(int32_t altitude) {
    if (altitude < 0) altitude = -altitude;
    if (altitude == 0) {
        alt_digits[0] = 0;
        alt_num_digits = 1;
    } else {
        uint8_t tmp[6];
        int n = 0;
        int32_t v = altitude;
        while (v > 0 && n < 6) { tmp[n++] = v % 10; v /= 10; }
        alt_num_digits = n;
        for (int i = 0; i < n; i++) alt_digits[i] = tmp[n - 1 - i];
    }
    alt_digit_idx = 0;
    phase = BZ_ALT_LONG_PAUSE;
    phase_start = 0;
}

static void start_alt_digit(uint32_t now_ms) {
    uint8_t d = alt_digits[alt_digit_idx];
    alt_beeps_remaining = (d == 0) ? 10 : d;
    buzzer_tone_on();
    phase = BZ_ALT_BEEP_ON;
    phase_start = now_ms;
}

/* ── Common ───────────────────────────────────────────────────────── */

void buzzer_stop(void) {
    buzzer_tone_off();
    phase = BZ_IDLE;
}

bool buzzer_is_active(void) {
    return phase != BZ_IDLE;
}

void buzzer_update(uint32_t now_ms) {
    if (phase == BZ_IDLE) return;
    if (phase_start == 0) phase_start = now_ms;
    uint32_t elapsed = now_ms - phase_start;

    switch (phase) {
    case BZ_CHIRP_ON:
        if (elapsed >= CHIRP_ON_MS) {
            buzzer_tone_off(); chirps_remaining--;
            phase = (chirps_remaining > 0) ? BZ_CHIRP_GAP : BZ_STARTUP_PAUSE;
            phase_start = now_ms;
        } break;
    case BZ_CHIRP_GAP:
        if (elapsed >= CHIRP_GAP_MS) { buzzer_tone_on(); phase = BZ_CHIRP_ON; phase_start = now_ms; }
        break;
    case BZ_STARTUP_PAUSE:
        if (elapsed >= STARTUP_PAUSE_MS) start_code_play(now_ms);
        break;
    case BZ_BEEP_ON:
        if (elapsed >= BEEP_ON_MS) {
            buzzer_tone_off(); beeps_remaining--;
            if (beeps_remaining > 0) phase = BZ_BEEP_GAP;
            else if (current_digit == 0) phase = BZ_DIGIT_GAP;
            else if (repeats_remaining > 0) { repeats_remaining--; phase = BZ_CODE_GAP; }
            else phase = BZ_IDLE;
            phase_start = now_ms;
        } break;
    case BZ_BEEP_GAP:
        if (elapsed >= BEEP_GAP_MS) { buzzer_tone_on(); phase = BZ_BEEP_ON; phase_start = now_ms; }
        break;
    case BZ_DIGIT_GAP:
        if (elapsed >= DIGIT_GAP_MS) {
            current_digit = 1; beeps_remaining = BEEP_DIGIT2(current_code);
            buzzer_tone_on(); phase = BZ_BEEP_ON; phase_start = now_ms;
        } break;
    case BZ_CODE_GAP:
        if (elapsed >= CODE_GAP_MS) start_code_play(now_ms);
        break;
    case BZ_ALT_LONG_PAUSE:
        if (elapsed >= ALT_LONG_PAUSE_MS) { buzzer_tone_on(); phase = BZ_ALT_LONG_BEEP; phase_start = now_ms; }
        break;
    case BZ_ALT_LONG_BEEP:
        if (elapsed >= ALT_LONG_BEEP_MS) { buzzer_tone_off(); alt_digit_idx = 0; phase = BZ_ALT_SHORT_PAUSE; phase_start = now_ms; }
        break;
    case BZ_ALT_SHORT_PAUSE:
        if (elapsed >= ALT_SHORT_PAUSE_MS) start_alt_digit(now_ms);
        break;
    case BZ_ALT_BEEP_ON:
        if (elapsed >= ALT_BEEP_ON_MS) {
            buzzer_tone_off(); alt_beeps_remaining--;
            if (alt_beeps_remaining > 0) phase = BZ_ALT_BEEP_GAP;
            else { alt_digit_idx++; phase = (alt_digit_idx < alt_num_digits) ? BZ_ALT_DIGIT_GAP : BZ_ALT_LONG_PAUSE; }
            phase_start = now_ms;
        } break;
    case BZ_ALT_BEEP_GAP:
        if (elapsed >= ALT_BEEP_GAP_MS) { buzzer_tone_on(); phase = BZ_ALT_BEEP_ON; phase_start = now_ms; }
        break;
    case BZ_ALT_DIGIT_GAP:
        if (elapsed >= ALT_DIGIT_GAP_MS) start_alt_digit(now_ms);
        break;
    default: phase = BZ_IDLE; break;
    }
}
