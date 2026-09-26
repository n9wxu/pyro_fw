/*
 * Tests for the board plant models in sim/plant/.
 *
 * Two different jobs, deliberately kept apart:
 *
 *   ASSERTIONS   The model must reproduce the levels and time constants
 *                that the board files and DESIGN.md state independently of
 *                it. These are the model's acceptance test. If one fails,
 *                the model is wrong -- or a component value changed under
 *                it, which is the other thing worth being told about.
 *
 *   REPORTS      What the model says about the firmware's own thresholds
 *                and settle times, and which injected faults the firmware
 *                detects. These PRINT rather than assert, because the
 *                answer is a finding about the firmware, not a property of
 *                the model, and freezing today's answer into an assertion
 *                would make the finding invisible the moment it is fixed.
 *
 * Built by the `plant_tests` target, which compiles each plant separately
 * against its own board's board_pins.h and links all three.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "plant.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Pin numbers, taken from each board's board_pins.h. Spelled out rather
 * than included, because all three headers cannot be on one include path
 * at once -- they define the same macros with different values. */
enum { A_FIRE1 = 9, A_LOW = 10, A_FIRE2 = 11, A_ADC1 = 0, A_ADC2 = 1 };
enum { B_COMMON_EN = 15, B_FLAG1 = 17, B_FLAG2 = 18, B_EN1 = 21, B_EN2 = 22 };
enum { C_ARM = 12, C_BIAS_A = 16, C_FIRE_A = 17, C_BIAS_BUS = 23,
       C_FIRE_B = 24, C_BIAS_B = 25 };
enum { C_ADC_VBAT = 0, C_ADC_BUS = 1, C_ADC_A = 2, C_ADC_B = 3 };

/* Firmware constants, from the board files. Repeated here so a test failure
 * names the number the firmware actually uses. */
#define MK1C_CNT_QUIESCENT_MAX 50
#define MK1C_TRACK_BIAS_MS       8

/* The bench MK1C, 2026-09-26: its ADC and a scope at CN1 (DD-054). U9's OUT
 * conducts back into the part above about 0.72 V, so the bus sits far below
 * DESIGN.md 4's 1058 counts, and the model is held to the board. */
#define MK1C_BENCH_BUS_BIASED   688  /* ADC 685 and 690                    */
#define MK1C_BENCH_CH_ISOLATED 1262  /* ADC 1258 (A) and 1266 (B)          */

#define MK1A_CNT_PATH_MAX      500
#define MK1A_CNT_OPEN_MIN     3000
#define MK1A_SETTLE_MS          50

#define MK1B_OPEN_THRESHOLD   3800
#define MK1B_SHORT_THRESHOLD    50
#define MK1B_SAMPLE_SLEEP_MS    10

static void ms(double t) { plant_step(t / 1000.0); }

/* The tabulated levels are quoted to the nearest count but derived from
 * values given to three figures, so agreement to 1 % is agreement. */
static void assert_counts_near(int expect, int got, const char *what) {
    int tol = (int)(expect * 0.01) + 2;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: expected about %d counts, model gives %d", what, expect, got);
    TEST_ASSERT_INT_WITHIN_MESSAGE(tol, expect, got, msg);
}

void setUp(void) {}
void tearDown(void) {}

/* ══════════════════ MK1C: the board as measured ═══════════════════ */

#include "../boards/mk1c/pyro_sense.h"

/* Measured levels scatter by a few counts between reads and boards. */
static void assert_bench_near(int expect, int got, int pct, const char *what) {
    int tol = expect * pct / 100;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: the bench read about %d counts, model gives %d", what, expect, got);
    TEST_ASSERT_INT_WITHIN_MESSAGE(tol, expect, got, msg);
}

static void mk1c_start(void) {
    plant_init(PLANT_MK1C);
    plant_set_pack_mv(8400);          /* 2S, variant B */
    plant_match(1)->state = MATCH_ABSENT;
    plant_match(2)->state = MATCH_ABSENT;
    ms(50);
}

static void test_mk1c_quiescent_is_cold(void) {
    mk1c_start();
    TEST_ASSERT_LESS_THAN_UINT16(MK1C_CNT_QUIESCENT_MAX, plant_adc_counts(C_ADC_BUS));
    TEST_ASSERT_LESS_THAN_UINT16(MK1C_CNT_QUIESCENT_MAX, plant_adc_counts(C_ADC_A));
    TEST_ASSERT_LESS_THAN_UINT16(MK1C_CNT_QUIESCENT_MAX, plant_adc_counts(C_ADC_B));
}

static void test_mk1c_vbat_divider(void) {
    mk1c_start();
    /* DESIGN.md 4: a 2S pack at 8.4 V is 3469 counts through 0.333. */
    assert_counts_near(3469, plant_adc_counts(C_ADC_VBAT), "SNS_VBAT at 8.4 V");
}

static void test_mk1c_bus_bias_no_match(void) {
    mk1c_start();
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    assert_bench_near(MK1C_BENCH_BUS_BIASED, plant_adc_counts(C_ADC_BUS), 3, "T2 bus bias, no match");
}

static void test_mk1c_channel_bias_match_off(void) {
    mk1c_start();
    plant_set_gpio(C_BIAS_A, 1);
    ms(MK1C_TRACK_BIAS_MS);
    assert_bench_near(MK1C_BENCH_CH_ISOLATED, plant_adc_counts(C_ADC_A), 2, "T3 channel bias, match off");
}

/* The scope's fall after the bias lets go, 2026-09-26: fast above U9's knee,
 * then the designed pull-down's time constant below it. A linear bus, as
 * DESIGN.md 4 drew it, takes about 2 ms just to reach 0.9 V. */
static void test_mk1c_bus_fall_as_measured(void) {
    mk1c_start();
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    plant_set_gpio(C_BIAS_BUS, 0);
    plant_probe_t pr;
    double t = 0.0, t09 = -1.0, t06 = -1.0, t02 = -1.0;
    while (t < 8e-3) {
        plant_step(5e-6);
        t += 5e-6;
        plant_probe(&pr);
        if (t09 < 0 && pr.bus_v <= 0.9) t09 = t;
        if (t06 < 0 && pr.bus_v <= 0.6) t06 = t;
        if (t02 < 0 && pr.bus_v <= 0.2) t02 = t;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "plateau to 0.9 V in %.0f us; the scope saw about 420", t09 * 1e6);
    TEST_ASSERT_TRUE_MESSAGE(t09 > 300e-6 && t09 < 600e-6, msg);
    snprintf(msg, sizeof(msg), "0.6 V to 0.2 V in %.2f ms; the scope saw 2.32", (t02 - t06) * 1e3);
    TEST_ASSERT_TRUE_MESSAGE(t02 - t06 > 1.9e-3 && t02 - t06 < 2.7e-3, msg);
}

/* T3 cannot tell a connected match from a shorted TVS (DESIGN.md 4), and on
 * this board both tie the channel to a bus U9 holds low, far below the
 * isolated level: the separation T3's check relies on. */
static void test_mk1c_channel_bias_match_fitted(void) {
    mk1c_start();
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    ms(10);
    plant_set_gpio(C_BIAS_A, 1);
    ms(MK1C_TRACK_BIAS_MS);
    uint16_t loaded = plant_adc_counts(C_ADC_A);
    TEST_ASSERT_TRUE_MESSAGE(loaded + 300 < MK1C_BENCH_CH_ISOLATED, "a fitted match must pull the channel well down");
    TEST_ASSERT_TRUE_MESSAGE(loaded < 1000, "and below the T3 threshold that refuses to arm");
}

static void test_mk1c_shorted_tvs_reads_like_a_match(void) {
    mk1c_start();
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    ms(10);
    plant_set_gpio(C_BIAS_A, 1);
    ms(MK1C_TRACK_BIAS_MS);
    uint16_t match = plant_adc_counts(C_ADC_A);
    mk1c_start();
    plant_set_fault(PF_TVS_A_SHORT, true);
    plant_set_gpio(C_BIAS_A, 1);
    ms(MK1C_TRACK_BIAS_MS);
    assert_counts_near(match, plant_adc_counts(C_ADC_A), "T3 with a shorted TVS against a fitted match");
}

/* [DESIGN.md S3] A present channel follows the bus; an open one stays cold. */
static void test_mk1c_tracking_separates_present_from_open(void) {
    mk1c_start();
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = MATCH_ABSENT;
    ms(10);
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    uint16_t bus = plant_adc_counts(C_ADC_BUS);
    TEST_ASSERT_EQUAL(TRACK_PRESENT, track_channel(plant_adc_counts(C_ADC_A), bus));
    TEST_ASSERT_EQUAL(TRACK_OPEN, track_channel(plant_adc_counts(C_ADC_B), bus));
}

/* Two matches load the bus by another 7.5k, which U9's path dwarfs. */
static void test_mk1c_bus_bias_two_matches(void) {
    mk1c_start();
    plant_match(1)->state = MATCH_PRESENT; plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = MATCH_PRESENT; plant_match(2)->r_ohm = 1.0;
    ms(10);
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    uint16_t bus = plant_adc_counts(C_ADC_BUS);
    assert_bench_near(MK1C_BENCH_BUS_BIASED, bus, 5, "T2 bus bias, two matches");
    TEST_ASSERT_EQUAL(TRACK_PRESENT, track_channel(plant_adc_counts(C_ADC_A), bus));
    TEST_ASSERT_EQUAL(TRACK_PRESENT, track_channel(plant_adc_counts(C_ADC_B), bus));
}

/* A bus that will not rise says nothing about the channels. */
static void test_mk1c_tracking_needs_the_bus_to_rise(void) {
    mk1c_start();
    plant_set_fault(PF_BUS_SHORT_GND, true);
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    TEST_ASSERT_EQUAL(TRACK_INVALID, track_channel(plant_adc_counts(C_ADC_A), plant_adc_counts(C_ADC_BUS)));
}

/* Presence is a ratio to the bus read in the same test, so it holds wherever
 * the bus sits: DESIGN's 1058, the bench's 688, and half that, which the
 * absolute 400-count threshold it replaces would read as open. */
static void test_mk1c_presence_is_a_ratio(void) {
    const uint16_t buses[] = {1058, 688, 344, 250};
    for (unsigned i = 0; i < sizeof(buses) / sizeof(buses[0]); i++) {
        uint16_t bus = buses[i];
        char msg[64];
        snprintf(msg, sizeof(msg), "bus at %u counts", bus);
        TEST_ASSERT_EQUAL_MESSAGE(TRACK_PRESENT, track_channel(bus - 3, bus), msg);             /* a match */
        TEST_ASSERT_EQUAL_MESSAGE(TRACK_PRESENT, track_channel(bus * 3 / 4, bus), msg);         /* 5k in the leads */
        TEST_ASSERT_EQUAL_MESSAGE(TRACK_OPEN, track_channel(MK1C_CNT_QUIESCENT_MAX - 1, bus), msg);
    }
    TEST_ASSERT_EQUAL(TRACK_INVALID, track_channel(10, TRACK_BUS_MIN_COUNTS - 1));
}

static void test_mk1c_tracking_current_is_far_below_no_fire(void) {
    /* DESIGN.md 4: tracking is about 0.2 mA, 500x below the 100 mA no-fire
     * current of M2. This is a safety claim, so the model owes an
     * assertion and not a printout. */
    mk1c_start();
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    ms(10);
    plant_set_gpio(C_BIAS_BUS, 1);
    ms(MK1C_TRACK_BIAS_MS);
    double i_ma = fabs(plant_match(1)->last_i_a) * 1000.0;
    char msg[128];
    snprintf(msg, sizeof(msg), "tracking current %.3f mA must stay below 1 mA", i_ma);
    TEST_ASSERT_TRUE_MESSAGE(i_ma < 1.0, msg);
    TEST_ASSERT_FALSE_MESSAGE(plant_match(1)->fired, "the tracking test must never light a match");
}

static void test_mk1c_precharge_follows_the_dvdt_slew(void) {
    /* DESIGN.md 4: 0.89 V/ms, independent of the capacitance. */
    mk1c_start();
    plant_probe_t p0, p1;

    for (int c = 0; c < 20; c++) {   /* 2 ms of pump: U9 is on and ramping */
        plant_set_gpio(C_ARM, 1); plant_step(50e-6);
        plant_set_gpio(C_ARM, 0); plant_step(50e-6);
    }
    plant_probe(&p0);
    TEST_ASSERT_TRUE_MESSAGE(p0.armed, "the charge pump should have enabled U9 within 2 ms");

    for (int c = 0; c < 20; c++) {   /* another 2 ms */
        plant_set_gpio(C_ARM, 1); plant_step(50e-6);
        plant_set_gpio(C_ARM, 0); plant_step(50e-6);
    }
    plant_probe(&p1);

    double slew_v_per_ms = (p1.bus_v - p0.bus_v) / 2.0;
    char msg[128];
    snprintf(msg, sizeof(msg), "precharge slew %.3f V/ms, DESIGN.md says 0.89", slew_v_per_ms);
    TEST_ASSERT_TRUE_MESSAGE(fabs(slew_v_per_ms - 0.89) < 0.05, msg);
}

static void test_mk1c_stopping_the_pump_disarms(void) {
    /* DESIGN.md 5.1: the board stays armed only while ARM_TOGGLE toggles.
     * Stopping the toggle IS the disarm, and C_HOLD bleeds through
     * R_BLEED_EN in about 9.6 ms. */
    mk1c_start();
    for (int c = 0; c < 100; c++) {
        plant_set_gpio(C_ARM, 1); plant_step(50e-6);
        plant_set_gpio(C_ARM, 0); plant_step(50e-6);
    }
    plant_probe_t p;
    plant_probe(&p);
    TEST_ASSERT_TRUE(p.armed);

    ms(20);                            /* stop pushing: nothing else changes */
    plant_probe(&p);
    TEST_ASSERT_FALSE_MESSAGE(p.armed, "U9 must disarm within 20 ms of the pump stopping");

    ms(30);
    plant_probe(&p);
    TEST_ASSERT_TRUE_MESSAGE(p.bus_v < 0.61,
                             "the bus must fall below the S9 indicator trip point");
}

/* ══════════════════ MK1A: the numbers in its own file ══════════════ */

static uint16_t mk1a_sense_with(match_state_t st, double ohms) {
    plant_init(PLANT_MK1A);
    plant_set_pack_mv(8400);
    plant_match(1)->state = st;
    if (ohms > 0.0)
        plant_match(1)->r_ohm = ohms;
    plant_match(2)->state = MATCH_ABSENT;
    plant_set_gpio(A_LOW, 1);
    ms(MK1A_SETTLE_MS);
    return plant_adc_counts(A_ADC1);
}

static void test_mk1a_sense_levels(void) {
    /* boards/mk1a/pyro_board.c: "a 2 ohm igniter reads 0 counts, a 1k bad
     * joint 41, a 10k leakage path 372, and a genuine open 4095." */
    assert_counts_near(0,    mk1a_sense_with(MATCH_PRESENT, 2.0),     "MK1A 2 ohm igniter");
    assert_counts_near(41,   mk1a_sense_with(MATCH_PRESENT, 1000.0),  "MK1A 1k bad joint");
    assert_counts_near(372,  mk1a_sense_with(MATCH_PRESENT, 10000.0), "MK1A 10k leakage");
    assert_counts_near(4095, mk1a_sense_with(MATCH_ABSENT, 0.0),      "MK1A genuine open");
}

static void test_mk1a_thresholds_bracket_a_degraded_joint(void) {
    /* The claim the sense scheme rests on: a degraded connection lands
     * BETWEEN the thresholds instead of rounding to "good".
     *
     * It holds, but over a narrower band than the file's own example
     * suggests -- see report_mk1a_threshold_band(), which prints where the
     * band actually starts. 100 kohm is inside it. */
    uint16_t leak100k = mk1a_sense_with(MATCH_PRESENT, 100000.0);
    TEST_ASSERT_GREATER_THAN_UINT16(MK1A_CNT_PATH_MAX, leak100k);
    TEST_ASSERT_LESS_THAN_UINT16(MK1A_CNT_OPEN_MIN, leak100k);
}

static void test_mk1a_open_settles_inside_its_window(void) {
    /* "a 10.1 ms time constant, so an open channel needs about 50 ms to
     * read as open." The settle must be enough; that is the assertion. */
    plant_init(PLANT_MK1A);
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = MATCH_ABSENT;
    plant_set_gpio(A_LOW, 1);
    ms(200);
    TEST_ASSERT_LESS_THAN_UINT16(MK1A_CNT_PATH_MAX, plant_adc_counts(A_ADC1));

    plant_match(1)->state = MATCH_SPENT_OPEN;   /* it fired */
    ms(MK1A_SETTLE_MS);
    TEST_ASSERT_GREATER_THAN_UINT16_MESSAGE(
        MK1A_CNT_OPEN_MIN, plant_adc_counts(A_ADC1),
        "a fired channel must read open by the end of SETTLE_MS");
}

static void test_mk1a_sense_draws_nothing_from_the_pack(void) {
    /* The safety property MK1A's sense scheme is built around: both high
     * sides stay off, so no current reaches a bridgewire from VBATT. */
    plant_init(PLANT_MK1A);
    plant_set_pack_mv(8400);
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    plant_set_gpio(A_LOW, 1);
    ms(200);
    double i_ma = fabs(plant_match(1)->last_i_a) * 1000.0;
    char msg[128];
    snprintf(msg, sizeof(msg), "sense current %.4f mA must stay far below the 100 mA no-fire", i_ma);
    TEST_ASSERT_TRUE_MESSAGE(i_ma < 1.0, msg);
    TEST_ASSERT_FALSE(plant_match(1)->fired);
}

/* ══════════════════ MK1B: the same topology, from the netlist ══════ */

static void test_mk1b_has_the_same_sense_topology_as_mk1a(void) {
    /* R26/R19 100k to +3V3, R25/R18 100R, C25/C21 100nF -- so a present
     * igniter pulls the node to nearly zero and an open one charges to the
     * rail, exactly as on MK1A. */
    plant_init(PLANT_MK1B);
    plant_set_pack_mv(8400);
    plant_match(1)->state = MATCH_PRESENT;
    plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = MATCH_ABSENT;
    plant_set_gpio(B_EN1, 0);
    plant_set_gpio(B_EN2, 0);
    plant_set_gpio(B_COMMON_EN, 1);
    ms(200);
    assert_counts_near(0,    plant_adc_counts(0), "MK1B present igniter");
    assert_counts_near(4095, plant_adc_counts(1), "MK1B open channel");
}

static void test_mk1b_flag_is_high_when_healthy(void) {
    plant_init(PLANT_MK1B);
    plant_match(1)->state = MATCH_PRESENT;
    ms(50);
    TEST_ASSERT_TRUE_MESSAGE(plant_get_gpio(B_FLAG1), "AP2192 FLAG is active low; idle is high");
    TEST_ASSERT_TRUE(plant_get_gpio(B_FLAG2));
}

/* ══════════════════ Reports ═══════════════════════════════════════ */

/* How long a just-opened channel takes to read as open, against the settle
 * the firmware actually waits. This is the post-fire verify case on both
 * boards, and the two answers differ. */
static void report_settle_margins(void) {
    struct { const char *name; plant_board_t b; int low_pin; int thresh; int settle_ms; } t[] = {
        {"MK1A", PLANT_MK1A, A_LOW,       MK1A_CNT_OPEN_MIN,   MK1A_SETTLE_MS},
        {"MK1B", PLANT_MK1B, B_COMMON_EN, MK1B_OPEN_THRESHOLD, MK1B_SAMPLE_SLEEP_MS},
    };
    printf("\n  ── time for a just-opened channel to read OPEN ──\n");
    printf("    %-6s %-10s %-10s %-12s %s\n", "board", "threshold", "firmware", "model says", "");
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        plant_init(t[i].b);
        plant_match(1)->state = MATCH_PRESENT;
        plant_match(1)->r_ohm = 1.0;
        plant_match(2)->state = MATCH_ABSENT;
        plant_set_gpio(t[i].low_pin, 1);
        ms(200);
        plant_match(1)->state = MATCH_SPENT_OPEN;

        int crossed = -1;
        uint16_t at_settle = 0;
        for (int k = 1; k <= 200; k++) {
            ms(1);
            uint16_t c = plant_adc_counts(0);
            if (k == t[i].settle_ms)
                at_settle = c;
            if (crossed < 0 && c > t[i].thresh)
                crossed = k;
        }
        printf("    %-6s %-10d %-10s %-12s %s\n", t[i].name, t[i].thresh, "", "", "");
        printf("           waits %d ms, reads %u counts there -> %s; crosses at %d ms\n",
               t[i].settle_ms, at_settle,
               (at_settle > t[i].thresh) ? "OPEN" : "NOT OPEN", crossed);
    }
}

/* MK1A's file says the thresholds "sit in empty space" so a degraded
 * connection lands between them. Print where the band really is, in path
 * resistance, because the file's own 10 kohm example does not. */
static void report_mk1a_threshold_band(void) {
    printf("\n  ── MK1A: what path resistance lands between the thresholds ──\n");
    printf("    CNT_PATH_MAX %d = \"a path exists\"; CNT_OPEN_MIN %d = \"no path\"\n",
           MK1A_CNT_PATH_MAX, MK1A_CNT_OPEN_MIN);
    const double r[] = {2, 1000, 10000, 14000, 50000, 100000, 300000, 1e6};
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        uint16_t c = mk1a_sense_with(MATCH_PRESENT, r[i]);
        const char *verdict = (c < MK1A_CNT_PATH_MAX) ? "good"
                            : (c > MK1A_CNT_OPEN_MIN) ? "open" : "neither (the gap)";
        printf("    %9.0f ohm -> %4u counts -> %s\n", r[i], c, verdict);
    }
}

/* What each board's firmware classification makes of a healthy igniter. */
static void report_present_match_classification(void) {
    printf("\n  ── what a healthy 1 ohm igniter reads, and how it is classified ──\n");

    plant_init(PLANT_MK1A);
    plant_match(1)->state = MATCH_PRESENT; plant_match(1)->r_ohm = 1.0;
    plant_set_gpio(A_LOW, 1); ms(200);
    uint16_t a = plant_adc_counts(A_ADC1);
    printf("    MK1A  %4u counts   CNT_PATH_MAX %d -> good=%s\n",
           a, MK1A_CNT_PATH_MAX, (a < MK1A_CNT_PATH_MAX) ? "true" : "false");

    plant_init(PLANT_MK1B);
    plant_match(1)->state = MATCH_PRESENT; plant_match(1)->r_ohm = 1.0;
    plant_set_gpio(B_COMMON_EN, 1); ms(200);
    uint16_t b = plant_adc_counts(0);
    bool b_open = b > MK1B_OPEN_THRESHOLD, b_short = b < MK1B_SHORT_THRESHOLD;
    printf("    MK1B  %4u counts   open>%d short<%d -> open=%s shorted=%s good=%s\n",
           b, MK1B_OPEN_THRESHOLD, MK1B_SHORT_THRESHOLD,
           b_open ? "true" : "false", b_short ? "true" : "false",
           (!b_open && !b_short) ? "true" : "false");

    plant_init(PLANT_MK1C);
    plant_match(1)->state = MATCH_PRESENT; plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = MATCH_ABSENT;   /* one match, not two */
    ms(10);
    plant_set_gpio(C_BIAS_BUS, 1); ms(MK1C_TRACK_BIAS_MS);
    uint16_t c = plant_adc_counts(C_ADC_A), cbus = plant_adc_counts(C_ADC_BUS);
    bool present = track_channel(c, cbus) == TRACK_PRESENT;
    printf("    MK1C  %4u counts   against the bus's %u -> open=%s good=%s\n",
           c, cbus, present ? "false" : "true", present ? "true" : "false");
}

/* Every fault the model can inject, against what MK1C's own quiescent and
 * tracking reads would show. A row that moves no reading is a latent
 * fault: DESIGN.md 8.4 asks for exactly this list. */
static void report_mk1c_fault_coverage(void) {
    printf("\n  ── MK1C: which injected faults move a reading ──\n");
    printf("    %-34s %6s %6s %6s %6s %6s  %s\n",
           "fault", "bus_q", "trk_b", "bias_a", "bias_b", "vbat", "visible");

    uint16_t base[5];
    for (int f = -1; f < PF_COUNT; f++) {
        if (f >= 0 && !plant_fault_applies(PLANT_MK1C, (plant_fault_t)f))
            continue;
        plant_init(PLANT_MK1C);
        plant_set_pack_mv(8400);
        plant_match(1)->state = MATCH_ABSENT;
        plant_match(2)->state = MATCH_ABSENT;
        if (f >= 0)
            plant_set_fault((plant_fault_t)f, true);
        ms(100);

        /* The same probes the firmware runs: T1 quiescent, T2 bus bias,
         * then T3 on each channel in turn. Both channels, because a
         * B-side fault is invisible to an A-side probe and reporting it
         * as latent on that basis would be a fault of the report. */
        uint16_t v[5];
        v[0] = plant_adc_counts(C_ADC_BUS);                       /* T1     */
        plant_set_gpio(C_BIAS_BUS, 1); ms(MK1C_TRACK_BIAS_MS);
        v[1] = plant_adc_counts(C_ADC_BUS);                       /* T2     */
        plant_set_gpio(C_BIAS_BUS, 0); ms(20);
        plant_set_gpio(C_BIAS_A, 1); ms(MK1C_TRACK_BIAS_MS);
        v[2] = plant_adc_counts(C_ADC_A);                         /* T3, A  */
        plant_set_gpio(C_BIAS_A, 0); ms(20);
        plant_set_gpio(C_BIAS_B, 1); ms(MK1C_TRACK_BIAS_MS);
        v[3] = plant_adc_counts(C_ADC_B);                         /* T3, B  */
        plant_set_gpio(C_BIAS_B, 0);
        v[4] = plant_adc_counts(C_ADC_VBAT);

        if (f < 0) {
            for (int i = 0; i < 5; i++) base[i] = v[i];
            printf("    %-34s %6u %6u %6u %6u %6u  %s\n", "(healthy baseline)",
                   v[0], v[1], v[2], v[3], v[4], "--");
            continue;
        }
        bool moved = false;
        for (int i = 0; i < 5; i++)
            if (abs((int)v[i] - (int)base[i]) > 20) moved = true;
        printf("    %-34s %6u %6u %6u %6u %6u  %s\n", plant_fault_name((plant_fault_t)f),
               v[0], v[1], v[2], v[3], v[4], moved ? "yes" : "NO -- latent");
    }
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_mk1c_quiescent_is_cold);
    RUN_TEST(test_mk1c_vbat_divider);
    RUN_TEST(test_mk1c_bus_bias_no_match);
    RUN_TEST(test_mk1c_channel_bias_match_off);
    RUN_TEST(test_mk1c_bus_fall_as_measured);
    RUN_TEST(test_mk1c_channel_bias_match_fitted);
    RUN_TEST(test_mk1c_shorted_tvs_reads_like_a_match);
    RUN_TEST(test_mk1c_tracking_separates_present_from_open);
    RUN_TEST(test_mk1c_bus_bias_two_matches);
    RUN_TEST(test_mk1c_tracking_needs_the_bus_to_rise);
    RUN_TEST(test_mk1c_presence_is_a_ratio);
    RUN_TEST(test_mk1c_tracking_current_is_far_below_no_fire);
    RUN_TEST(test_mk1c_precharge_follows_the_dvdt_slew);
    RUN_TEST(test_mk1c_stopping_the_pump_disarms);

    RUN_TEST(test_mk1a_sense_levels);
    RUN_TEST(test_mk1a_thresholds_bracket_a_degraded_joint);
    RUN_TEST(test_mk1a_open_settles_inside_its_window);
    RUN_TEST(test_mk1a_sense_draws_nothing_from_the_pack);

    RUN_TEST(test_mk1b_has_the_same_sense_topology_as_mk1a);
    RUN_TEST(test_mk1b_flag_is_high_when_healthy);

    int rc = UNITY_END();

    printf("\n================ reports (not assertions) ================\n");
    report_present_match_classification();
    report_mk1a_threshold_band();
    report_settle_margins();
    report_mk1c_fault_coverage();
    printf("\n");
    return rc;
}
