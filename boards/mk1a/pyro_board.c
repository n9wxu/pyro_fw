/*
 * Pyro backend — Pyro MK1A.
 *
 * Implements src/pyro.h for the MK1A firing architecture: a per-channel
 * high-side MOSFET (Q6, Q1 DMC2053UVT) and ONE shared low-side MOSFET
 * (Q2 AO3400A) behind an 8 A fuse, with a filtered sense tap on each
 * igniter's low end.
 *
 * ===========================================================================
 * STAGE: quiescent sense only. Continuity and firing are NOT implemented.
 * ===========================================================================
 *
 * Two things must be settled on the bench before either can be written, and
 * neither can be read off the schematic with the confidence a pyro path
 * needs. Both are stated here rather than guessed at, because a wrong guess
 * is not a bug that shows up as a failed test -- it is an igniter that fires
 * during a self-check, or a continuity reading whose polarity is inverted so
 * an open channel reports good.
 *
 *   1. WHICH STIMULUS PROVES CONTINUITY WITHOUT RISKING IGNITION.
 *
 *      Current through an igniter needs its own high side AND the shared low
 *      side, so exactly one of them may be asserted at a time. But the two
 *      choices are not equivalent:
 *
 *        - low side alone (PYRO_LOW): pulls Initiator_ground to GND with
 *          both igniter tops floating. Safe, but the sense node then says
 *          nothing about whether a bridgewire is present.
 *        - high side alone (FIRE1/FIRE2): puts VBATT across the igniter into
 *          the sense tap's impedance. This does distinguish present from
 *          open -- but it means asserting a FIRE line outside a firing
 *          sequence, and its safety rests entirely on Q2 being genuinely off
 *          rather than leaking.
 *
 *      MK1B answers this by asserting only its shared element, because on
 *      that board the shared element is the HIGH side. Moving the shared
 *      element to the low side does not carry that answer across.
 *
 *   2. THE SENSE SCALING.
 *
 *      SENSE1/SENSE2 reach the ADC through a 1k series resistor with a 100nF
 *      filter to ground (R5/C6, R14/C5). No divider is visible on the
 *      schematic. If VBATT can exceed 3.3 V -- a 2S pack is 8.4 V -- then
 *      whatever stimulus is chosen must not present VBATT to an ADC pin, and
 *      the counts-to-volts relation is unknown until that is resolved.
 *
 * Until both are answered, pyro_sample() applies NO stimulus and pyro_get()
 * reports good == false. That is the reference template's rule -- report
 * good == false until continuity is genuinely proven -- and the flight logic
 * gates firing on it, so a board in this state cannot deploy. The raw
 * quiescent counts ARE reported, because they are what the bench needs to
 * answer question 2.
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

static uint16_t sns1, sns2;
static uint32_t last_report_ms;

/* Drive every pyro output inactive. Called from board_early_init() before
 * any slow initialisation, and again from pyro_init().
 *
 * This is the ONLY function in the tree that writes FIRE1, FIRE2 or
 * PYRO_LOW at this stage, and it only ever writes them low. */
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

/* Median of 3: cheap, and the sense taps are RC-filtered but not immune to
 * a switching transient elsewhere on the board. */
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

void pyro_init(void) {
    pyro_safe_all_outputs();

    adc_init();
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);

    last_report_ms = 0;
    hal_telemetry_send("!PYRO MK1A quiescent-sense build: continuity and firing not implemented\r\n");
}

/* No stimulus: both high sides and the shared low side stay low, so no
 * current can reach either bridgewire. Non-blocking, unlike MK1B's version,
 * because there is no settle to wait for when nothing is driven. */
void pyro_sample(void) {
    sns1 = adc_median3(BOARD_ADC_CH_SENSE1);
    sns2 = adc_median3(BOARD_ADC_CH_SENSE2);
}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;

    out->raw_adc = (channel == 1) ? sns1 : sns2;

    /* Deliberately not classified. See question 1 in the header: without a
     * stimulus these counts carry no continuity information, and reporting
     * anything other than "not good" here would let the flight logic deploy
     * on a number that means nothing. */
    out->good = false;
    out->open = true;
    out->shorted = false;
}

void pyro_fire(uint8_t channel) {
    /* Not implemented. Refuse loudly rather than silently doing nothing, so
     * a bench operator sees why. */
    (void)channel;
    hal_telemetry_send("!PYRO FIRE REFUSED: firing not implemented on MK1A\r\n");
}

void pyro_update(uint32_t now_ms) {
    pyro_sample();

    /* Bring-up telemetry. These are the numbers that answer question 2 in
     * the header comment: what the sense taps read with nothing driven. */
    if ((int32_t)(now_ms - last_report_ms) < 5000)
        return;
    last_report_ms = now_ms;

    char line[96];
    snprintf(line, sizeof(line), "!PYRO q[s1=%u s2=%u]\r\n", sns1, sns2);
    hal_telemetry_send(line);
}

bool pyro_is_firing(void) {
    return false;
}

bool pyro_fault(uint8_t channel) {
    /* Q6/Q1 are plain dual MOSFETs and Q2 is a plain AO3400A -- no FLAG or
     * fault output exists anywhere on this board, so there is nothing to
     * report. Compare MK1B, whose AP2192 does provide one. */
    (void)channel;
    return false;
}
