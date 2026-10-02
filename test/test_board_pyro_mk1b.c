/*
 * MK1B's pyro backend, boards/mk1b/pyro_board.c, run on the host against
 * test/fake_sdk. The loop is the only clock (DD-053): a continuity reading
 * waits out its settle across loop iterations, never inside one.
 *
 * The sense node is modelled as the netlist has it (sim/plant/plant_mk1b.c):
 * 100k to 3V3, 100 nF at the pin, and PYRO_COMMON_EN the shared low-side
 * gate. An igniter pulls the node to ground only while the common is on; a
 * short to ground pulls it down either way (DD-059).
 */
#include "../src/loop_period.h"
#include "unity.h"
#include "board_pins.h"
#include "fake_sdk.h"
#include "pyro.h"
#include <stdio.h>

static bool owns[FAKE_PINS];
bool pin_store_owns(uint8_t pin) {
    return pin < FAKE_PINS && owns[pin];
}

#define COMMON BOARD_PIN_PYRO_COMMON_EN
#define EN1 BOARD_PIN_PYRO1_EN
#define EN2 BOARD_PIN_PYRO2_EN
#define SETTLE_MS 10u /* the sense node's settle, as the sleep it replaces */

#define RECHARGE_MS 50u /* 5 x 100k x 100 nF: the node back at 3V3 */

typedef enum { L_OPEN, L_IGNITER, L_SHORT, L_JOINT_1K } load_t;
static load_t load[2];
static uint32_t common_fell_ms;

static int reads, reads_stimulated, reads_unsettled;

/* Counts at the pin: 0 for a 1 ohm igniter, 41 for a 1k joint, 4095 open. */
static uint16_t node_counts(load_t l, bool common) {
    switch (l) {
    case L_SHORT:
        return 0u;
    case L_IGNITER:
        return common ? 0u : 4095u;
    case L_JOINT_1K:
        return common ? 41u : 4095u;
    default:
        return 4095u;
    }
}

static void on_read(uint8_t channel) {
    reads++;
    bool common = fake_level[COMMON];
    if (common) {
        reads_stimulated++;
        if (fake_now_ms - fake_rose_ms[COMMON] < SETTLE_MS)
            reads_unsettled++;
    } else if (fake_now_ms - common_fell_ms < RECHARGE_MS) {
        reads_unsettled++; /* the node has not charged back up */
    }
    if (channel < 2)
        fake_adc[channel] = node_counts(load[channel], common);
}

static uint32_t common_on_ms;

/* The flight loop: pyro_update() every period, and the once-a-second
 * continuity check's pyro_sample(). */
static void loops(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += LOOP_PERIOD_MS) {
        fake_now_ms += LOOP_PERIOD_MS;
        if (fake_level[COMMON])
            common_on_ms += LOOP_PERIOD_MS;
        bool was = fake_level[COMMON];
        pyro_update(fake_now_ms);
        if (was && !fake_level[COMMON])
            common_fell_ms = fake_now_ms;
        if (fake_now_ms % 1000u == 0u)
            pyro_sample();
        char msg[64];
        snprintf(msg, sizeof(msg), "%s at %lu ms", fake_slept ? fake_slept : "", (unsigned long)fake_now_ms);
        TEST_ASSERT_NULL_MESSAGE(fake_slept, msg);
    }
}

void setUp(void) {
    for (int i = 0; i < FAKE_PINS; i++) {
        fake_level[i] = fake_output[i] = false;
        fake_writes[i] = fake_rose_ms[i] = 0;
        owns[i] = false;
    }
    owns[COMMON] = owns[EN1] = owns[EN2] = true;
    load[0] = load[1] = L_IGNITER;
    fake_on_adc_read = on_read;
    fake_slept = NULL;
    reads = reads_stimulated = reads_unsettled = 0;
    common_on_ms = 0;
    fake_now_ms = 100000u;
    common_fell_ms = 0;
    pyro_init();
}

void tearDown(void) {}

void test_mk1b_continuity_never_sleeps(void) {
    loops(3000u);
    TEST_ASSERT_TRUE(reads > 0);
}

/* A presence read comes a settle after the common came on; a short read, with
 * the common off, once the node has charged back up. */
void test_mk1b_reads_after_the_settle(void) {
    loops(3000u);
    TEST_ASSERT_TRUE(reads_stimulated > 0);
    TEST_ASSERT_TRUE(reads > reads_stimulated);
    TEST_ASSERT_EQUAL(0, reads_unsettled);
}

/* Nothing is driven until the loop runs the cycle, so a board whose channels
 * are both released, which never calls pyro_update(), never raises the
 * common. */
void test_mk1b_common_raised_only_by_the_loop(void) {
    TEST_ASSERT_FALSE(fake_level[COMMON]);
}

/* One reading a second, as before, and the stimulus on only for its settle. */
void test_mk1b_one_reading_a_second(void) {
    uint32_t w = fake_writes[COMMON];
    loops(10000u);
    uint32_t rises = (fake_writes[COMMON] - w) / 2u;
    TEST_ASSERT_UINT32_WITHIN(1u, 10u, rises);
    TEST_ASSERT_TRUE_MESSAGE(common_on_ms <= 10u * 30u, "the stimulus stays on past its reading");
}

/* [PYR-CONT-01] A fresh reading at least once a second, on the loop's grid. */
void test_PYR_CONT_01_mk1b_a_reading_at_least_once_a_second(void) {
    uint32_t last = 0, worst = 0, rose = fake_rose_ms[COMMON];
    for (uint32_t t = 0; t < 10000u; t += LOOP_PERIOD_MS) {
        loops(LOOP_PERIOD_MS);
        if (fake_rose_ms[COMMON] != rose) {
            rose = fake_rose_ms[COMMON];
            if (last && rose - last > worst)
                worst = rose - last;
            last = rose;
        }
    }
    char m[48];
    snprintf(m, sizeof(m), "%u ms between readings", (unsigned)worst);
    TEST_ASSERT_TRUE_MESSAGE(worst > 0 && worst <= 1000u, m);
}

void test_mk1b_not_good_before_a_reading(void) {
    pyro_continuity_t c;
    pyro_get(1, &c);
    TEST_ASSERT_FALSE(c.good);
    TEST_ASSERT_TRUE(c.open);
}

static void read_both(pyro_continuity_t *c1, pyro_continuity_t *c2) {
    loops(2100u);
    pyro_get(1, c1);
    pyro_get(2, c2);
}

/* A fitted igniter pulls the node to 0 counts with the common on. That is a
 * present igniter, as on MK1A, and not a short. */
void test_mk1b_igniter_reads_good(void) {
    pyro_continuity_t c1, c2;
    read_both(&c1, &c2);
    TEST_ASSERT_TRUE_MESSAGE(c1.good, "a fitted igniter reads good");
    TEST_ASSERT_FALSE(c1.shorted);
    TEST_ASSERT_FALSE(c1.open);
    TEST_ASSERT_TRUE(c2.good);
    TEST_ASSERT_EQUAL_UINT16(0u, c1.raw_adc);
}

void test_mk1b_empty_connector_reads_open(void) {
    load[0] = L_OPEN;
    pyro_continuity_t c1, c2;
    read_both(&c1, &c2);
    TEST_ASSERT_TRUE(c1.open);
    TEST_ASSERT_FALSE(c1.good);
    TEST_ASSERT_FALSE(c1.shorted);
    TEST_ASSERT_TRUE_MESSAGE(c2.good, "the other channel is its own");
}

/* A path to ground with the common off bypasses the low side: a harness or
 * connector shorted to ground. */
void test_mk1b_short_to_ground_reads_shorted(void) {
    load[1] = L_SHORT;
    pyro_continuity_t c1, c2;
    read_both(&c1, &c2);
    TEST_ASSERT_TRUE(c2.shorted);
    TEST_ASSERT_FALSE(c2.good);
    TEST_ASSERT_TRUE(c1.good);
}

/* A degraded joint still conducts; the raw count shows how well. */
void test_mk1b_bad_joint_reads_good_with_its_count(void) {
    load[0] = L_JOINT_1K;
    pyro_continuity_t c1, c2;
    read_both(&c1, &c2);
    TEST_ASSERT_TRUE(c1.good);
    TEST_ASSERT_EQUAL_UINT16(41u, c1.raw_adc);
}

/* The pulse runs its 500 ms; then a fresh reading, within two loops. */
void test_mk1b_fire_then_a_fresh_reading(void) {
    loops(1500u);
    pyro_fire(1);
    TEST_ASSERT_TRUE(pyro_is_firing());
    TEST_ASSERT_TRUE(fake_level[EN1] && fake_level[COMMON]);
    loops(500u - LOOP_PERIOD_MS);
    TEST_ASSERT_TRUE(pyro_is_firing());
    TEST_ASSERT_TRUE(fake_level[EN1] && fake_level[COMMON]);
    load[0] = L_OPEN; /* the bridgewire is gone */
    loops(LOOP_PERIOD_MS);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[EN1]);
    loops(2u * LOOP_PERIOD_MS);
    pyro_continuity_t c;
    pyro_get(1, &c);
    TEST_ASSERT_TRUE_MESSAGE(c.open, "no fresh reading within 50 ms of the pulse");
    TEST_ASSERT_FALSE_MESSAGE(c.shorted, "the common, on through the pulse, is no short");
    TEST_ASSERT_EQUAL(0, reads_unsettled);
}

/* [PYR-FIRE-01] The fire stamps its pulse on the live clock; the loop
 * updates with the `now` it read at the top of the period. A millisecond
 * boundary between the two must not end the pulse at once. */
void test_PYR_FIRE_01_mk1b_pulse_lasts_when_the_fire_clock_leads_the_loop(void) {
    loops(1500u);
    uint32_t loop_now = fake_now_ms;
    fake_now_ms = loop_now + 1u;
    pyro_fire(1);
    fake_now_ms = loop_now;
    pyro_update(loop_now);
    TEST_ASSERT_TRUE_MESSAGE(pyro_is_firing(), "the pulse ended in the loop that started it");
    TEST_ASSERT_TRUE(fake_level[EN1] && fake_level[COMMON]);
    loops(480u);
    TEST_ASSERT_TRUE_MESSAGE(fake_level[EN1], "the pulse ended before its 500 ms");
    loops(40u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[EN1]);
}

/* [PYR-DEPLOY-02] Only one channel live at a time, whatever the caller does:
 * a fire mid-pulse is refused, and the running pulse keeps its length. */
void test_PYR_DEPLOY_02_mk1b_refuses_a_fire_mid_pulse(void) {
    loops(1500u);
    pyro_fire(1);
    loops(200u);
    pyro_fire(2);
    TEST_ASSERT_FALSE_MESSAGE(fake_level[EN2], "the second channel was energised over the first");
    TEST_ASSERT_TRUE_MESSAGE(fake_level[EN1], "the first channel's pulse was cut short");
    loops(280u);
    TEST_ASSERT_TRUE(pyro_is_firing());
    loops(40u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[EN1] || fake_level[EN2]);
}

/* No such channel: nothing is driven, nothing acknowledges. */
void test_PYR_DEPLOY_02_mk1b_refuses_a_channel_out_of_range(void) {
    loops(1500u);
    uint32_t w_common = fake_writes[COMMON];
    pyro_fire(0);
    TEST_ASSERT_FALSE(pyro_is_firing());
    pyro_fire(3);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(w_common, fake_writes[COMMON], "a bad channel drove the common");
    TEST_ASSERT_FALSE(fake_level[EN1] || fake_level[EN2]);
}

/* [PYR-VERIFY-01] What the post-fire verify reads: nothing, until a
 * check has run after the pulse. A reading from before the fire would say
 * the igniter is still there. */
void test_PYR_VERIFY_01_mk1b_fired_channel_unknown_until_a_check_after_the_pulse(void) {
    loops(1500u);
    pyro_continuity_t c;
    pyro_get(1, &c);
    TEST_ASSERT_TRUE(c.good);
    pyro_fire(1);
    for (uint32_t t = 0; pyro_is_firing() && t < 1000u; t += LOOP_PERIOD_MS) {
        pyro_get(1, &c);
        TEST_ASSERT_FALSE_MESSAGE(c.good || c.open || c.shorted, "the pre-fire reading stands in for a verdict");
        loops(LOOP_PERIOD_MS);
    }
    pyro_get(1, &c);
    TEST_ASSERT_FALSE_MESSAGE(c.good || c.open, "a verdict before any check after the pulse");
    pyro_continuity_t c2;
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE_MESSAGE(c2.good, "the channel that did not fire keeps its reading");
    loops(2u * LOOP_PERIOD_MS);
    pyro_get(1, &c);
    TEST_ASSERT_TRUE_MESSAGE(c.good, "a misfire, checked after the pulse, reads present");
}

/* A released channel's enable is Lua's pad: the cycle never writes it. */
void test_mk1b_released_enable_left_alone(void) {
    owns[EN2] = false;
    uint32_t w = fake_writes[EN2];
    loops(3000u);
    TEST_ASSERT_EQUAL_UINT32(w, fake_writes[EN2]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mk1b_continuity_never_sleeps);
    RUN_TEST(test_mk1b_reads_after_the_settle);
    RUN_TEST(test_mk1b_common_raised_only_by_the_loop);
    RUN_TEST(test_mk1b_one_reading_a_second);
    RUN_TEST(test_PYR_CONT_01_mk1b_a_reading_at_least_once_a_second);
    RUN_TEST(test_mk1b_not_good_before_a_reading);
    RUN_TEST(test_mk1b_igniter_reads_good);
    RUN_TEST(test_mk1b_empty_connector_reads_open);
    RUN_TEST(test_mk1b_short_to_ground_reads_shorted);
    RUN_TEST(test_mk1b_bad_joint_reads_good_with_its_count);
    RUN_TEST(test_mk1b_fire_then_a_fresh_reading);
    RUN_TEST(test_mk1b_released_enable_left_alone);
    RUN_TEST(test_PYR_FIRE_01_mk1b_pulse_lasts_when_the_fire_clock_leads_the_loop);
    RUN_TEST(test_PYR_DEPLOY_02_mk1b_refuses_a_fire_mid_pulse);
    RUN_TEST(test_PYR_DEPLOY_02_mk1b_refuses_a_channel_out_of_range);
    RUN_TEST(test_PYR_VERIFY_01_mk1b_fired_channel_unknown_until_a_check_after_the_pulse);
    return UNITY_END();
}
