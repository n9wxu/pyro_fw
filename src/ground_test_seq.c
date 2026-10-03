/*
 * The ground test procedure, in time (ground_test_seq.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "ground_test_seq.h"
#include <string.h>

static void enter(gt_seq_t *s, gt_phase_t p, uint32_t now) {
    s->phase = p;
    s->phase_ms = now;
    s->sounded = false;
}

static gt_sound_t sound_of(gt_phase_t p) {
    switch (p) {
    case GT_ALERT:
        return GT_SOUND_ALERT;
    case GT_COUNTDOWN:
    case GT_COUNTDOWN_2:
        return GT_SOUND_COUNTDOWN;
    case GT_TONE:
        return GT_SOUND_TONE;
    case GT_ALL_CLEAR:
        return GT_SOUND_ALL_CLEAR;
    default:
        return GT_SOUND_NONE;
    }
}

/* The pin put back before anything has fired again: the mode is announced
 * afresh, and a release must be earned again. */
static void abort_to_alert(gt_seq_t *s, uint32_t now) {
    s->armed = false;
    enter(s, GT_ALERT, now);
}

/* After the countdown, or after channel 1: the next enabled channel, or the
 * all-clear. */
static void after(gt_seq_t *s, int channel_done, uint32_t now) {
    if (channel_done < 1 && s->enabled[0])
        enter(s, GT_FIRE_1, now);
    else if (channel_done < 1 && s->enabled[1])
        enter(s, GT_FIRE_2, now);
    else if (channel_done == 1 && s->enabled[1])
        enter(s, GT_TONE, now);
    else
        enter(s, GT_ALL_CLEAR, now);
}

void gt_seq_begin(gt_seq_t *s, bool pyro1, bool pyro2, bool asserted, uint32_t now) {
    memset(s, 0, sizeof(*s));
    s->enabled[0] = pyro1;
    s->enabled[1] = pyro2;
    s->raw = asserted;
    s->raw_since = now;
    s->asserted = asserted;
    s->held_since = now;
    enter(s, GT_ALERT, now);
}

static void debounce(gt_seq_t *s, bool raw, uint32_t now) {
    if (raw != s->raw) {
        s->raw = raw;
        s->raw_since = now;
    }
    if (s->raw != s->asserted && now - s->raw_since >= GT_DEBOUNCE_MS) {
        s->asserted = s->raw;
        if (s->asserted)
            s->held_since = now;
    }
}

/* [GND-TEST-09] The switch counts as opened only once the mode has been
 * announced for GT_ARM_MS with it closed. */
static void alert_step(gt_seq_t *s, uint32_t now) {
    if (s->asserted && now - s->held_since >= GT_ARM_MS)
        s->armed = true;
    if (s->armed && !s->asserted)
        enter(s, GT_COUNTDOWN, now);
}

static bool phase_has_run(const gt_seq_t *s, uint32_t duration_ms, uint32_t now) {
    return now - s->phase_ms >= duration_ms;
}

static void advance(gt_seq_t *s, uint32_t now) {
    switch (s->phase) {
    case GT_ALERT:
        alert_step(s, now);
        break;
    case GT_COUNTDOWN:
        if (phase_has_run(s, GT_COUNTDOWN_MS, now))
            after(s, 0, now);
        break;
    case GT_TONE:
        if (phase_has_run(s, GT_TONE_MS, now))
            enter(s, GT_COUNTDOWN_2, now);
        break;
    case GT_COUNTDOWN_2:
        if (phase_has_run(s, GT_COUNTDOWN_MS, now))
            enter(s, GT_FIRE_2, now);
        break;
    case GT_ALL_CLEAR:
        if (phase_has_run(s, GT_ALL_CLEAR_MS, now))
            enter(s, GT_DONE, now);
        break;
    default:
        break;
    }
}

/* [GND-TEST-10] */
static bool stopped_by_the_switch(gt_phase_t p) {
    return p == GT_COUNTDOWN || p == GT_TONE || p == GT_COUNTDOWN_2;
}

static uint8_t channel_to_fire(gt_phase_t p) {
    return p == GT_FIRE_1 ? 1u : p == GT_FIRE_2 ? 2u : 0u;
}

gt_action_t gt_seq_step(gt_seq_t *s, bool asserted, uint32_t now) {
    debounce(s, asserted, now);
    if (s->asserted && stopped_by_the_switch(s->phase))
        abort_to_alert(s, now);
    else
        advance(s, now);
    gt_action_t a = {GT_SOUND_NONE, channel_to_fire(s->phase)};
    if (!s->sounded) {
        a.sound = sound_of(s->phase);
        s->sounded = true;
    }
    return a;
}

void gt_seq_fired(gt_seq_t *s, uint8_t channel, gt_fire_t r, uint32_t now) {
    if (r == GT_FIRE_BUSY)
        return;
    if (channel == 1 && s->phase == GT_FIRE_1) {
        s->result[0] = r;
        after(s, 1, now);
    } else if (channel == 2 && s->phase == GT_FIRE_2) {
        s->result[1] = r;
        after(s, 2, now);
    }
}

const char *gt_seq_phase_name(gt_phase_t p) {
    static const char *const names[] = {"alert",       "countdown", "fire_1",    "tone",
                                        "countdown_2", "fire_2",    "all_clear", "done"};
    return (unsigned)p < sizeof(names) / sizeof(names[0]) ? names[p] : "?";
}
