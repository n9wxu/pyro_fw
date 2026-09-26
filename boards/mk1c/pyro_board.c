/*
 * Pyro backend — Pyro MK1C.
 *
 * Implements src/pyro.h for the MK1C firing architecture: a TPS259570 eFuse
 * high side, bias-injection continuity sensing, and four divided analog
 * sense channels.
 *
 * Specifications live with the board design, not in this repo:
 *   ~/Documents/pyro_mk1c/DESIGN.md            hardware, levels, FMEA, invariants
 *   ~/Documents/pyro_mk1c/IGNITER_OPERATION.md S0-S8 and F0-F10 state machines
 *
 * [DD-055] The firmware checks two things: that each pyro is present (T2,
 * the bus-bias tracking test) and that nothing is shorted (the latches
 * below). It runs no other check on the hardware. Firing is not implemented.
 *
 * SAFETY: pyro_safe_all_outputs() drives ARM_TOGGLE, FIRE_A and FIRE_B low,
 * and no code path here raises them again. That absence is what makes T2
 * safe to run continuously, and it is what DESIGN.md invariants 13a and 13b
 * require.
 *
 * The fault latch below implements DESIGN.md invariants 8 and 9.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "pyro_sense.h"
#include "board_if.h"
#include "board_pins.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include <stdio.h>

extern void hal_telemetry_send(const char *sentence);

/* ── Sense scaling ────────────────────────────────────────────────── */

/* All four dividers are 4.99k/(10k+4.99k) = 49.9k/(100k+49.9k) = 0.3329.
 * At 12 bits against a 3.3 V reference:
 *     3300mV / 4095 counts / 0.3329 = 2.421 mV of NODE voltage per count,
 * i.e. 413 counts per volt at the node.
 *
 * Reproduces every level in DESIGN.md 4. The anchors below fail the build if
 * a divider value or the reference changes. */
#define NODE_UV_PER_COUNT 2421

uint32_t pyro_counts_to_node_mv(uint16_t counts) {
    return ((uint32_t)counts * NODE_UV_PER_COUNT) / 1000u;
}

/* DESIGN.md 4. */
_Static_assert((1058u * NODE_UV_PER_COUNT) / 1000u >= 2550 && (1058u * NODE_UV_PER_COUNT) / 1000u <= 2570,
               "bus bias, no match: 1058 counts should be ~2.56 V");
_Static_assert((1214u * NODE_UV_PER_COUNT) / 1000u >= 2930 && (1214u * NODE_UV_PER_COUNT) / 1000u <= 2950,
               "channel bias, match off: 1214 counts should be ~2.94 V");
_Static_assert((1037u * NODE_UV_PER_COUNT) / 1000u >= 2500 && (1037u * NODE_UV_PER_COUNT) / 1000u <= 2520,
               "shorted TVS / match present: 1037 counts should be ~2.51 V");
_Static_assert((3469u * NODE_UV_PER_COUNT) / 1000u >= 8390 && (3469u * NODE_UV_PER_COUNT) / 1000u <= 8410,
               "full 2S bus: 3469 counts should be ~8.4 V");

/* ── Levels, in ADC counts (DD-054) ──────────────────────────────
 *
 * The bench MK1C, not DESIGN.md 4. U9's OUT conducts back into the part
 * above about 0.72 V, so the bus sits far lower under bias than the divider
 * algebra says (688 counts, not 1058) and moves with the part and its
 * temperature. So nothing decides on the bus's level: presence is the
 * channel against the bus in the same test (pyro_sense.h). */
#define CNT_QUIESCENT_MAX 50 /* a cold, unbiased node */

/* ── Tracking test timing (DESIGN.md S3) ─────────────────────────── */

#define TRACK_BIAS_MS 8     /* 5-10 ms of bias per measurement */
#define TRACK_PERIOD_MS 500 /* duty-cycled: one test per few hundred ms */

/* ── Fault latch ──────────────────────────────────────────────────── */

#define FAULT_CONFIRM_N 3 /* invariant 8: N consecutive agreeing samples */

typedef enum {
    PF_NONE = 0,
    PF_BUS_HOT = 1u << 0,      /* bus at pack voltage with nothing armed    */
    PF_BUS_SHORT_GND = 1u << 1 /* bus will not rise under its own bias      */
} pyro_fault_bits_t;

static uint8_t fault_latch;   /* pyro_fault_bits_t, latched (invariant 4) */
static uint8_t bus_hot_run;   /* consecutive agreeing samples */
static uint8_t bus_short_run; /* consecutive agreeing samples */

/* ── Sense state ──────────────────────────────────────────────────── */

static uint16_t sns_vbat, sns_bus, sns_a, sns_b; /* latest quiescent (T1) */
static uint16_t trk_bus, trk_a, trk_b;           /* latest tracking (T2)  */
static bool trk_valid;
static bool trk_biased; /* BIAS_BUS is on, settling */
static uint32_t trk_due_ms;
static uint32_t last_report_ms;

static uint16_t adc_sample(uint8_t channel) {
    adc_select_input(channel);
    return (uint16_t)adc_read(); /* raw 12-bit, 0-4095 */
}

/* Median of 3, per DESIGN.md 7.1 step 3. */
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

/* ── Output safing ────────────────────────────────────────────────── */

/* Called from board_early_init() before any slow initialisation, and again
 * from pyro_init(). Nothing else in the tree writes FIRE_A, FIRE_B or
 * ARM_TOGGLE. */
void pyro_safe_all_outputs(void) {
    static const uint8_t outputs[] = {
        BOARD_PIN_ARM_TOGGLE, BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B,
        BOARD_PIN_BIAS_A,     BOARD_PIN_BIAS_B, BOARD_PIN_BIAS_BUS,
    };
    for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        gpio_init(outputs[i]);
        gpio_put(outputs[i], 0); /* set the level before the direction */
        gpio_set_dir(outputs[i], GPIO_OUT);
        gpio_put(outputs[i], 0);
    }
}

/* ── T1: quiescent read ───────────────────────────────────────────
 *
 * No stimulus. Free, so it runs on every update; the bus-hot latch reads
 * it. */
static void t1_quiescent(void) {
    sns_vbat = adc_median3(BOARD_ADC_CH_VBAT);
    sns_bus = adc_median3(BOARD_ADC_CH_BUS);
    sns_a = adc_median3(BOARD_ADC_CH_A);
    sns_b = adc_median3(BOARD_ADC_CH_B);
}

/* ── T2: bus-bias tracking test (DESIGN.md S3) ────────────────────
 *
 * Asserts BIAS_BUS only, and never arms the bus. Bridgewire current is about
 * 0.11 mA on the bench (DD-054), far below a 100 mA no-fire current. A
 * channel following the bus has a match across it; one near zero is open or
 * absent (pyro_sense.h). The raw counts are reported, not just the verdict.
 *
 * The settle is a deadline the loop checks, never a wait [DD-053]. */
static void track_service(uint32_t now_ms) {
    if ((int32_t)(now_ms - trk_due_ms) < 0)
        return;
    if (!trk_biased) {
        gpio_put(BOARD_PIN_BIAS_BUS, 1);
        trk_biased = true;
        trk_due_ms = now_ms + TRACK_BIAS_MS;
        return;
    }
    trk_bus = adc_median3(BOARD_ADC_CH_BUS);
    trk_a = adc_median3(BOARD_ADC_CH_A);
    trk_b = adc_median3(BOARD_ADC_CH_B);
    gpio_put(BOARD_PIN_BIAS_BUS, 0);
    trk_biased = false;
    trk_valid = true;
    trk_due_ms = now_ms + TRACK_PERIOD_MS - TRACK_BIAS_MS;
}

/* ── Short circuits (DESIGN.md 8.1) ───────────────────────────────
 *
 * The two rows of 8.1 that are safety decisions: a high side shorted to the
 * pack, and a bus that cannot rise because something shorts it to ground --
 * the bus itself, a harness lead, or a shorted low-side FET behind a fitted
 * match. Both latch; nothing clears them but a reset (invariants 4 and 6). */
static void evaluate_faults(void) {
    bool hot = (sns_vbat > 200u) && (sns_bus > (uint16_t)(sns_vbat - (sns_vbat / 4u)));
    bus_hot_run = hot ? (uint8_t)(bus_hot_run + 1) : 0;
    if (bus_hot_run >= FAULT_CONFIRM_N) {
        bus_hot_run = FAULT_CONFIRM_N;
        fault_latch |= PF_BUS_HOT;
    }

    if (trk_valid) {
        bool shorted = trk_bus < CNT_QUIESCENT_MAX;
        bus_short_run = shorted ? (uint8_t)(bus_short_run + 1) : 0;
        if (bus_short_run >= FAULT_CONFIRM_N) {
            bus_short_run = FAULT_CONFIRM_N;
            fault_latch |= PF_BUS_SHORT_GND;
        }
    }
}

/* The quiescent bus says whether the high side has shorted; the biased bus,
 * whether anything shorts it to ground. */
bool pyro_raw_sense(board_pyro_raw_t *out) {
    out->bus_quiescent = sns_bus;
    out->bus_biased = trk_valid ? trk_bus : 0;
    out->vbat = sns_vbat;
    return true;
}

/* ── pyro.h implementation ────────────────────────────────────────── */

void pyro_init(void) {
    pyro_safe_all_outputs();

    adc_init();
    adc_gpio_init(BOARD_PIN_SNS_VBAT);
    adc_gpio_init(BOARD_PIN_SNS_BUS);
    adc_gpio_init(BOARD_PIN_SNS_A);
    adc_gpio_init(BOARD_PIN_SNS_B);

    fault_latch = PF_NONE;
    bus_hot_run = bus_short_run = 0;
    trk_valid = false;
    trk_biased = false;
    trk_due_ms = 0;
    last_report_ms = 0;

    t1_quiescent();
    hal_telemetry_send("!PYRO MK1C sense-only build: firing not implemented\r\n");
}

/* Nothing to do: the tracking test runs continuously from pyro_update(), so
 * trk_* is never more than TRACK_PERIOD_MS old. */
void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    uint16_t counts = 0;
    if (channel == 1)
        counts = trk_valid ? trk_a : 0;
    else if (channel == 2)
        counts = trk_valid ? trk_b : 0;
    else
        return;

    /* A test whose bus never rose is no reading: not good, as with no
     * result yet. The raw count is reported either way. */
    track_t t = trk_valid ? track_channel(counts, trk_bus) : TRACK_INVALID;
    out->raw_adc = counts;
    out->open = t != TRACK_PRESENT;
    out->good = t == TRACK_PRESENT;
    out->shorted = false; /* a short is the bus's, reported by pyro_fault() */
}

void pyro_fire(uint8_t channel) {
    /* Asserting FIRE_x is reserved to the F0-F10 sequence (invariant 13a),
     * which does not exist in this build. Refuse loudly. */
    (void)channel;
    hal_telemetry_send("!PYRO FIRE REFUSED: firing not implemented on MK1C\r\n");
}

void pyro_update(uint32_t now_ms) {
    t1_quiescent();
    track_service(now_ms);
    evaluate_faults();

    if ((int32_t)(now_ms - last_report_ms) < 5000)
        return;
    last_report_ms = now_ms;

    char line[160];
    snprintf(line, sizeof(line), "!PYRO q[vbat=%u bus=%u a=%u b=%u] trk[bus=%u a=%u b=%u] vbat=%lumV flt=0x%02x\r\n",
             sns_vbat, sns_bus, sns_a, sns_b, trk_bus, trk_a, trk_b, (unsigned long)pyro_counts_to_node_mv(sns_vbat),
             (unsigned)fault_latch);
    hal_telemetry_send(line);
}

bool pyro_is_firing(void) {
    return false;
}

bool pyro_fault(uint8_t channel) {
    /* U9's ~FLT is not routed to the MCU on MK1C. The bus-level latches
     * apply to both channels. */
    (void)channel;
    return fault_latch != PF_NONE;
}
