/*
 * MK1C's pyro backend, boards/mk1c/pyro_board.c, run on the host against the
 * Pico SDK stand-in in sim/hw/ and the plant model of the board as measured
 * (DD-054). The firmware checks two things and no more (DD-055): that each
 * pyro is present, and that nothing is shorted. Nothing it does blocks.
 */
#include "unity.h"
#include "plant.h"
#include "pyro.h"
#include "rp2040_shim.h"
#include "board_pins.h"
#include <stdio.h>

void hal_telemetry_send(const char *sentence) {
    (void)sentence;
}

#define LOOP_MS 10u

static int bias_bus_rises, bias_ch_rises;
static uint64_t longest_update_us;

/* The flight loop: 10 ms passes, then pyro_update(). Time spent inside the
 * update is time the board code blocked. */
static void loops(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += LOOP_MS) {
        bool bus0 = plant_get_gpio(BOARD_PIN_BIAS_BUS);
        bool a0 = plant_get_gpio(BOARD_PIN_BIAS_A), b0 = plant_get_gpio(BOARD_PIN_BIAS_B);
        shim_advance_us(LOOP_MS * 1000u);
        uint64_t t0 = shim_now_us();
        pyro_update((uint32_t)(t0 / 1000u));
        uint64_t spent = shim_now_us() - t0;
        if (spent > longest_update_us)
            longest_update_us = spent;
        if (!bus0 && plant_get_gpio(BOARD_PIN_BIAS_BUS))
            bias_bus_rises++;
        if ((!a0 && plant_get_gpio(BOARD_PIN_BIAS_A)) || (!b0 && plant_get_gpio(BOARD_PIN_BIAS_B)))
            bias_ch_rises++;
    }
}

static void board(bool match1, bool match2) {
    plant_init(PLANT_MK1C);
    plant_set_pack_mv(8400);
    plant_match(1)->state = match1 ? MATCH_PRESENT : MATCH_ABSENT;
    plant_match(1)->r_ohm = 1.0;
    plant_match(2)->state = match2 ? MATCH_PRESENT : MATCH_ABSENT;
    plant_match(2)->r_ohm = 1.0;
    shim_reset();
    pyro_init();
    bias_bus_rises = bias_ch_rises = 0;
    longest_update_us = 0;
}

void setUp(void) {}
void tearDown(void) {}

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

/* The only stimulus is the tracking test's bus bias, once in each 500 ms:
 * no channel bias, no second bus pulse, and nothing that blocks. */
void test_mk1c_only_the_tracking_test_runs(void) {
    board(true, false);
    loops(5000u);
    char msg[96];
    snprintf(msg, sizeof(msg), "%d bus bias pulses in 5 s", bias_bus_rises);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 10, bias_bus_rises, msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, bias_ch_rises, "no channel bias");
    snprintf(msg, sizeof(msg), "pyro_update() held the loop %llu us", (unsigned long long)longest_update_us);
    TEST_ASSERT_TRUE_MESSAGE(longest_update_us < 200u, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mk1c_match_present_and_absent);
    RUN_TEST(test_mk1c_two_matches);
    RUN_TEST(test_mk1c_bus_short_latches);
    RUN_TEST(test_mk1c_shorted_lowside_behind_a_match_latches);
    RUN_TEST(test_mk1c_high_side_short_latches);
    RUN_TEST(test_mk1c_only_the_tracking_test_runs);
    return UNITY_END();
}
