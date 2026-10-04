/*
 * Pyro backend -- Pyro MK1A: a switched high side per channel and one shared
 * low side. See THEORY_OF_OPERATION.md "Pyro circuit".
 *
 * pyro_fire() is the only writer of a firing output high; see
 * THEORY_OF_OPERATION.md "Firing".
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "board_if.h"
#include "board_pins.h"
#include "board_support.h"
#include "pico/time.h"

/* See THEORY_OF_OPERATION.md "Continuity check". */
#define PATH_TO_GROUND_MAX_COUNTS 500
#define NO_PATH_MIN_COUNTS 3000
#define NODE_SETTLE_MS 50 /* 5 x 101k x 100 nF */
#define IDLE_BETWEEN_CHECKS_MS 400

#define FIRE_PULSE_MS 500

static const uint8_t pyro_outputs[] = {BOARD_PIN_FIRE1, BOARD_PIN_FIRE2, BOARD_PIN_PYRO_LOW};
static const uint8_t high_side_pin[2] = {BOARD_PIN_FIRE1, BOARD_PIN_FIRE2};
static const uint8_t sense_adc[2] = {BOARD_ADC_CH_SENSE1, BOARD_ADC_CH_SENSE2};

typedef enum {
    CHECK_PRESENCE, /* low side on: a channel pulled low has its igniter */
    CHECK_SHORTS,   /* low side off: a channel still low is shorted       */
    CHECK_IDLE,
} check_phase_t;

static struct {
    check_phase_t phase;
    uint32_t due_ms;
    uint16_t presence_counts[2];
    bool complete;
    bool fired_since[2]; /* fired, and no check begun after its pulse has ended */
    pyro_continuity_t result[2];
} check;

static struct {
    uint8_t channel; /* 0: no pulse */
    uint32_t start_ms;
} pulse;

void board_early_init(void) {
    DRIVE_ALL_LOW(pyro_outputs);
}

static void low_side(bool on) {
    gpio_put(BOARD_PIN_PYRO_LOW, on);
}

static void read_sense(uint16_t counts[2]) {
    for (int i = 0; i < 2; i++)
        counts[i] = adc_read_median3(sense_adc[i]);
}

static pyro_continuity_t classify(uint16_t presence, uint16_t shorts) {
    pyro_continuity_t c;
    c.raw_adc = presence;
    c.shorted = shorts < PATH_TO_GROUND_MAX_COUNTS;
    c.open = presence > NO_PATH_MIN_COUNTS;
    c.good = presence < PATH_TO_GROUND_MAX_COUNTS && !c.shorted;
    return c;
}

static void begin_check(uint32_t now_ms) {
    low_side(true);
    check.phase = CHECK_PRESENCE;
    check.due_ms = now_ms + NODE_SETTLE_MS;
}

static void check_step(uint32_t now_ms) {
    if (!deadline_reached(now_ms, check.due_ms))
        return;
    switch (check.phase) {
    case CHECK_PRESENCE:
        read_sense(check.presence_counts);
        low_side(false);
        check.phase = CHECK_SHORTS;
        check.due_ms = now_ms + NODE_SETTLE_MS;
        break;
    case CHECK_SHORTS: {
        uint16_t short_counts[2];
        read_sense(short_counts);
        for (int i = 0; i < 2; i++) {
            check.result[i] = classify(check.presence_counts[i], short_counts[i]);
            check.fired_since[i] = false;
        }
        check.complete = true;
        check.phase = CHECK_IDLE;
        check.due_ms = now_ms + IDLE_BETWEEN_CHECKS_MS;
        break;
    }
    case CHECK_IDLE:
        begin_check(now_ms);
        break;
    }
}

/* ── pyro.h ───────────────────────────────────────────────────────── */

void pyro_init(void) {
    DRIVE_ALL_LOW(pyro_outputs);
    adc_init();
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);
    pulse.channel = 0;
    check.complete = false;
    check.fired_since[0] = check.fired_since[1] = false;
    begin_check(to_ms_since_boot(get_absolute_time()));
}

void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    if (!check.complete) {
        *out = (pyro_continuity_t){.raw_adc = 0, .good = false, .open = true, .shorted = false};
        return;
    }
    if (check.fired_since[channel - 1]) {
        *out = (pyro_continuity_t){0}; /* no verdict yet [PYR-VERIFY-01] */
        return;
    }
    *out = check.result[channel - 1];
}

/* [PYR-DEPLOY-02] One channel at a time, whatever the caller checked. */
void pyro_fire(uint8_t channel) {
    if ((channel != 1 && channel != 2) || pulse.channel != 0)
        return;
    low_side(true);
    gpio_put(high_side_pin[channel - 1], 1);
    pulse.channel = channel;
    pulse.start_ms = to_ms_since_boot(get_absolute_time());
    check.fired_since[channel - 1] = true;
}

/* See THEORY_OF_OPERATION.md "Firing" for why a check restarts at once. */
static void end_pulse(uint32_t now_ms) {
    gpio_put(high_side_pin[pulse.channel - 1], 0);
    pulse.channel = 0;
    begin_check(now_ms);
}

void pyro_update(uint32_t now_ms) {
    if (pulse.channel == 0)
        check_step(now_ms);
    else if (deadline_reached(now_ms, pulse.start_ms + FIRE_PULSE_MS))
        end_pulse(now_ms);
}

bool pyro_is_firing(void) {
    return pulse.channel != 0;
}

bool pyro_fault(uint8_t channel) {
    (void)channel;
    return false; /* no fault output on this board */
}
