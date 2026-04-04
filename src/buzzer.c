/*
 * Buzzer driver — autonomous async task (v2).
 *
 * Architecture: one async_task_t handles both encoding and playback.
 *
 *   Flight software calls buzzer_play_code() or buzzer_play_altitude().
 *   That stores the request and arms the task for immediate execution.
 *
 *   On the first tick the task encodes the request into a flat
 *   buzzer_pattern_t[] array (BZ_ENCODE phase) and immediately
 *   transitions to BZ_PLAYING.
 *
 *   Subsequent ticks step through the pattern, toggling the GPIO via
 *   hal_buzzer_tone_on/off() on the exact schedule, with no main-loop
 *   involvement.
 *
 *   The task is registered with the platform task runner via
 *   hal_buzzer_task_register() so it runs alongside the pressure task.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "buzzer.h"
#include "async_task.h"
#include <string.h>

/* ── Timing constants ─────────────────────────────────────────────── */

/* Status code timing */
#define CHIRP_ON_MS 30
#define CHIRP_GAP_MS 30
#define CHIRP_COUNT 10
#define STARTUP_PAUSE_MS 500
#define BEEP_ON_MS 100
#define BEEP_GAP_MS 200
#define DIGIT_GAP_MS 300
#define CODE_GAP_MS 500

/* Altitude beep-out timing */
#define ALT_LONG_PAUSE_MS 2000
#define ALT_LONG_BEEP_MS 500
#define ALT_SHORT_PAUSE_MS 300
#define ALT_BEEP_ON_MS 100
#define ALT_BEEP_GAP_MS 200
#define ALT_DIGIT_GAP_MS 400

/* ── Pattern builder helpers ──────────────────────────────────────── */

/*
 * Append a single step to the pattern buffer.
 * Returns the new write index, or -1 if the buffer is full.
 */
static int pat_append(buzzer_pattern_t *buf, int idx, uint16_t dur, bool on) {
    if (idx >= BUZZER_MAX_PATTERN - 1)
        return idx; /* silently truncate — pattern is too long */
    buf[idx].duration_ms = dur;
    buf[idx].tone_on = on;
    return idx + 1;
}

/*
 * Append N on/off beep pairs separated by GAP_MS, followed by a
 * final TAIL_MS pause (used for digit gaps or end-of-code gaps).
 */
static int pat_append_beeps(buzzer_pattern_t *buf, int idx, int count, uint16_t on_ms, uint16_t gap_ms) {
    for (int i = 0; i < count; i++) {
        idx = pat_append(buf, idx, on_ms, true);
        if (i < count - 1)
            idx = pat_append(buf, idx, gap_ms, false);
    }
    return idx;
}

/*
 * Build the status-code pattern into buf[].
 * Layout:
 *   10x chirp on/off → startup pause  ← played only on the FIRST pass
 *   → digit1 beeps → digit gap        ← loop_start points here
 *   → digit2 beeps → code gap
 *   → zero-duration sentinel (loop/stop decision is in the task handler)
 *
 * *p_loop_start is set to the index of the first digit step so that
 * subsequent passes skip the startup chirps (fixing BUZ-01: chirps once).
 */
static int build_code_pattern(uint8_t code, buzzer_pattern_t *buf, int *p_loop_start) {
    int idx = 0;

    /* 10 startup chirps — played once; subsequent loops restart after here */
    for (int i = 0; i < CHIRP_COUNT; i++) {
        idx = pat_append(buf, idx, CHIRP_ON_MS, true);
        idx = pat_append(buf, idx, CHIRP_GAP_MS, false);
    }

    /* Startup pause */
    idx = pat_append(buf, idx, STARTUP_PAUSE_MS, false);

    /* Record where the digit section starts — this is the loop restart point */
    *p_loop_start = idx;

    /* Digit 1 */
    int d1 = BEEP_DIGIT1(code);
    if (d1 < 1)
        d1 = 1;
    idx = pat_append_beeps(buf, idx, d1, BEEP_ON_MS, BEEP_GAP_MS);

    /* Inter-digit gap */
    idx = pat_append(buf, idx, DIGIT_GAP_MS, false);

    /* Digit 2 */
    int d2 = BEEP_DIGIT2(code);
    if (d2 < 1)
        d2 = 1;
    idx = pat_append_beeps(buf, idx, d2, BEEP_ON_MS, BEEP_GAP_MS);

    /* End-of-code gap, then zero-duration sentinel */
    idx = pat_append(buf, idx, CODE_GAP_MS, false);
    buf[idx].duration_ms = 0;
    buf[idx].tone_on = false;
    idx++;
    return idx;
}

/*
 * Build the altitude beep-out pattern into buf[].
 * Layout:
 *   long pause → long beep → short pause
 *   → for each digit: digit beeps → digit gap
 *   → loop (zero-duration sentinel with repeat=true implied)
 */
static int build_alt_pattern(int32_t value, buzzer_pattern_t *buf) {
    /* Extract decimal digits, most-significant first */
    if (value < 0)
        value = -value;

    uint8_t digits[6];
    int num_digits = 0;

    if (value == 0) {
        digits[0] = 0;
        num_digits = 1;
    } else {
        uint8_t tmp[6];
        int32_t v = value;
        while (v > 0 && num_digits < 6) {
            tmp[num_digits++] = (uint8_t)(v % 10);
            v /= 10;
        }
        /* Reverse to get MSD first */
        for (int i = 0; i < num_digits; i++)
            digits[i] = tmp[num_digits - 1 - i];
    }

    int idx = 0;

    /* Long pause → long header beep → short pause */
    idx = pat_append(buf, idx, ALT_LONG_PAUSE_MS, false);
    idx = pat_append(buf, idx, ALT_LONG_BEEP_MS, true);
    idx = pat_append(buf, idx, ALT_SHORT_PAUSE_MS, false);

    /* Each digit: a digit value of 0 beeps 10 times */
    for (int d = 0; d < num_digits; d++) {
        int count = (digits[d] == 0) ? 10 : digits[d];
        idx = pat_append_beeps(buf, idx, count, ALT_BEEP_ON_MS, ALT_BEEP_GAP_MS);
        idx = pat_append(buf, idx, ALT_DIGIT_GAP_MS, false);
    }

    /* Loop sentinel (zero duration) — always repeats */
    buf[idx].duration_ms = 0;
    buf[idx].tone_on = false;
    idx++;
    return idx;
}

/* ── Async task ───────────────────────────────────────────────────── */

typedef enum {
    BZ_IDLE,
    BZ_ENCODE_CODE,
    BZ_ENCODE_ALT,
    BZ_PLAYING,
} bz_state_t;

typedef struct {
    async_task_t base; /* MUST be first */
    bz_state_t state;

    /* Request fields — set by buzzer_play_code/altitude before arming */
    uint8_t req_code;
    int32_t req_altitude;
    uint8_t req_repeat_count; /* 0=infinite, N=play N times */

    /* Encoded pattern */
    buzzer_pattern_t pattern[BUZZER_MAX_PATTERN];
    int pattern_len;
    int index;
    int loop_start;       /* index to restart from when looping (after chirps) */
    uint8_t repeat_count; /* 0=infinite, N=play N times */
    uint8_t loops_done;   /* complete passes so far */
} buzzer_task_t;

static buzzer_task_t bz;

/* ── Task tick function ───────────────────────────────────────────── */

static void buzzer_tick(async_task_t *self, uint32_t now_ms) {
    buzzer_task_t *t = (buzzer_task_t *)self;

    switch (t->state) {
    case BZ_IDLE:
        /* Nothing to do — tick should not fire, but be safe */
        t->base.tick = NULL;
        return;

    case BZ_ENCODE_CODE:
        t->pattern_len = build_code_pattern(t->req_code, t->pattern, &t->loop_start);
        t->repeat_count = t->req_repeat_count;
        t->loops_done = 0;
        t->index = 0;
        t->state = BZ_PLAYING;
        t->base.next_due_ms = now_ms; /* play first step immediately */
        return;

    case BZ_ENCODE_ALT:
        t->pattern_len = build_alt_pattern(t->req_altitude, t->pattern);
        t->loop_start = 0;   /* altitude loops from the beginning */
        t->repeat_count = 0; /* infinite — BUZ-06 */
        t->loops_done = 0;
        t->index = 0;
        t->state = BZ_PLAYING;
        t->base.next_due_ms = now_ms; /* play first step immediately */
        return;

    case BZ_PLAYING: {
        /* Bounds check */
        if (t->index >= t->pattern_len) {
            /* Should not happen — sentinel should stop us first */
            hal_buzzer_tone_off();
            t->state = BZ_IDLE;
            t->base.tick = NULL;
            return;
        }

        const buzzer_pattern_t *step = &t->pattern[t->index];

        /* Sentinel: zero-duration entry — one complete pass finished */
        if (step->duration_ms == 0) {
            t->loops_done++;
            /* repeat_count==0 means infinite; otherwise stop when done */
            if (t->repeat_count == 0 || t->loops_done < t->repeat_count) {
                t->index = t->loop_start; /* restart from digits (chirps skipped) */
                t->base.next_due_ms = now_ms;
                return;
            }
            hal_buzzer_tone_off();
            t->state = BZ_IDLE;
            t->base.tick = NULL;
            return;
        }

        /* Apply the step */
        if (step->tone_on)
            hal_buzzer_tone_on();
        else
            hal_buzzer_tone_off();

        t->base.next_due_ms = now_ms + step->duration_ms;
        t->index++;
        return;
    }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void buzzer_init(void) {
    memset(&bz, 0, sizeof(bz));
    bz.base.tick = NULL; /* inactive until first play */
    bz.base.next_due_ms = 0;
    bz.state = BZ_IDLE;
    hal_buzzer_init();
    hal_buzzer_task_register(&bz.base);
}

void buzzer_play_code(uint8_t code, uint8_t repeat_count) {
    hal_buzzer_tone_off(); /* silence immediately */
    bz.req_code = code;
    bz.req_repeat_count = repeat_count;
    bz.loops_done = 0;
    bz.index = 0;
    bz.state = BZ_ENCODE_CODE;
    bz.base.next_due_ms = 0; /* run on next hal_tasks_tick() */
    bz.base.tick = buzzer_tick;
}

void buzzer_play_altitude(int32_t value_in_units) {
    hal_buzzer_tone_off();
    bz.req_altitude = value_in_units;
    bz.index = 0;
    bz.state = BZ_ENCODE_ALT;
    bz.base.next_due_ms = 0;
    bz.base.tick = buzzer_tick;
}

void buzzer_stop(void) {
    hal_buzzer_tone_off();
    bz.state = BZ_IDLE;
    bz.base.tick = NULL;
}

bool buzzer_is_active(void) {
    return bz.state != BZ_IDLE;
}
