/*
 * Buzzer driver: GPIO 16 on/off with non-blocking beep code sequencer.
 *
 * Startup: 10 fast chirps, pause, then status code played twice, then stop.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UNIT_TEST
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#else
#include "mocks.h"
#endif

#include "buzzer.h"

#define BUZZER_PIN 16

#define CHIRP_ON_MS    30
#define CHIRP_GAP_MS   30
#define CHIRP_COUNT    10
#define STARTUP_PAUSE_MS 500

#define BEEP_ON_MS    100
#define BEEP_GAP_MS   200
#define DIGIT_GAP_MS  300
#define CODE_GAP_MS   500

typedef enum {
    BZ_IDLE,
    BZ_CHIRP_ON,
    BZ_CHIRP_GAP,
    BZ_STARTUP_PAUSE,
    BZ_BEEP_ON,
    BZ_BEEP_GAP,
    BZ_DIGIT_GAP,
    BZ_CODE_GAP,
} buzzer_phase_t;

static buzzer_phase_t phase = BZ_IDLE;
static uint8_t current_code = 0;
static uint32_t phase_start = 0;
static uint8_t current_digit = 0;
static uint8_t beeps_remaining = 0;
static uint8_t chirps_remaining = 0;
static uint8_t repeats_remaining = 0;

void buzzer_init(void) {
#ifndef UNIT_TEST
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);
#endif
}

void buzzer_tone_on(void) {
#ifndef UNIT_TEST
    gpio_put(BUZZER_PIN, 1);
#endif
}

void buzzer_tone_off(void) {
#ifndef UNIT_TEST
    gpio_put(BUZZER_PIN, 0);
#endif
}

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

void buzzer_stop(void) {
    buzzer_tone_off();
    phase = BZ_IDLE;
    current_code = 0;
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
            buzzer_tone_off();
            chirps_remaining--;
            phase = (chirps_remaining > 0) ? BZ_CHIRP_GAP : BZ_STARTUP_PAUSE;
            phase_start = now_ms;
        }
        break;

    case BZ_CHIRP_GAP:
        if (elapsed >= CHIRP_GAP_MS) {
            buzzer_tone_on();
            phase = BZ_CHIRP_ON;
            phase_start = now_ms;
        }
        break;

    case BZ_STARTUP_PAUSE:
        if (elapsed >= STARTUP_PAUSE_MS) {
            start_code_play(now_ms);
        }
        break;

    case BZ_BEEP_ON:
        if (elapsed >= BEEP_ON_MS) {
            buzzer_tone_off();
            beeps_remaining--;
            if (beeps_remaining > 0) {
                phase = BZ_BEEP_GAP;
            } else if (current_digit == 0) {
                phase = BZ_DIGIT_GAP;
            } else if (repeats_remaining > 0) {
                repeats_remaining--;
                phase = BZ_CODE_GAP;
            } else {
                phase = BZ_IDLE;
            }
            phase_start = now_ms;
        }
        break;

    case BZ_BEEP_GAP:
        if (elapsed >= BEEP_GAP_MS) {
            buzzer_tone_on();
            phase = BZ_BEEP_ON;
            phase_start = now_ms;
        }
        break;

    case BZ_DIGIT_GAP:
        if (elapsed >= DIGIT_GAP_MS) {
            current_digit = 1;
            beeps_remaining = BEEP_DIGIT2(current_code);
            buzzer_tone_on();
            phase = BZ_BEEP_ON;
            phase_start = now_ms;
        }
        break;

    case BZ_CODE_GAP:
        if (elapsed >= CODE_GAP_MS) {
            start_code_play(now_ms);
        }
        break;

    default:
        phase = BZ_IDLE;
        break;
    }
}
