/*
 * Buzzer driver: 3kHz PWM on GPIO 16 with non-blocking beep code sequencer.
 *
 * Timing:
 *   100ms beep, 200ms pause within digit
 *   300ms pause between digits
 *   500ms pause between codes
 *   1000ms pause before repeat
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UNIT_TEST
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#else
#include "mocks.h"
#endif

#include "buzzer.h"

#define BUZZER_PIN 16
#define BUZZER_FREQ 3000

#define BEEP_ON_MS    100
#define BEEP_GAP_MS   200
#define DIGIT_GAP_MS  300
#define CODE_GAP_MS   500
#define REPEAT_GAP_MS 1000

typedef enum {
    BZ_IDLE,
    BZ_BEEP_ON,
    BZ_BEEP_GAP,
    BZ_DIGIT_GAP,
    BZ_CODE_GAP,
    BZ_REPEAT_GAP,
} buzzer_phase_t;

static buzzer_phase_t phase = BZ_IDLE;
static uint8_t current_code = 0;
static bool repeating = false;
static uint32_t phase_start = 0;
static uint8_t current_digit = 0;   /* 0 = first digit, 1 = second */
static uint8_t beeps_remaining = 0;

#ifndef UNIT_TEST
static uint pwm_slice;

void buzzer_init(void) {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    pwm_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    uint32_t clk = clock_get_hz(clk_sys);
    uint32_t wrap = clk / BUZZER_FREQ - 1;
    pwm_set_wrap(pwm_slice, wrap);
    pwm_set_gpio_level(BUZZER_PIN, 0);
    pwm_set_enabled(pwm_slice, true);
}

void buzzer_tone_on(void) {
    uint32_t clk = clock_get_hz(clk_sys);
    uint32_t wrap = clk / BUZZER_FREQ - 1;
    pwm_set_gpio_level(BUZZER_PIN, wrap / 2);  /* 50% duty */
}

void buzzer_tone_off(void) {
    pwm_set_gpio_level(BUZZER_PIN, 0);
}
#else
void buzzer_init(void) {}
void buzzer_tone_on(void) {}
void buzzer_tone_off(void) {}
#endif

void buzzer_set_code(uint8_t code, bool repeat) {
    current_code = code;
    repeating = repeat;
    current_digit = 0;
    beeps_remaining = BEEP_DIGIT1(code);
    phase = BZ_BEEP_ON;
    phase_start = 0;  /* will be set on next update */
    buzzer_tone_on();
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
    case BZ_BEEP_ON:
        if (elapsed >= BEEP_ON_MS) {
            buzzer_tone_off();
            beeps_remaining--;
            if (beeps_remaining > 0) {
                phase = BZ_BEEP_GAP;
            } else if (current_digit == 0) {
                phase = BZ_DIGIT_GAP;
            } else {
                phase = repeating ? BZ_REPEAT_GAP : BZ_CODE_GAP;
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
            phase = BZ_IDLE;
        }
        break;

    case BZ_REPEAT_GAP:
        if (elapsed >= REPEAT_GAP_MS) {
            current_digit = 0;
            beeps_remaining = BEEP_DIGIT1(current_code);
            buzzer_tone_on();
            phase = BZ_BEEP_ON;
            phase_start = now_ms;
        }
        break;

    default:
        phase = BZ_IDLE;
        break;
    }
}
