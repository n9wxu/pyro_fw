/*
 * MK1C's pyro backend, boards/mk1c/pyro_board.c, run on the host against the
 * Pico SDK stand-in in sim/hw/ and the plant model of the board as measured
 * (DD-054). Between fires the firmware checks two things and no more
 * (DD-055): that each pyro is present, and that nothing is shorted. What it
 * finds is reported and withholds no fire (DD-081). A fire arms the bus with
 * the charge pump, closes the gate on the measured bus or at its deadline,
 * and verifies with the next tracking test (DD-056). Nothing it does blocks.
 *
 * Verifies [PYR-FIRE-01, PYR-HEALTH-01, PYR-CONT-01, PYR-CONT-02,
 * PYR-FAULT-01, PYR-FAULT-02, PYR-FAULT-03, PYR-VERIFY-01, PYR-ARM-01,
 * PYR-ARM-03, PYR-ARM-05, PYR-ARM-06, PYR-DEPLOY-02, SYS-FAULT-01,
 * SYS-FAULT-02].
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/loop_period.h"
#include "unity.h"
#include "plant.h"
#include "pyro.h"
#include "flash_op.h"
#include "rp2040_shim.h"
#include "board_pins.h"
#include <stdio.h>
#include <string.h>

static char telemetry[8192];

void hal_telemetry_send(const char *sentence) {
    size_t n = strlen(telemetry), m = strlen(sentence);
    if (n + m < sizeof(telemetry))
        memcpy(telemetry + n, sentence, m + 1);
}

#define LOOP_MS LOOP_PERIOD_MS
#define WATCH_US 25u

static int bias_bus_rises, bias_bus_falls, bias_ch_rises;
static uint64_t longest_update_us;

/* What the plant did between loop iterations, watched every WATCH_US. */
static struct {
    bool both_gates;  /* FIRE_A and FIRE_B high together  */
    bool toggle_seen; /* ARM_TOGGLE ever high             */
    uint64_t toggle_first_us, toggle_last_us;
    uint64_t armed_last_us; /* U9 last seen conducting          */
    bool fire_seen[2];
    uint64_t fire_us[2]; /* FIRE_x rising, as the loop drove it */
    double bus_at_fire[2], vbat_at_fire[2];
    double bus_at_bias_max; /* the bus when BIAS_BUS went high  */
} w;

/* The presence test's bias on the bus, edge by edge: a hardware alarm ends
 * the pulse between loops, so it is watched there as well as at each loop. */
static bool bias_was_on;
static uint64_t bias_rose_us, bias_pulse_min_us, bias_pulse_max_us;

static void note_bias(void) {
    bool on = plant_get_gpio(BOARD_PIN_BIAS_BUS);
    if (on == bias_was_on)
        return;
    bias_was_on = on;
    if (on) {
        plant_probe_t p;
        plant_probe(&p);
        bias_bus_rises++;
        bias_rose_us = shim_now_us();
        if (p.bus_v > w.bus_at_bias_max)
            w.bus_at_bias_max = p.bus_v;
        return;
    }
    bias_bus_falls++;
    uint64_t width = shim_now_us() - bias_rose_us;
    if (bias_pulse_min_us == 0 || width < bias_pulse_min_us)
        bias_pulse_min_us = width;
    if (width > bias_pulse_max_us)
        bias_pulse_max_us = width;
}

static void watch(uint32_t us) {
    for (uint32_t t = 0; t < us; t += WATCH_US) {
        shim_advance_us(WATCH_US);
        note_bias();
        uint64_t now = shim_now_us();
        if (plant_get_gpio(BOARD_PIN_FIRE_A) && plant_get_gpio(BOARD_PIN_FIRE_B))
            w.both_gates = true;
        if (plant_get_gpio(BOARD_PIN_ARM_TOGGLE)) {
            if (!w.toggle_seen)
                w.toggle_first_us = now;
            w.toggle_seen = true;
            w.toggle_last_us = now;
        }
        plant_probe_t p;
        plant_probe(&p);
        if (p.armed)
            w.armed_last_us = now;
    }
}

/* One pyro_update(), as STAGE 4 of the loop runs it, and the edges it drove. */
static void update_now(void) {
    bool a0 = plant_get_gpio(BOARD_PIN_BIAS_A), b0 = plant_get_gpio(BOARD_PIN_BIAS_B);
    const int fire_pin[2] = {BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B};
    bool f0[2] = {plant_get_gpio(fire_pin[0]), plant_get_gpio(fire_pin[1])};

    uint64_t t0 = shim_now_us();
    pyro_update((uint32_t)(t0 / 1000u));
    uint64_t spent = shim_now_us() - t0;
    if (spent > longest_update_us)
        longest_update_us = spent;

    plant_probe_t p;
    plant_probe(&p);
    note_bias();
    if ((!a0 && plant_get_gpio(BOARD_PIN_BIAS_A)) || (!b0 && plant_get_gpio(BOARD_PIN_BIAS_B)))
        bias_ch_rises++;
    for (int i = 0; i < 2; i++) {
        if (!f0[i] && plant_get_gpio(fire_pin[i]) && !w.fire_seen[i]) {
            w.fire_seen[i] = true;
            w.fire_us[i] = shim_now_us();
            w.bus_at_fire[i] = p.bus_v;
            w.vbat_at_fire[i] = p.vbat_v;
        }
    }
}

/* The flight loop: 10 ms passes, then pyro_update(). Time spent inside the
 * update is time the board code blocked. */
static void loops(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += LOOP_MS) {
        watch(LOOP_MS * 1000u);
        update_now();
    }
}

/* The loop stops: time passes and nothing runs. */
static void stall(uint32_t ms) {
    watch(ms * 1000u);
}

/* The flight's fire, STAGE 3: pyro_fire(), the acknowledgement read straight
 * after it, then STAGE 4's pyro_update() in the same iteration. */
static uint64_t accept_us;
static bool accepted(uint8_t ch) {
    accept_us = shim_now_us();
    pyro_fire(ch);
    bool ack = pyro_is_firing();
    update_now();
    return ack;
}

/* Loop until the sequence lets go of the gate, at most max_ms. */
static void until_released(uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms && pyro_is_firing(); t += LOOP_MS)
        loops(LOOP_MS);
}

/* The board as built: no bulk capacitor, 1.1 uF on the bus (DD-054). */
static void board_at(bool match1, bool match2, double pack_mv) {
    plant_init(PLANT_MK1C);
    plant_set_pack_mv(pack_mv);
    plant_match_defaults(plant_match(1));
    plant_match_defaults(plant_match(2));
    plant_match(1)->state = match1 ? MATCH_PRESENT : MATCH_ABSENT;
    plant_match(2)->state = match2 ? MATCH_PRESENT : MATCH_ABSENT;
    shim_reset();
    telemetry[0] = '\0';
    memset(&w, 0, sizeof(w));
    bias_bus_rises = bias_bus_falls = bias_ch_rises = 0;
    bias_was_on = false;
    bias_rose_us = bias_pulse_min_us = bias_pulse_max_us = 0;
    longest_update_us = 0;
    pyro_init();
}

static void board(bool match1, bool match2) {
    board_at(match1, match2, 8400);
}

static bool fired(int ch) {
    plant_probe_t p;
    plant_probe(&p);
    return ch == 1 ? p.fired_a : p.fired_b;
}

void setUp(void) {}
void tearDown(void) {}

/* ── Presence and shorts, between fires (DD-055) ──────────────────── */

void test_mk1c_match_present_and_absent(void) {
    board(true, false);
    loops(1100u);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE_MESSAGE(c1.good, "a fitted match reads present");
    TEST_ASSERT_TRUE_MESSAGE(c2.open, "an absent one reads open");
    TEST_ASSERT_FALSE(pyro_fault(1));
}

void test_mk1c_two_matches(void) {
    board(true, true);
    loops(1100u);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE(c1.good);
    TEST_ASSERT_TRUE(c2.good);
}

/* A bus that will not rise under its bias: a short to ground on the bus, a
 * harness lead, or a shorted low-side FET behind a fitted match. */
void test_mk1c_bus_short_latches(void) {
    board(true, false);
    plant_set_fault(PF_BUS_SHORT_GND, true);
    loops(2100u);
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "a shorted bus latches a fault");
    pyro_continuity_t c1;
    pyro_get(1, &c1);
    TEST_ASSERT_FALSE_MESSAGE(c1.good, "and no channel reads present on it");
}

void test_mk1c_shorted_lowside_behind_a_match_latches(void) {
    board(true, false);
    plant_set_fault(PF_LOWSIDE_A_SHORT, true);
    loops(2100u);
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "a shorted low side with a match fitted shorts the bus");
}

/* The bus at the pack with nothing armed: the high side is shorted. */
void test_mk1c_high_side_short_latches(void) {
    board(false, false);
    plant_set_fault(PF_HIGH_SIDE_SHORT, true);
    loops(200u);
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "a shorted high side latches a fault");
}

/* DESIGN.md invariant 8: N consecutive agreeing samples. One tracking test
 * is one sample, however many loops read it. */
void test_mk1c_one_bad_tracking_reading_does_not_latch(void) {
    board(true, false);
    loops(1100u);
    plant_set_fault(PF_BUS_SHORT_GND, true);
    int falls = bias_bus_falls;
    for (int i = 0; i < 100 && bias_bus_falls == falls; i++)
        loops(LOOP_MS);
    TEST_ASSERT_EQUAL_MESSAGE(falls + 1, bias_bus_falls, "one tracking test ran with the bus shorted");
    plant_set_fault(PF_BUS_SHORT_GND, false);
    loops(2000u);
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "one bad reading is an outlier, not a short");
}

/* The only stimulus is the tracking test's bus bias, once in each 500 ms:
 * no channel bias, no second bus pulse, no pump, and nothing that blocks. */
void test_mk1c_only_the_tracking_test_runs(void) {
    board(true, false);
    loops(5000u);
    char msg[96];
    snprintf(msg, sizeof(msg), "%d bus bias pulses in 5 s", bias_bus_rises);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 10, bias_bus_rises, msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, bias_ch_rises, "no channel bias");
    TEST_ASSERT_FALSE_MESSAGE(w.toggle_seen, "the pump never runs outside a fire");
    snprintf(msg, sizeof(msg), "pyro_update() held the loop %llu us", (unsigned long long)longest_update_us);
    TEST_ASSERT_TRUE_MESSAGE(longest_update_us < 200u, msg);
}

/* The bias pulse is ended by a hardware alarm 8 ms after it starts, not by
 * the next 20 ms loop: DESIGN.md S3 asks for 5 to 10 ms. */
void test_mk1c_presence_pulse_is_8_ms(void) {
    board(true, true);
    loops(5000u);
    char msg[96];
    snprintf(msg, sizeof(msg), "pulses of %llu to %llu us", (unsigned long long)bias_pulse_min_us,
             (unsigned long long)bias_pulse_max_us);
    TEST_ASSERT_TRUE_MESSAGE(bias_bus_falls >= 9, msg);
    TEST_ASSERT_TRUE_MESSAGE(bias_pulse_min_us >= 8000u && bias_pulse_max_us <= 8200u, msg);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE_MESSAGE(c1.good && c2.good, "and the reading taken at its end is the presence reading");
}

/* A fire that comes while the bus is biased drops the test: the alarm that
 * would have ended the pulse finds nothing to read. */
void test_mk1c_a_fire_during_the_presence_pulse_drops_the_test(void) {
    board(true, false);
    loops(1100u);
    int rises = bias_bus_rises;
    for (int i = 0; i < 100 && bias_bus_rises == rises; i++)
        loops(LOOP_MS);
    TEST_ASSERT_TRUE_MESSAGE(plant_get_gpio(BOARD_PIN_BIAS_BUS), "the bus is biased");
    TEST_ASSERT_FALSE_MESSAGE(board_flash_ok(), "no storage write beside the pulse");
    TEST_ASSERT_TRUE(accepted(1));
    TEST_ASSERT_FALSE_MESSAGE(plant_get_gpio(BOARD_PIN_BIAS_BUS), "the bias is dropped at the command");
    loops(100u);
    TEST_ASSERT_TRUE(fired(1));
}

/* ── The fire (DD-056) ────────────────────────────────────────────── */

void test_mk1c_fires_a_present_channel(void) {
    board(true, true);
    loops(1100u);
    TEST_ASSERT_TRUE_MESSAGE(accepted(1), "a present channel is energised");
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(fired(1), "the match took its energy");
    TEST_ASSERT_FALSE_MESSAGE(fired(2), "the other did not");
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(pyro_fault(1));
    TEST_ASSERT_FALSE_MESSAGE(w.both_gates, "never both gates");
    char msg[96];
    snprintf(msg, sizeof(msg), "pyro_update() held the loop %llu us", (unsigned long long)longest_update_us);
    TEST_ASSERT_TRUE_MESSAGE(longest_update_us < 200u, msg);
}

/* DESIGN.md 7.2: fire on the measured bus against the measured pack, never
 * on elapsed time. The slew takes 8.9 ms to 90 % on 2S; the loop sees it on
 * the next iteration, a period after the command [DD-065]. */
void test_mk1c_fires_on_the_measured_bus(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    loops(100u);
    TEST_ASSERT_TRUE(w.fire_seen[0]);
    char msg[96];
    snprintf(msg, sizeof(msg), "bus %.2f V against a pack of %.2f V", w.bus_at_fire[0], w.vbat_at_fire[0]);
    TEST_ASSERT_TRUE_MESSAGE(w.bus_at_fire[0] >= 0.9 * w.vbat_at_fire[0], msg);
    uint64_t after = w.fire_us[0] - accept_us;
    snprintf(msg, sizeof(msg), "fired %llu us after the command", (unsigned long long)after);
    TEST_ASSERT_TRUE_MESSAGE(after >= 8000u && after <= LOOP_PERIOD_US + 1000u, msg);
    TEST_ASSERT_TRUE_MESSAGE(w.toggle_first_us >= accept_us, "the pump starts with the command");
}

/* DESIGN.md 5.1 and 7.1 step 7: the pump runs only inside a fire and stops
 * at it, and U9 lets go within its 9.6 ms after that. */
void test_mk1c_pump_runs_only_inside_a_fire(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_FALSE(w.toggle_seen);
    TEST_ASSERT_TRUE(accepted(1));
    loops(100u);
    TEST_ASSERT_TRUE(w.fire_seen[0]);
    TEST_ASSERT_TRUE_MESSAGE(w.toggle_last_us <= w.fire_us[0], "the pump stops at the fire");
    char msg[96];
    snprintf(msg, sizeof(msg), "U9 on %llu us after the fire", (unsigned long long)(w.armed_last_us - w.fire_us[0]));
    TEST_ASSERT_TRUE_MESSAGE(w.armed_last_us <= w.fire_us[0] + 10000u, msg);
    uint64_t last = w.toggle_last_us;
    loops(2000u);
    TEST_ASSERT_EQUAL_MESSAGE(last, w.toggle_last_us, "and does not run again");
}

/* The passive disarm: a loop that stops feeding the pump disarms the bus by
 * construction -- the FIFO drains, then C_HOLD bleeds. The FIFO carries a
 * loop and a quarter (arm_pump.h), and U9 lets go within 9.6 ms after
 * [DD-065]. */
void test_mk1c_a_stopped_loop_disarms(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    stall(40u);
    char msg[96];
    snprintf(msg, sizeof(msg), "U9 on %llu us after the loop stopped",
             (unsigned long long)(w.armed_last_us - accept_us));
    TEST_ASSERT_TRUE_MESSAGE(w.armed_last_us > accept_us, "the pump armed the bus");
    TEST_ASSERT_TRUE_MESSAGE(w.armed_last_us <= accept_us + LOOP_PERIOD_US * 5u / 4u + 10000u, msg);
    TEST_ASSERT_FALSE(w.fire_seen[0]);
}

/* [PYR-ARM-03, PYR-FAULT-02] A bus that has not reached 90 % by 1.5 times
 * the slew's time: the gate closes at that deadline on whatever the bus has,
 * and the timeout is recorded. The pulse is never abandoned. */
void test_mk1c_a_bus_that_will_not_charge_is_gated_at_the_deadline(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    plant_set_fault(PF_BUS_SHORT_GND, true);
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(w.fire_seen[0], "the gate closed");
    uint64_t after = w.fire_us[0] - accept_us;
    char msg[96];
    snprintf(msg, sizeof(msg), "gated %llu us after the command", (unsigned long long)after);
    TEST_ASSERT_TRUE_MESSAGE(after <= 2u * LOOP_PERIOD_US + 1000u, msg);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "what the board observed is recorded");
    TEST_ASSERT_NOT_NULL(strstr(telemetry, "FIRE bus not charged"));
    TEST_ASSERT_TRUE_MESSAGE(w.toggle_last_us <= w.fire_us[0], "the pump stopped at the gate");
}

/* [PYR-FAULT-01] A pulse that failed prevents nothing. U9 latched off in
 * its current limit; the pump stopping takes its enable low, which is what
 * releases the latch, so the next attempt is delivered and reports for
 * itself. */
void test_mk1c_a_failed_pulse_does_not_prevent_the_next(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    plant_set_fault(PF_BUS_SHORT_GND, true);
    loops(100u);
    TEST_ASSERT_FALSE(fired(1));
    plant_set_fault(PF_BUS_SHORT_GND, false);
    loops(1000u);
    TEST_ASSERT_TRUE_MESSAGE(accepted(1), "the second attempt is taken");
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(fired(1), "and delivered");
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "the first pulse's timeout is not the second's");
}

/* ── No reading withholds a fire [PYR-FIRE-01, PYR-HEALTH-01] ─────── */

static void assert_gated(uint8_t ch) {
    TEST_ASSERT_TRUE_MESSAGE(accepted(ch), "the command is taken");
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(w.toggle_seen, "the pump ran");
    TEST_ASSERT_TRUE_MESSAGE(w.fire_seen[ch - 1], "the gate closed");
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE_MESSAGE(w.both_gates, "never both gates");
}

/* The presence test reads channel 2 open: a lead the test cannot see
 * through may still carry a match. */
void test_mk1c_fires_a_channel_that_reads_open(void) {
    board(true, false);
    loops(1100u);
    pyro_continuity_t c2;
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE(c2.open);
    assert_gated(2);
    TEST_ASSERT_FALSE_MESSAGE(fired(1), "and only that channel");
}

/* A flight resumed after a restart fires before any presence test has run
 * [FLT-BROWN-06]. */
void test_mk1c_fires_before_a_presence_test(void) {
    board(true, false);
    assert_gated(1);
    TEST_ASSERT_TRUE(fired(1));
}

void test_mk1c_fires_with_a_latched_fault(void) {
    board(true, false);
    plant_set_fault(PF_HIGH_SIDE_SHORT, true);
    loops(200u);
    TEST_ASSERT_TRUE(pyro_fault(1));
    plant_set_fault(PF_HIGH_SIDE_SHORT, false);
    loops(1100u);
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "the fault is still reported");
    assert_gated(1);
    TEST_ASSERT_TRUE(fired(1));
}

/* The high side shorted: the bus is already at the pack, so the gate closes
 * on it at once. */
void test_mk1c_fires_on_a_bus_that_is_already_live(void) {
    board(true, false);
    loops(1100u);
    plant_set_fault(PF_HIGH_SIDE_SHORT, true);
    loops(200u);
    TEST_ASSERT_TRUE(pyro_fault(1));
    TEST_ASSERT_TRUE(accepted(1));
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(w.fire_seen[0], "the gate closed");
    TEST_ASSERT_TRUE(fired(1));
}

/* A pack below the level the firing path is rated for: the pulse is still
 * attempted. */
void test_mk1c_fires_on_a_low_pack(void) {
    board_at(true, false, 2800);
    loops(1100u);
    assert_gated(1);
}

/* ── After the fire ───────────────────────────────────────────────── */

/* S6: the bus drains, the tracking test resumes only on a cold bus
 * (invariants 1 and 7), and the fired channel reads open. */
void test_mk1c_fired_channel_reads_open_after(void) {
    board(true, true);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    until_released(100u);
    int rises = bias_bus_rises;
    w.bus_at_bias_max = 0.0;
    for (int i = 0; i < 1500 && bias_bus_rises == rises; i++)
        loops(LOOP_MS);
    char msg[96];
    snprintf(msg, sizeof(msg), "the bus at %.2f V when the bias came back", w.bus_at_bias_max);
    TEST_ASSERT_TRUE_MESSAGE(bias_bus_rises > rises, "the tracking test resumed");
    TEST_ASSERT_TRUE_MESSAGE(w.bus_at_bias_max < 0.2, msg);
    loops(1000u);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_FALSE_MESSAGE(c1.good, "the fired channel reads open");
    TEST_ASSERT_TRUE_MESSAGE(c2.good, "the other still reads present");
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "a drained bus is not a fault");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(telemetry, "ch=1 open: fired"), telemetry);
}

/* Invariant 12: a misfire on one channel never inhibits the other. */
void test_mk1c_misfire_leaves_the_other_channel(void) {
    board(true, true);
    plant_match(1)->fire_energy_j = 1e9;
    plant_match(1)->all_fire_a = 1e9;
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    loops(100u);
    TEST_ASSERT_FALSE(fired(1));
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "a misfire is not a fault");
    TEST_ASSERT_TRUE_MESSAGE(accepted(2), "the other channel is still available");
    loops(100u);
    TEST_ASSERT_TRUE(fired(2));
    loops(1500u);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(telemetry, "ch=1 still present: misfire"), telemetry);
    pyro_continuity_t c1;
    pyro_get(1, &c1);
    TEST_ASSERT_TRUE_MESSAGE(c1.good, "the misfired match still reads present, and is live");
}

/* DESIGN.md 5.3: two events one after the other. The second needs the bus
 * charged, so it does not wait for the drain: it fires within four loops,
 * inside the 100 ms the drain takes [DD-065]. */
void test_mk1c_both_channels_one_after_the_other(void) {
    board(true, true);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    until_released(100u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_TRUE_MESSAGE(accepted(2), "the second fire starts on a live bus");
    loops(100u);
    TEST_ASSERT_TRUE(fired(1));
    TEST_ASSERT_TRUE(fired(2));
    TEST_ASSERT_FALSE_MESSAGE(w.both_gates, "never both gates");
    char msg[96];
    snprintf(msg, sizeof(msg), "second fire %llu us after the first",
             (unsigned long long)(w.fire_us[1] - w.fire_us[0]));
    TEST_ASSERT_TRUE_MESSAGE(w.fire_us[1] - w.fire_us[0] <= 4u * LOOP_PERIOD_US && 4u * LOOP_PERIOD_US < 100000u, msg);
}

/* A U9 that will not turn off keeps the bus at the pack after the fire: the
 * high side is shorted, and it latches once the bleed has had its time. */
void test_mk1c_bus_stuck_live_after_a_fire_latches(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    plant_set_fault(PF_EFUSE_WONT_TURN_OFF, true);
    loops(100u);
    TEST_ASSERT_TRUE(fired(1));
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "a live bus just after a fire is ours");
    loops(200u);
    TEST_ASSERT_TRUE_MESSAGE(pyro_fault(1), "a bus still live once the bleed has had its time is a shorted high side");
}

/* One cell: U9's limit is 3.5 A into the match at 4.2 V. */
void test_mk1c_fires_on_one_cell(void) {
    board_at(true, false, 4200);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    loops(100u);
    TEST_ASSERT_TRUE(fired(1));
    TEST_ASSERT_FALSE(pyro_fault(1));
}

/* An erase stalls the loop for tens of milliseconds, longer than the pump
 * coasts: no flash write while a fire is in its sequence. */
void test_mk1c_flash_waits_out_a_fire(void) {
    board(true, false);
    loops(1100u);
    TEST_ASSERT_TRUE(board_flash_ok());
    TEST_ASSERT_TRUE(accepted(1));
    TEST_ASSERT_FALSE_MESSAGE(board_flash_ok(), "no flash from the command");
    for (int i = 0; i < 10 && pyro_is_firing(); i++) {
        loops(LOOP_MS);
        TEST_ASSERT_EQUAL(!pyro_is_firing(), board_flash_ok());
    }
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_TRUE_MESSAGE(board_flash_ok(), "flash again once the gate is released");
}

/* [PYR-ARM-03] The fire is stamped on the live clock, and the loop updates
 * with the `now` it read at the top of the period: a millisecond boundary
 * between the two is not a precharge that timed out. */
void test_PYR_ARM_03_mk1c_fire_clock_ahead_of_the_loop_is_no_timeout(void) {
    board(true, false);
    loops(1100u);
    uint32_t loop_now = (uint32_t)(shim_now_us() / 1000u);
    shim_advance_us(1000u);
    pyro_fire(1);
    TEST_ASSERT_TRUE(pyro_is_firing());
    pyro_update(loop_now);
    TEST_ASSERT_FALSE_MESSAGE(pyro_fault(1), "a precharge a millisecond old latched a timeout");
    loops(100u);
    TEST_ASSERT_TRUE_MESSAGE(fired(1), "the match took its energy");
    TEST_ASSERT_FALSE(pyro_fault(1));
    TEST_ASSERT_NULL_MESSAGE(strstr(telemetry, "precharge timeout"), telemetry);
}

/* [PYR-VERIFY-01] Between the fire and the next presence test the fired
 * channel has no verdict, and says so: neither present nor open. */
void test_PYR_VERIFY_01_mk1c_fired_channel_unknown_until_the_next_presence_test(void) {
    board(true, true);
    loops(1100u);
    TEST_ASSERT_TRUE(accepted(1));
    pyro_continuity_t c1;
    pyro_get(1, &c1);
    TEST_ASSERT_FALSE_MESSAGE(c1.good || c1.open, "a verdict before the presence test that gives it");
    for (int i = 0; i < 100 && !strstr(telemetry, "F10 ch=1"); i++)
        loops(LOOP_MS);
    pyro_get(1, &c1);
    TEST_ASSERT_TRUE_MESSAGE(c1.open, "the fired channel, tested, reads open");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mk1c_match_present_and_absent);
    RUN_TEST(test_mk1c_two_matches);
    RUN_TEST(test_mk1c_bus_short_latches);
    RUN_TEST(test_mk1c_shorted_lowside_behind_a_match_latches);
    RUN_TEST(test_mk1c_high_side_short_latches);
    RUN_TEST(test_mk1c_one_bad_tracking_reading_does_not_latch);
    RUN_TEST(test_mk1c_only_the_tracking_test_runs);
    RUN_TEST(test_mk1c_presence_pulse_is_8_ms);
    RUN_TEST(test_mk1c_a_fire_during_the_presence_pulse_drops_the_test);
    RUN_TEST(test_mk1c_fires_a_present_channel);
    RUN_TEST(test_mk1c_fires_on_the_measured_bus);
    RUN_TEST(test_mk1c_pump_runs_only_inside_a_fire);
    RUN_TEST(test_mk1c_a_stopped_loop_disarms);
    RUN_TEST(test_mk1c_a_bus_that_will_not_charge_is_gated_at_the_deadline);
    RUN_TEST(test_mk1c_a_failed_pulse_does_not_prevent_the_next);
    RUN_TEST(test_mk1c_fires_a_channel_that_reads_open);
    RUN_TEST(test_mk1c_fires_before_a_presence_test);
    RUN_TEST(test_mk1c_fires_with_a_latched_fault);
    RUN_TEST(test_mk1c_fires_on_a_bus_that_is_already_live);
    RUN_TEST(test_mk1c_fires_on_a_low_pack);
    RUN_TEST(test_mk1c_fired_channel_reads_open_after);
    RUN_TEST(test_mk1c_misfire_leaves_the_other_channel);
    RUN_TEST(test_mk1c_both_channels_one_after_the_other);
    RUN_TEST(test_mk1c_bus_stuck_live_after_a_fire_latches);
    RUN_TEST(test_mk1c_fires_on_one_cell);
    RUN_TEST(test_mk1c_flash_waits_out_a_fire);
    RUN_TEST(test_PYR_ARM_03_mk1c_fire_clock_ahead_of_the_loop_is_no_timeout);
    RUN_TEST(test_PYR_VERIFY_01_mk1c_fired_channel_unknown_until_the_next_presence_test);
    return UNITY_END();
}
