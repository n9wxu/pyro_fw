#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include "beep_codes.h"
#include <stdbool.h>

/* ── Buzzer pattern types ─────────────────────────────────────────── */

/*
 * One step in a buzzer pattern: a duration and a tone state.
 * A zero-duration entry is the end-of-pattern sentinel.
 */
typedef struct {
    uint16_t duration_ms;
    bool tone_on;
} buzzer_pattern_t;

#define BUZZER_PATTERN_END {0, false}
/* Max steps in one encoded pattern. An altitude beep-out spends ten beeps on
 * each zero digit, so a six-digit value needs up to 120; a shorter buffer
 * truncates the pattern silently and loops a garbled one. */
#define BUZZER_MAX_PATTERN 128

/* What a beep sounds like lives in beep_codes.h, one row per REASON, and is
 * configurable through beep.ini. The firmware asks beep_for(BR_...) and gets
 * whatever is assigned; nothing outside beep_store.c should hold a raw spec. */

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * buzzer_init() — register the buzzer as an autonomous async task.
 * Call once at boot before any buzzer_play_* call.
 * Calls hal_buzzer_init() internally for GPIO setup.
 */
void buzzer_init(void);

/* Play one outcome under the active personality. gap_ms is the silence
 * between re-announcements; repeat_count 0 runs until stopped.
 *
 * Takes a spec rather than a code because a chirp -- Eggtimer's ready-to-fly
 * -- cannot be written as a pair of beep counts. See beep_codes.h. */
void buzzer_play_spec(const beep_spec_t *spec, uint16_t gap_ms, uint8_t repeat_count);

/*
 * buzzer_play_altitude() — encode and play an altitude digit beep-out.
 * Sequence: long pause → long beep → each digit beep → repeat forever.
 * value_in_units is already in the configured units (m or ft).
 * Non-blocking: encoding and playback run in the async task runner.
 */
void buzzer_play_altitude(int32_t value_in_units);

/* [USB-03] Two short chirps, once: the only thing a board on USB says. Fixed
 * rather than a beep.ini reason, so no personality can make it sound like a
 * status code. */
void buzzer_play_usb_ok(void);

/* Stop playback immediately and silence the buzzer. */
void buzzer_stop(void);

/* Returns true if the buzzer task is currently encoding or playing. */
bool buzzer_is_active(void);

#endif
