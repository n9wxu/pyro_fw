/*
 * Pyro backend -- Pyro MK1B: AP2192 high-side switches per channel and one
 * shared low side. See THEORY_OF_OPERATION.md "Pyro circuit".
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
#include "pin_store.h"
#include "pico/stdlib.h"

/* See THEORY_OF_OPERATION.md "Continuity check" [DD-059]. */
#define PATH_TO_GROUND_MAX_COUNTS 500
#define NO_PATH_MIN_COUNTS 3000
#define PRESENCE_SETTLE_MS 10
#define CHECK_PERIOD_MS 1000
#define NODE_RECHARGE_MS 50 /* 5 x 100.1k x 100 nF */

#define FIRE_PULSE_MS 500

static const uint8_t pyro_outputs[] = {BOARD_PIN_PYRO_COMMON_EN, BOARD_PIN_PYRO1_EN, BOARD_PIN_PYRO2_EN};
static const uint8_t high_side_pin[2] = {BOARD_PIN_PYRO1_EN, BOARD_PIN_PYRO2_EN};
static const uint8_t fault_flag_pin[2] = {BOARD_PIN_PYRO1_FLAG, BOARD_PIN_PYRO2_FLAG};
static const uint8_t sense_adc[2] = {BOARD_ADC_CH_SENSE1, BOARD_ADC_CH_SENSE2};

typedef enum {
    CHECK_IDLE,     /* common off; ends with the short reading   */
    CHECK_PRESENCE, /* common on; ends with the presence reading */
} check_phase_t;

static struct {
    check_phase_t phase;
    uint32_t due_ms;
    uint16_t short_counts[2];
    bool complete;
    pyro_continuity_t result[2];
} check;

static struct {
    uint8_t channel; /* 0: no pulse */
    uint32_t start_ms;
} pulse;

void board_early_init(void) {
    DRIVE_ALL_LOW(pyro_outputs);
}

static void read_sense(uint16_t counts[2]) {
    for (int i = 0; i < 2; i++)
        counts[i] = adc_read_raw(sense_adc[i]);
}

static pyro_continuity_t classify(uint16_t presence, uint16_t shorts) {
    pyro_continuity_t c;
    c.raw_adc = presence;
    c.shorted = shorts < PATH_TO_GROUND_MAX_COUNTS;
    c.open = presence > NO_PATH_MIN_COUNTS;
    c.good = presence < PATH_TO_GROUND_MAX_COUNTS && !c.shorted;
    return c;
}

/* A released channel's enable is a Lua pad: only the owned ones are driven. */
static void hold_high_sides_off(void) {
    for (int i = 0; i < 2; i++)
        if (pin_store_owns(high_side_pin[i]))
            gpio_put(high_side_pin[i], 0);
}

static void begin_presence(uint32_t now_ms, bool read_shorts_first) {
    if (read_shorts_first)
        read_sense(check.short_counts);
    hold_high_sides_off();
    gpio_put(BOARD_PIN_PYRO_COMMON_EN, 1);
    check.phase = CHECK_PRESENCE;
    check.due_ms = now_ms + PRESENCE_SETTLE_MS;
}

static void finish_presence(uint32_t now_ms) {
    uint16_t presence[2];
    read_sense(presence);
    gpio_put(BOARD_PIN_PYRO_COMMON_EN, 0);
    for (int i = 0; i < 2; i++)
        check.result[i] = classify(presence[i], check.short_counts[i]);
    check.complete = true;
    check.phase = CHECK_IDLE;
    check.due_ms = now_ms + CHECK_PERIOD_MS - PRESENCE_SETTLE_MS;
}

static void check_step(uint32_t now_ms) {
    if (!deadline_reached(now_ms, check.due_ms))
        return;
    if (check.phase == CHECK_IDLE)
        begin_presence(now_ms, true);
    else
        finish_presence(now_ms);
}

/* ── pyro.h ───────────────────────────────────────────────────────── */

/* The first check starts from pyro_update(), never here: a board with both
 * channels released never calls it, so the common, by then Lua's pad, is
 * never left raised. */
void pyro_init(void) {
    adc_init();
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);
    DRIVE_ALL_LOW(pyro_outputs);
    for (int i = 0; i < 2; i++) {
        gpio_init(fault_flag_pin[i]);
        gpio_set_dir(fault_flag_pin[i], GPIO_IN);
        gpio_pull_up(fault_flag_pin[i]); /* open drain, active low */
    }
    pulse.channel = 0;
    check.phase = CHECK_IDLE;
    check.due_ms = to_ms_since_boot(get_absolute_time()) + NODE_RECHARGE_MS;
    check.complete = false;
    check.short_counts[0] = check.short_counts[1] = 4095;
}

void pyro_sample(void) {}

/* [PYR-HEALTH-01] A board that cannot judge a channel reports it ready, with
 * the count it read. */
static pyro_continuity_t ready_unjudged(uint8_t channel) {
    return (pyro_continuity_t){.raw_adc = check.complete ? check.result[channel - 1].raw_adc : 0, .good = true};
}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    if (BOARD_PYRO_U5_DISCHARGES_OUTPUTS) {
        *out = ready_unjudged(channel);
        return;
    }
    if (!check.complete) {
        *out = (pyro_continuity_t){.raw_adc = 0, .good = false, .open = true, .shorted = false};
        return;
    }
    *out = check.result[channel - 1];
}

void pyro_fire(uint8_t channel) {
    gpio_put(BOARD_PIN_PYRO_COMMON_EN, 1);
    gpio_put(high_side_pin[channel - 1], 1);
    pulse.channel = channel;
    pulse.start_ms = to_ms_since_boot(get_absolute_time());
}

/* See THEORY_OF_OPERATION.md "Firing": the common stays on into a fresh
 * presence reading, and the last short reading stands. */
static void end_pulse(uint32_t now_ms) {
    gpio_put(high_side_pin[pulse.channel - 1], 0);
    pulse.channel = 0;
    begin_presence(now_ms, false);
}

void pyro_update(uint32_t now_ms) {
    if (pulse.channel == 0)
        check_step(now_ms);
    else if (now_ms - pulse.start_ms >= FIRE_PULSE_MS)
        end_pulse(now_ms);
}

bool pyro_is_firing(void) {
    return pulse.channel != 0;
}

bool pyro_fault(uint8_t channel) {
    return !gpio_get(fault_flag_pin[channel - 1]); /* AP2192 FLG: low is a fault */
}
