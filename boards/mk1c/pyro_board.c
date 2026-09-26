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
 * [DD-055] Between fires the firmware checks two things: that each pyro is
 * present (T2, the bus-bias tracking test) and that nothing is shorted (the
 * latches below). It runs no other check on the hardware.
 *
 * [DD-056] A fire is DESIGN.md 7.1 as loop steps: arm the bus with the charge
 * pump, fire when the bus reaches the measured pack, stop the pump, hold the
 * gate until the bus is flat, let the bleed drain it, and verify with the
 * next tracking test.
 *
 * SAFETY: pyro_safe_all_outputs() drives ARM_TOGGLE, FIRE_A and FIRE_B low.
 * Only the firing sequence raises them again, and only it feeds the pump.
 * T2 runs only while the bus is cold (invariants 1 and 7).
 *
 * The fault latch below implements DESIGN.md invariants 8 and 9.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "pyro_sense.h"
#include "arm_pump.h"
#include "board_if.h"
#include "board_pins.h"
#include "flash_window.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/time.h"
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

/* DESIGN.md S3's 5-10 ms settles the bench board's 1.1 uF, not C_BULK: the
 * bias charges it through 282 ohm (330 against the bus's 1.92k), so the bus
 * needs about 6 ms to pass TRACK_BUS_MIN_COUNTS at 100 uF and 130 ms at
 * 2200 uF. The bias is held until it does, this long at most; a bus still
 * below it then is shorted [DD-056]. The bridgewire sees 0.2 mA at most. */
#define TRACK_BIAS_MAX_MS 400

/* ── Firing (DESIGN.md 7.1, IGNITER_OPERATION.md F0-F10) ──────────── */

#define FIRE_AT_TENTHS 9   /* 7.1 step 4: bus at 90 % of the measured pack */
#define PUMP_TO_EN_US 400  /* 7.1 step 1                                   */
#define SLEW_MV_PER_MS 890 /* DESIGN.md 4: the dVdT ramp                   */
#define EN_COLLAPSE_MS 10  /* 5.1: U9 off within 9.6 ms of the pump stopping */
#define HOLD_MAX_MS 30     /* 7.1 step 8                                   */
#define FLAT_COUNTS 25     /* 60 mV at the node between two loops          */
#define UVLO_COUNTS 1239   /* 3.0 V: an empty 1S cell, above U9's own 2.5 V */

/* How long the bleed has to bring the bus below three quarters of the pack
 * after a fire: 1.2 s at 2200 uF, 2 s with one of R_BLEED's two resistors
 * open. A bus still hot after this is a high side that did not turn off. */
#define BLEED_BELOW_HOT_MS 5000

typedef enum {
    SEQ_IDLE,      /* S4: the tracking test runs                      */
    SEQ_PRECHARGE, /* F1-F2: the pump runs, the bus ramps             */
    SEQ_HOLD,      /* F3-F7: FIRE_x on, the pump stopped              */
    SEQ_DRAIN      /* F9: the gate open, the bleed drains the bus      */
} seq_t;

static seq_t seq;
static uint8_t seq_ch; /* 1 or 2 */
static uint32_t seq_arm_ms, seq_timeout_ms, seq_fire_ms, seq_disarm_ms;
static uint16_t hold_prev_bus;
static bool fired_since_track[2]; /* F10: the next tracking test verifies */

/* ── Fault latch ──────────────────────────────────────────────────── */

#define FAULT_CONFIRM_N 3 /* invariant 8: N consecutive agreeing samples */

typedef enum {
    PF_NONE = 0,
    PF_BUS_HOT = 1u << 0,          /* bus at pack voltage with nothing armed */
    PF_BUS_SHORT_GND = 1u << 1,    /* bus will not rise under its own bias   */
    PF_PRECHARGE_TIMEOUT = 1u << 2 /* 7.2: the bus did not reach the pack    */
} pyro_fault_bits_t;

static uint8_t fault_latch;   /* pyro_fault_bits_t, latched (invariant 4) */
static uint8_t bus_hot_run;   /* consecutive agreeing samples */
static uint8_t bus_short_run; /* consecutive agreeing samples */

/* ── Sense state ──────────────────────────────────────────────────── */

static uint16_t sns_vbat, sns_bus, sns_a, sns_b; /* latest quiescent (T1) */
static uint16_t trk_bus, trk_a, trk_b;           /* latest tracking (T2)  */
static bool trk_valid;
static bool trk_biased; /* BIAS_BUS is on, settling */
static uint32_t trk_start_ms, trk_due_ms;
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
static void tracking_sample_taken(void);

static void track_service(uint32_t now_ms) {
    if (!trk_biased) {
        if ((int32_t)(now_ms - trk_due_ms) < 0)
            return;
        gpio_put(BOARD_PIN_BIAS_BUS, 1);
        trk_biased = true;
        trk_start_ms = now_ms;
        trk_due_ms = now_ms + TRACK_PERIOD_MS;
        return;
    }
    uint32_t on_ms = now_ms - trk_start_ms;
    if (on_ms < TRACK_BIAS_MS)
        return;
    uint16_t bus = adc_median3(BOARD_ADC_CH_BUS);
    if (bus < TRACK_BUS_MIN_COUNTS && on_ms < TRACK_BIAS_MAX_MS)
        return; /* C_BULK still charging */
    trk_bus = bus;
    trk_a = adc_median3(BOARD_ADC_CH_A);
    trk_b = adc_median3(BOARD_ADC_CH_B);
    gpio_put(BOARD_PIN_BIAS_BUS, 0);
    trk_biased = false;
    trk_valid = true;
    tracking_sample_taken();
}

static void track_abandon(void) {
    gpio_put(BOARD_PIN_BIAS_BUS, 0);
    trk_biased = false;
}

/* ── Short circuits (DESIGN.md 8.1) ───────────────────────────────
 *
 * The two rows of 8.1 that are safety decisions: a high side shorted to the
 * pack, and a bus that cannot rise because something shorts it to ground --
 * the bus itself, a harness lead, or a shorted low-side FET behind a fitted
 * match. Both latch; nothing clears them but a reset (invariants 4 and 6).
 * A tracking test is one sample, however many loops read it. */
static bool bus_is_hot(void) {
    return (sns_vbat > 200u) && (sns_bus > (uint16_t)(sns_vbat - (sns_vbat / 4u)));
}

static bool bus_live_by_us(uint32_t now_ms);

static void evaluate_bus_hot(uint32_t now_ms) {
    bool hot = !bus_live_by_us(now_ms) && bus_is_hot();
    bus_hot_run = hot ? (uint8_t)(bus_hot_run + 1) : 0;
    if (bus_hot_run >= FAULT_CONFIRM_N) {
        bus_hot_run = FAULT_CONFIRM_N;
        fault_latch |= PF_BUS_HOT;
    }
}

/* F10 (7.1 step 11): the verdict on a fired channel. What follows it --
 * S8 on the ground, nothing in flight -- is the flight code's. */
static void verify_fired(uint8_t ch, uint16_t counts) {
    track_t t = track_channel(counts, trk_bus);
    char line[112];
    snprintf(line, sizeof(line), "!PYRO F10 ch=%u %s (%u, bus %u)\r\n", ch,
             t == TRACK_OPEN      ? "open: fired"
             : t == TRACK_PRESENT ? "still present: misfire, treat as live"
                                  : "unverified: the bus did not rise",
             counts, trk_bus);
    hal_telemetry_send(line);
}

static void tracking_sample_taken(void) {
    bool shorted = trk_bus < TRACK_BUS_MIN_COUNTS;
    bus_short_run = shorted ? (uint8_t)(bus_short_run + 1) : 0;
    if (bus_short_run >= FAULT_CONFIRM_N) {
        bus_short_run = FAULT_CONFIRM_N;
        fault_latch |= PF_BUS_SHORT_GND;
    }
    for (uint8_t i = 0; i < 2; i++) {
        if (fired_since_track[i]) {
            fired_since_track[i] = false;
            verify_fired((uint8_t)(i + 1), i == 0 ? trk_a : trk_b);
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

/* ── The firing sequence ──────────────────────────────────────────
 *
 * Each step is one loop iteration; nothing waits [DD-053]. The pump is fed
 * only from the precharge step, after its re-check, so the toggle is a
 * byproduct of the checks (invariant 5), and a loop that stops stops it. */
static const uint8_t fire_pin[2] = {BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B};

static void report_fire(const char *what, uint8_t ch) {
    char line[112];
    snprintf(line, sizeof(line), "!PYRO %s ch=%u bus=%u vbat=%u\r\n", what, ch, sns_bus, sns_vbat);
    hal_telemetry_send(line);
}

/* 7.2: 1.5 times the pump and the ramp to 90 % of the measured pack. */
static uint32_t precharge_timeout_ms(void) {
    uint32_t us = PUMP_TO_EN_US + (pyro_counts_to_node_mv(sns_vbat) * FIRE_AT_TENTHS * 100u) / SLEW_MV_PER_MS;
    return (us * 3u / 2u + 999u) / 1000u;
}

static bool bus_charged(void) {
    return 10u * (uint32_t)sns_bus >= FIRE_AT_TENTHS * (uint32_t)sns_vbat;
}

/* A bus at the pack because the sequence put it there. */
static bool bus_live_by_us(uint32_t now_ms) {
    if (seq == SEQ_PRECHARGE || seq == SEQ_HOLD)
        return true;
    return seq == SEQ_DRAIN && (int32_t)(now_ms - seq_disarm_ms) < BLEED_BELOW_HOT_MS;
}

/* 7.1's preconditions. The bus may be live only because a fire just left
 * it charged: a second event does not wait for the drain (5.3). */
static const char *fire_refusal(uint8_t ch) {
    if (ch != 1 && ch != 2)
        return "no such channel";
    if (seq == SEQ_PRECHARGE || seq == SEQ_HOLD)
        return "a fire is in progress";
    if (fault_latch != PF_NONE)
        return "latched fault";
    if (!trk_valid)
        return "no tracking result yet";
    if (fired_since_track[ch - 1])
        return "no tracking test since this channel fired";
    if (track_channel(ch == 1 ? trk_a : trk_b, trk_bus) != TRACK_PRESENT)
        return "the channel does not read present";
    if (sns_vbat < UVLO_COUNTS)
        return "pack below UVLO";
    if (seq == SEQ_IDLE && bus_is_hot())
        return "the bus is live with nothing armed";
    return NULL;
}

static void disarm(uint32_t now_ms) {
    arm_pump_stop(); /* C_HOLD bleeds; U9 turns itself off */
    seq_disarm_ms = now_ms;
}

static void sequence_service(uint32_t now_ms) {
    switch (seq) {
    case SEQ_PRECHARGE:
        if (fault_latch != PF_NONE) {
            disarm(now_ms);
            seq = SEQ_DRAIN;
            report_fire("ABORT", seq_ch);
        } else if (bus_charged()) {
            /* 7.1 steps 4 and 7 together: U9 stays on for the 9.6 ms its
             * enable takes to bleed, which carries the pulse. */
            gpio_put(fire_pin[seq_ch - 1], 1);
            disarm(now_ms);
            seq_fire_ms = now_ms;
            hold_prev_bus = sns_bus;
            seq = SEQ_HOLD;
            report_fire("FIRE", seq_ch);
        } else if (now_ms - seq_arm_ms >= seq_timeout_ms) {
            disarm(now_ms);
            fault_latch |= PF_PRECHARGE_TIMEOUT;
            seq = SEQ_DRAIN;
            report_fire("ABORT precharge timeout", seq_ch);
        } else {
            arm_pump_feed();
        }
        break;
    case SEQ_HOLD: {
        /* 7.1 steps 8-9: the gate opens at about 0 A, once U9 is off and the
         * bus has stopped moving, or at the limit. */
        uint32_t held = now_ms - seq_fire_ms;
        uint16_t d = sns_bus > hold_prev_bus ? sns_bus - hold_prev_bus : hold_prev_bus - sns_bus;
        hold_prev_bus = sns_bus;
        if ((held >= EN_COLLAPSE_MS && d <= FLAT_COUNTS) || held >= HOLD_MAX_MS) {
            gpio_put(fire_pin[seq_ch - 1], 0);
            fired_since_track[seq_ch - 1] = true;
            seq = SEQ_DRAIN;
        }
        break;
    }
    case SEQ_DRAIN:
        if (sns_bus <= CNT_QUIESCENT_MAX) {
            seq = SEQ_IDLE;
            trk_due_ms = now_ms; /* F10: verify now */
        }
        break;
    case SEQ_IDLE:
        break;
    }
}

bool board_flash_ok(void) {
    return !pyro_is_firing();
}

/* ── pyro.h implementation ────────────────────────────────────────── */

void pyro_init(void) {
    pyro_safe_all_outputs();

    adc_init();
    adc_gpio_init(BOARD_PIN_SNS_VBAT);
    adc_gpio_init(BOARD_PIN_SNS_BUS);
    adc_gpio_init(BOARD_PIN_SNS_A);
    adc_gpio_init(BOARD_PIN_SNS_B);

    arm_pump_init();
    arm_pump_stop();

    fault_latch = PF_NONE;
    bus_hot_run = bus_short_run = 0;
    trk_valid = false;
    trk_biased = false;
    trk_due_ms = 0;
    last_report_ms = 0;
    seq = SEQ_IDLE;
    fired_since_track[0] = fired_since_track[1] = false;

    t1_quiescent();
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
     * result yet, or a channel fired since. The raw count is reported
     * either way. */
    bool known = trk_valid && !fired_since_track[channel - 1];
    track_t t = known ? track_channel(counts, trk_bus) : TRACK_INVALID;
    out->raw_adc = counts;
    out->open = t != TRACK_PRESENT;
    out->good = t == TRACK_PRESENT;
    out->shorted = false; /* a short is the bus's, reported by pyro_fault() */
}

void pyro_fire(uint8_t channel) {
    const char *why = fire_refusal(channel);
    if (why) {
        char line[112];
        snprintf(line, sizeof(line), "!PYRO FIRE REFUSED ch=%u: %s\r\n", channel, why);
        hal_telemetry_send(line);
        return;
    }
    /* F0: the bias off, and off while the bus is live (invariant 1). */
    track_abandon();
    /* F1 */
    seq_ch = channel;
    seq_arm_ms = to_ms_since_boot(get_absolute_time());
    seq_timeout_ms = precharge_timeout_ms();
    seq = SEQ_PRECHARGE;
    arm_pump_start();
    arm_pump_feed();
    report_fire("ARM", channel);
}

void pyro_update(uint32_t now_ms) {
    t1_quiescent();
    sequence_service(now_ms);
    if (seq == SEQ_IDLE)
        track_service(now_ms);
    evaluate_bus_hot(now_ms);

    if ((int32_t)(now_ms - last_report_ms) < 5000)
        return;
    last_report_ms = now_ms;

    char line[160];
    snprintf(line, sizeof(line),
             "!PYRO q[vbat=%u bus=%u a=%u b=%u] trk[bus=%u a=%u b=%u] vbat=%lumV seq=%u flt=0x%02x\r\n", sns_vbat,
             sns_bus, sns_a, sns_b, trk_bus, trk_a, trk_b, (unsigned long)pyro_counts_to_node_mv(sns_vbat),
             (unsigned)seq, (unsigned)fault_latch);
    hal_telemetry_send(line);
}

bool pyro_is_firing(void) {
    return seq == SEQ_PRECHARGE || seq == SEQ_HOLD;
}

bool pyro_fault(uint8_t channel) {
    /* U9's ~FLT is not routed to the MCU on MK1C. The bus-level latches
     * apply to both channels. */
    (void)channel;
    return fault_latch != PF_NONE;
}
