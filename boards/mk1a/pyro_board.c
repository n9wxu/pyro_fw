/*
 * Pyro backend — Pyro MK1A.
 *
 * Implements src/pyro.h. Per-channel high-side MOSFET (Q6, Q1 DMC2053UVT)
 * and ONE shared low-side MOSFET (Q2 AO3400A) behind an 8 A fuse:
 *
 *   VBATT -> Q6 [FIRE1] -> J3 igniter -.
 *   VBATT -> Q1 [FIRE2] -> J4 igniter -+-> Initiator_ground
 *                    Initiator_ground -> F1 8A -> Q2 [PYRO_LOW] -> GND
 *
 * Current through an igniter needs BOTH its own high side and the shared low
 * side, so either one off breaks the circuit.
 *
 * ---------------------------------------------------------------------------
 * SAFETY INVARIANT
 *
 *   pyro_fire() is the only function below that writes FIRE1 or FIRE2 high,
 *   and only together with PYRO_LOW. No diagnostic, self-check, boot path or
 *   continuity test asserts either. That absence is what makes the sense
 *   cycle safe to run continuously.
 *
 * ---------------------------------------------------------------------------
 * CONTINUITY SENSE
 *
 * R9/R10 (100k) weakly pull each igniter\'s high node to +3V3, and that node
 * is what SENSE1/SENSE2 tap through a 1k series resistor. Both high sides stay
 * off throughout, so no current reaches a bridgewire from VBATT.
 *
 *   Phase 1, PYRO_LOW ASSERTED -- Initiator_ground is pulled to GND, so a
 *   connected igniter ties the sense node down against the 100k pull-up:
 *       low  -> a conducting path exists (igniter present)
 *       high -> open
 *
 *   Phase 2, PYRO_LOW DEASSERTED -- Initiator_ground floats, so the pull-up
 *   should win on both channels:
 *       low  -> short to ground
 *
 * The weak pull-up makes the discrimination wide. Against 100k at 12 bits, a
 * 2 ohm igniter reads 0 counts, a 1k bad joint 41, a 10k leakage path 372,
 * and a genuine open 4095. A degraded connection therefore lands in the gap
 * between the two thresholds rather than being rounded up to "good", which is
 * what a boolean alone would hide.
 *
 * TIMING
 *
 *   The node going open has to charge C6/C5 (100nF) through R9+R5 (101k), a
 *   10.1 ms time constant, so an open channel needs about 50 ms to read as
 *   open. Going low is fast -- 1k into 100nF, 100 us -- but the slow edge is
 *   the one that decides open, and it cannot be a sleep_ms() inside a 10 ms
 *   main loop. Each phase parks on a deadline and samples on a later
 *   iteration, so the loop period is the settle timer.
 *
 *   One full cycle is 2 x SETTLE_MS = 100 ms, and pyro_get() returns the last
 *   completed one. Continuity does not change except by firing, so the
 *   staleness costs nothing: the post-fire verify window opens 500 ms after
 *   the pulse, by which point every cycle reflects the fired state.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "board_pins.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include <stdio.h>

extern void hal_telemetry_send(const char *sentence);

/* ── Thresholds, in raw 12-bit counts ─────────────────────────────
 *
 * Against the 100k pull-up: 0 counts for a real igniter, 41 for a 1k bad
 * joint, 372 for a 10k leakage path, 4095 open. Both thresholds sit in empty
 * space, and the gap between them is reported as neither good nor open. */
#define CNT_PATH_MAX 500  /* below: a conducting path to ground exists */
#define CNT_OPEN_MIN 3000 /* above: no path at all                     */

/* 5 time constants of 101k x 100nF. The slow edge decides "open". */
#define SETTLE_MS 50

/* Idle between cycles, with PYRO_LOW released.
 *
 * Continuity does not change except by firing, so measuring it continuously
 * buys nothing and holds the shared low side on half the time. With an idle
 * the duty drops to 50 ms in 500, which is less exposure if a high side is
 * ever leaking, and less average current through the pull-up path. */
#define IDLE_MS 400

#define FIRE_DURATION_MS 500

/* ── Firing ───────────────────────────────────────────────────────── */

static uint8_t firing_channel;
static uint32_t fire_start_ms;

/* ── Sense cycle ──────────────────────────────────────────────────── */

typedef enum {
    SNS_LOW_ON,  /* PYRO_LOW asserted, settling; sample decides present/open */
    SNS_LOW_OFF, /* PYRO_LOW released, settling; sample decides short        */
    SNS_IDLE,    /* PYRO_LOW released, nothing driven                        */
} sns_phase_t;

static sns_phase_t sns_phase;
static uint32_t sns_due_ms;
static uint16_t cont_counts[2];  /* phase 1 */
static uint16_t short_counts[2]; /* phase 2 */
static bool sns_valid;
static pyro_continuity_t cont[2];

/* Called from board_early_init() before any slow initialisation, and again
 * from pyro_init(). The only function that writes FIRE1 or FIRE2 low. */
void pyro_safe_all_outputs(void) {
    static const uint8_t outputs[] = {
        BOARD_PIN_FIRE1,
        BOARD_PIN_FIRE2,
        BOARD_PIN_PYRO_LOW,
    };
    for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        gpio_init(outputs[i]);
        gpio_put(outputs[i], 0); /* set the level before the direction */
        gpio_set_dir(outputs[i], GPIO_OUT);
        gpio_put(outputs[i], 0);
    }
}

static uint16_t adc_sample(uint8_t channel) {
    adc_select_input(channel);
    return (uint16_t)adc_read(); /* raw 12-bit, 0-4095 */
}

/* Median of 3. The taps are RC-filtered, but the filter does nothing about a
 * conversion that lands on a switching transient elsewhere on the board. */
static uint16_t adc_median3(uint8_t channel) {
    uint16_t x = adc_sample(channel);
    uint16_t y = adc_sample(channel);
    uint16_t z = adc_sample(channel);
    if (x > y) {
        uint16_t t = x;
        x = y;
        y = t;
    }
    if (y > z)
        y = (x > z) ? x : z;
    return y;
}

/* Report the raw count alongside the booleans: a degraded connection sits
 * between the thresholds and only the number shows it. */
static void classify(int i) {
    cont[i].raw_adc = cont_counts[i];
    cont[i].shorted = short_counts[i] < CNT_PATH_MAX;
    cont[i].open = cont_counts[i] > CNT_OPEN_MIN;
    cont[i].good = (cont_counts[i] < CNT_PATH_MAX) && !cont[i].shorted;
}

/* Advance one step. Never blocks: each phase parks on a deadline and the
 * next main-loop iteration picks it up. */
static void sense_service(uint32_t now_ms) {
    if ((int32_t)(now_ms - sns_due_ms) < 0)
        return;

    switch (sns_phase) {
    case SNS_LOW_ON:
        cont_counts[0] = adc_median3(BOARD_ADC_CH_SENSE1);
        cont_counts[1] = adc_median3(BOARD_ADC_CH_SENSE2);
        gpio_put(BOARD_PIN_PYRO_LOW, 0);
        sns_phase = SNS_LOW_OFF;
        sns_due_ms = now_ms + SETTLE_MS;
        break;

    case SNS_LOW_OFF:
        short_counts[0] = adc_median3(BOARD_ADC_CH_SENSE1);
        short_counts[1] = adc_median3(BOARD_ADC_CH_SENSE2);
        classify(0);
        classify(1);
        sns_valid = true;
        sns_phase = SNS_IDLE;
        sns_due_ms = now_ms + IDLE_MS;
        break;

    case SNS_IDLE:
    default:
        gpio_put(BOARD_PIN_PYRO_LOW, 1);
        sns_phase = SNS_LOW_ON;
        sns_due_ms = now_ms + SETTLE_MS;
        break;
    }
}

/* ── pyro.h implementation ────────────────────────────────────────── */

void pyro_init(void) {
    pyro_safe_all_outputs();

    adc_init();
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);

    firing_channel = 0;
    sns_valid = false;

    /* Start phase 1 settling now. The deadline has to be a real timestamp:
     * leaving it at 0 would make the first pyro_update() sample immediately,
     * before the node had settled at all. */
    sns_phase = SNS_LOW_ON;
    sns_due_ms = to_ms_since_boot(get_absolute_time()) + SETTLE_MS;
    gpio_put(BOARD_PIN_PYRO_LOW, 1);
}

/* The cycle runs continuously from pyro_update(), so there is no stimulus to
 * start and nothing to wait for. This board settles in 50 ms, five main-loop
 * periods, which is not time to spend inside the flight loop. */
void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    if (!sns_valid) {
        /* No complete cycle yet. Report not-good rather than a default-
         * initialised struct that reads as healthy. */
        out->raw_adc = 0;
        out->good = false;
        out->open = true;
        out->shorted = false;
        return;
    }
    *out = cont[channel - 1];
}

/* The only place FIRE1 or FIRE2 goes high, and only with PYRO_LOW. */
void pyro_fire(uint8_t channel) {
    if (channel != 1 && channel != 2)
        return;
    gpio_put(BOARD_PIN_PYRO_LOW, 1);
    gpio_put(channel == 1 ? BOARD_PIN_FIRE1 : BOARD_PIN_FIRE2, 1);
    firing_channel = channel;
    fire_start_ms = to_ms_since_boot(get_absolute_time());
}

void pyro_update(uint32_t now_ms) {
    if (firing_channel) {
        /* The sense cycle is suspended while firing: it owns PYRO_LOW, which
         * the pulse also needs. Ending the pulse is the only thing that
         * matters here, and it is the only path that lowers FIRE1/FIRE2. */
        if ((int32_t)(now_ms - (fire_start_ms + FIRE_DURATION_MS)) >= 0) {
            gpio_put(firing_channel == 1 ? BOARD_PIN_FIRE1 : BOARD_PIN_FIRE2, 0);
            gpio_put(BOARD_PIN_PYRO_LOW, 0);
            firing_channel = 0;

            /* Restart at phase 1, not at the idle, and re-assert PYRO_LOW
             * straight away. The post-fire verify window opens 500 ms after
             * the pulse started -- which is the instant the pulse ends -- and
             * checks that continuity has gone open. Resuming anywhere else
             * would leave the PRE-fire reading latched through that window,
             * and flight_states.c reads a still-good channel as a verify
             * failure. Settling from here lands a fresh reading ~50 ms in,
             * inside the window. */
            gpio_put(BOARD_PIN_PYRO_LOW, 1);
            sns_phase = SNS_LOW_ON;
            sns_due_ms = now_ms + SETTLE_MS;
        }
        return;
    }
    sense_service(now_ms);
}

bool pyro_is_firing(void) {
    return firing_channel != 0;
}

bool pyro_fault(uint8_t channel) {
    /* Q6/Q1 are plain dual MOSFETs and Q2 a plain AO3400A -- no FLAG or fault
     * output exists anywhere on this board, so there is nothing to report.
     * Compare MK1B, whose AP2192 does provide one. */
    (void)channel;
    return false;
}
