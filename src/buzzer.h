#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
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
#define BUZZER_MAX_PATTERN 64 /* max steps in one encoded pattern */

/* ── Beep codes: two digits, each 1-5 beeps ──────────────────────── */

#define BEEP_CODE(d1, d2) (((d1) << 4) | (d2))
#define BEEP_DIGIT1(code) (((code) >> 4) & 0x0F)
#define BEEP_DIGIT2(code) ((code) & 0x0F)

/* Status codes */
#define BEEP_ALL_GOOD BEEP_CODE(1, 1)
#define BEEP_P1_OPEN BEEP_CODE(2, 1)
#define BEEP_P1_SHORT BEEP_CODE(2, 2)
#define BEEP_P1_FAULT BEEP_CODE(2, 3)
#define BEEP_P1_NO_OPEN BEEP_CODE(2, 4)
#define BEEP_P2_OPEN BEEP_CODE(3, 1)
#define BEEP_P2_SHORT BEEP_CODE(3, 2)
#define BEEP_P2_FAULT BEEP_CODE(3, 3)
#define BEEP_P2_NO_OPEN BEEP_CODE(3, 4)
#define BEEP_SENSOR_FAIL BEEP_CODE(4, 1)
#define BEEP_FS_FAIL BEEP_CODE(4, 2)
#define BEEP_CFG_RANGE BEEP_CODE(4, 3) /* pyro altitude setting out of range */
#define BEEP_CRITICAL BEEP_CODE(5, 5)

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * buzzer_init() — register the buzzer as an autonomous async task.
 * Call once at boot before any buzzer_play_* call.
 * Calls hal_buzzer_init() internally for GPIO setup.
 */
void buzzer_init(void);

/*
 * buzzer_play_code() — encode and play a two-digit beep code.
 * Sequence: 10 startup chirps → pause → digit1 beeps → gap → digit2 beeps.
 * If repeat=true the sequence restarts forever after the last digit.
 * Non-blocking: encoding and playback run in the async task runner.
 */
void buzzer_play_code(uint8_t code, bool repeat);

/*
 * buzzer_play_altitude() — encode and play an altitude digit beep-out.
 * Sequence: long pause → long beep → each digit beep → repeat forever.
 * value_in_units is already in the configured units (m or ft).
 * Non-blocking: encoding and playback run in the async task runner.
 */
void buzzer_play_altitude(int32_t value_in_units);

/* Stop playback immediately and silence the buzzer. */
void buzzer_stop(void);

/* Returns true if the buzzer task is currently encoding or playing. */
bool buzzer_is_active(void);

/* ── Legacy compatibility shims ───────────────────────────────────── */

/* These redirect to the new API so existing call sites continue to
 * compile without modification during the migration. */
static inline void buzzer_set_code(uint8_t code, bool repeat) {
    buzzer_play_code(code, repeat);
}
static inline void buzzer_set_altitude(int32_t alt) {
    buzzer_play_altitude(alt);
}

#endif
