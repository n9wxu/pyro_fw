/*
 * The ground test procedure, in time [GND-TEST-05..11].
 *
 * A board powered up with its ground test pin asserted enters ground test
 * mode and says so. The pin held, then released, starts a countdown, and at
 * zero channel 1 fires. A steady tone and a second countdown lead to
 * channel 2, then the all-clear sounds. A channel the configuration does not
 * enable is skipped, with its tone and countdown. Nothing else happens until
 * the next power-up.
 *
 * The sequence owns the schedule and the buzzer only voices it, so a board
 * with no buzzer fits the same firings to the same clock, silently. The
 * caller reads the pin, sounds what it is told and fires what it is told;
 * the host tests drive this with a fake clock.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GROUND_TEST_SEQ_H
#define GROUND_TEST_SEQ_H

#include <stdbool.h>
#include <stdint.h>

/* A pin level counts once it has held this long. */
#define GT_DEBOUNCE_MS 100u
/* Asserted this long at the end of the power-up settle: ground test mode. */
#define GT_BOOT_HOLD_MS 500u
/* Held this long in ground test mode before a release starts the countdown,
 * so the operator has heard the mode before anything can follow from it. */
#define GT_ARM_MS 1000u

/* The voices, shared with buzzer.c so the sound and the schedule cannot
 * drift apart. */
#define GT_LONG_ON_MS 500u
#define GT_LONG_OFF_MS 300u
#define GT_ALERT_PAUSE_MS 1500u
/* The countdown: a count a second, 5 fast beeps, then 4, down to 1, and the
 * fire at zero. Five fast beeps take 0.54 s, so each count ends well before
 * the next begins. */
#define GT_FAST_ON_MS 60u
#define GT_FAST_OFF_MS 60u
#define GT_COUNT_FROM 5u
#define GT_COUNT_MS 1000u
#define GT_COUNTDOWN_MS (GT_COUNT_FROM * GT_COUNT_MS)
/* The steady tone after channel 1, before channel 2's countdown. */
#define GT_TONE_MS 3000u
/* Three long beeps, once. */
#define GT_ALL_CLEAR_MS (3u * GT_LONG_ON_MS + 2u * GT_LONG_OFF_MS)

typedef enum {
    GT_ALERT,       /* ground test mode, announced; waiting for the release */
    GT_COUNTDOWN,   /* released: 5 down to 0, then channel 1 or 2 */
    GT_FIRE_1,      /* firing channel 1 */
    GT_TONE,        /* the steady tone after channel 1 */
    GT_COUNTDOWN_2, /* 5 down to 0, then channel 2 */
    GT_FIRE_2,      /* firing channel 2 */
    GT_ALL_CLEAR,   /* three long beeps */
    GT_DONE,        /* silent until the next power-up */
} gt_phase_t;

typedef enum {
    GT_SOUND_NONE,      /* leave the buzzer as it is */
    GT_SOUND_ALERT,     /* three long beeps and a pause, repeating */
    GT_SOUND_COUNTDOWN, /* the countdown, once */
    GT_SOUND_TONE,      /* a steady tone for GT_TONE_MS */
    GT_SOUND_ALL_CLEAR, /* three long beeps, once */
} gt_sound_t;

typedef enum {
    GT_FIRE_NONE,      /* not asked */
    GT_FIRE_BUSY,      /* the other channel's pulse still runs: ask again */
    GT_FIRE_FAULT,     /* commanded; the board reported nothing energised */
    GT_FIRE_ENERGISED, /* fired */
} gt_fire_t;

typedef struct {
    gt_phase_t phase;
    uint32_t phase_ms; /* when the phase began */
    bool enabled[2];
    bool raw;            /* the pin as last read */
    uint32_t raw_since;  /* when it last changed */
    bool asserted;       /* debounced */
    uint32_t held_since; /* when the debounced level last became asserted */
    bool armed;          /* held GT_ARM_MS in the mode: a release now counts */
    bool sounded;        /* the phase's sound has been asked for */
    gt_fire_t result[2]; /* each channel's last fire, for the status */
} gt_seq_t;

/* What to do now. */
typedef struct {
    gt_sound_t sound; /* start this sound */
    uint8_t fire;     /* fire this channel, 1 or 2; 0 for none */
} gt_action_t;

/* Enter ground test mode, with the channels the configuration enables and
 * the pin's level at entry. */
void gt_seq_begin(gt_seq_t *s, bool pyro1, bool pyro2, bool asserted, uint32_t now);

/* Once a loop, with the pin as read. */
gt_action_t gt_seq_step(gt_seq_t *s, bool asserted, uint32_t now);

/* The outcome of the fire gt_seq_step() asked for. BUSY leaves the request
 * standing for the next step. */
void gt_seq_fired(gt_seq_t *s, uint8_t channel, gt_fire_t r, uint32_t now);

/* For telemetry and /api/status. */
const char *gt_seq_phase_name(gt_phase_t p);

#endif /* GROUND_TEST_SEQ_H */
