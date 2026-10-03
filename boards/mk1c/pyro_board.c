/*
 * Pyro backend -- Pyro MK1C: a TPS259570 eFuse high side armed by a charge
 * pump, a low-side gate per channel, and a biased-bus presence test. See
 * THEORY_OF_OPERATION.md "Firing bus".
 *
 * Between fires the board checks presence and shorts, nothing else
 * [DD-055], and what it finds is reported and gates nothing [DD-081]. A fire
 * is the sequence in pyro_sequence.c [DD-056]. Only that sequence raises a
 * gate or feeds the arm pump.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "arm_pump.h"
#include "board_if.h"
#include "board_pins.h"
#include "board_support.h"
#include "flash_op.h"
#include "pyro_faults.h"
#include "pyro_measure.h"
#include "pyro_sense.h"
#include "pyro_sequence.h"
#include <stdio.h>

extern void hal_telemetry_send(const char *sentence);

#define STATUS_REPORT_MS 5000

static const uint8_t pyro_outputs[] = {
    BOARD_PIN_ARM_TOGGLE, BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B, BOARD_PIN_BIAS_A, BOARD_PIN_BIAS_B, BOARD_PIN_BIAS_BUS,
};

static quiescent_t quiescent;
static uint32_t last_report_ms;

void board_early_init(void) {
    DRIVE_ALL_LOW(pyro_outputs);
}

bool board_pyro_raw(board_pyro_raw_t *out) {
    const tracking_t *t = tracking_result();
    out->bus_quiescent = quiescent.bus;
    out->bus_biased = t->valid ? t->bus : 0;
    out->vbat = quiescent.vbat;
    return true;
}

/* See THEORY_OF_OPERATION.md "Flash and the fire". */
bool board_flash_ok(void) {
    return !sequence_firing();
}

static void on_tracking_result(const tracking_t *t) {
    faults_count_bus_short(t);
    sequence_verify_fired(t);
}

static void report_status(uint32_t now_ms) {
    if ((int32_t)(now_ms - last_report_ms) < STATUS_REPORT_MS)
        return;
    last_report_ms = now_ms;
    const tracking_t *t = tracking_result();
    char line[160];
    snprintf(line, sizeof(line),
             "!PYRO q[vbat=%u bus=%u a=%u b=%u] trk[bus=%u a=%u b=%u] vbat=%lumV seq=%u flt=0x%02x\r\n", quiescent.vbat,
             quiescent.bus, quiescent.a, quiescent.b, t->bus, t->a, t->b,
             (unsigned long)counts_to_node_mv(quiescent.vbat), (unsigned)sequence_current_step(),
             (unsigned)faults_latched());
    hal_telemetry_send(line);
}

/* ── pyro.h ───────────────────────────────────────────────────────── */

void pyro_init(void) {
    DRIVE_ALL_LOW(pyro_outputs);
    adc_init();
    adc_gpio_init(BOARD_PIN_SNS_VBAT);
    adc_gpio_init(BOARD_PIN_SNS_BUS);
    adc_gpio_init(BOARD_PIN_SNS_A);
    adc_gpio_init(BOARD_PIN_SNS_B);
    arm_pump_init();
    arm_pump_stop();
    faults_init();
    tracking_init();
    sequence_init();
    last_report_ms = 0;
    read_quiescent(&quiescent);
}

void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    const tracking_t *t = tracking_result();
    uint16_t counts = t->valid ? (channel == 1 ? t->a : t->b) : 0;
    bool known = t->valid && !sequence_fired_since_tracking(channel);
    track_t verdict = known ? track_channel(counts, t->bus) : TRACK_INVALID;
    out->raw_adc = counts;
    out->open = verdict != TRACK_PRESENT;
    out->good = verdict == TRACK_PRESENT;
    out->shorted = false; /* a short is the bus's: pyro_fault() */
}

/* [PYR-FIRE-01, PYR-HEALTH-01] No reading withholds a fire. One sequence
 * runs at a time: the caller serialises the channels [PYR-DEPLOY-02]. */
void pyro_fire(uint8_t channel) {
    if ((channel != 1 && channel != 2) || sequence_firing())
        return;
    tracking_abandon();
    sequence_arm(channel, &quiescent);
}

void pyro_update(uint32_t now_ms) {
    read_quiescent(&quiescent);
    sequence_step(now_ms, &quiescent);
    if (sequence_current_step() == STEP_IDLE && tracking_step(now_ms))
        on_tracking_result(tracking_result());
    faults_watch_bus_hot(&quiescent, sequence_charged_the_bus(now_ms));
    report_status(now_ms);
}

bool pyro_is_firing(void) {
    return sequence_firing();
}

bool pyro_fault(uint8_t channel) {
    (void)channel; /* the bus's faults are both channels' */
    return faults_latched() != FAULT_NONE;
}
