/*
 * MK1A's pyro backend, boards/mk1a/pyro_board.c, run on the host against
 * test/fake_sdk. See boards/mk1a/THEORY_OF_OPERATION.md "Continuity check".
 *
 * The sense node as modelled here: an igniter pulls it to ground only while
 * the shared low side is on; a short to ground pulls it down either way.
 */
#include "../src/loop_period.h"
#include "unity.h"
#include "board_pins.h"
#include "fake_sdk.h"
#include "pyro.h"

#define LOW BOARD_PIN_PYRO_LOW
#define HS1 BOARD_PIN_FIRE1
#define HS2 BOARD_PIN_FIRE2

typedef enum { L_OPEN, L_IGNITER, L_SHORT } load_t;
static load_t load[2];

static void on_read(uint8_t channel) {
    if (channel >= 2)
        return;
    bool low_on = fake_level[LOW];
    switch (load[channel]) {
    case L_SHORT:
        fake_adc[channel] = 0u;
        break;
    case L_IGNITER:
        fake_adc[channel] = low_on ? 0u : 4095u;
        break;
    default:
        fake_adc[channel] = 4095u;
    }
}

static void loops(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += LOOP_PERIOD_MS) {
        fake_now_ms += LOOP_PERIOD_MS;
        pyro_update(fake_now_ms);
        TEST_ASSERT_NULL_MESSAGE(fake_slept, fake_slept);
    }
}

void setUp(void) {
    for (int i = 0; i < FAKE_PINS; i++) {
        fake_level[i] = fake_output[i] = false;
        fake_writes[i] = fake_rose_ms[i] = 0;
    }
    load[0] = load[1] = L_IGNITER;
    fake_on_adc_read = on_read;
    fake_slept = NULL;
    fake_now_ms = 100000u;
    pyro_init();
}

void tearDown(void) {}

void test_mk1a_igniters_read_good(void) {
    loops(1000u);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE(c1.good);
    TEST_ASSERT_TRUE(c2.good);
}

/* [PYR-FIRE-01] The fire stamps its pulse on the live clock; the loop
 * updates with the `now` it read at the top of the period. */
void test_PYR_FIRE_01_mk1a_pulse_lasts_when_the_fire_clock_leads_the_loop(void) {
    loops(1000u);
    uint32_t loop_now = fake_now_ms;
    fake_now_ms = loop_now + 1u;
    pyro_fire(1);
    fake_now_ms = loop_now;
    pyro_update(loop_now);
    TEST_ASSERT_TRUE(pyro_is_firing());
    loops(480u);
    TEST_ASSERT_TRUE_MESSAGE(fake_level[HS1] && fake_level[LOW], "the pulse ended before its 500 ms");
    loops(40u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[HS1]);
}

/* [PYR-DEPLOY-02] A fire mid-pulse is refused; the running pulse keeps its
 * length. */
void test_PYR_DEPLOY_02_mk1a_refuses_a_fire_mid_pulse(void) {
    loops(1000u);
    pyro_fire(1);
    loops(200u);
    pyro_fire(2);
    TEST_ASSERT_FALSE_MESSAGE(fake_level[HS2], "the second channel was energised over the first");
    loops(280u);
    TEST_ASSERT_TRUE_MESSAGE(fake_level[HS1], "the first channel's pulse was cut short");
    loops(40u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[HS1] || fake_level[HS2]);
}

void test_PYR_DEPLOY_02_mk1a_refuses_a_channel_out_of_range(void) {
    loops(1000u);
    uint32_t w_low = fake_writes[LOW];
    pyro_fire(0);
    pyro_fire(3);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_EQUAL_UINT32(w_low, fake_writes[LOW]);
}

/* [PYR-VERIFY-01] Nothing is said of a fired channel until a whole check has
 * run after its pulse: the presence half of a check read before the fire
 * would say the igniter is still there. */
void test_PYR_VERIFY_01_mk1a_fired_channel_unknown_until_a_check_after_the_pulse(void) {
    loops(1000u);
    pyro_continuity_t c;
    while (!fake_level[LOW]) /* into a check's presence half */
        loops(LOOP_PERIOD_MS);
    loops(LOOP_PERIOD_MS * 3u); /* presence read; the short half running */
    pyro_fire(1);
    while (pyro_is_firing()) {
        pyro_get(1, &c);
        TEST_ASSERT_FALSE_MESSAGE(c.good || c.open, "the pre-fire reading stands in for a verdict");
        loops(LOOP_PERIOD_MS);
    }
    load[0] = L_OPEN; /* the bridgewire is gone */
    for (int i = 0; i < 3; i++) {
        pyro_get(1, &c);
        TEST_ASSERT_FALSE_MESSAGE(c.good || c.open, "a verdict before a whole check after the pulse");
        loops(LOOP_PERIOD_MS);
    }
    loops(200u);
    pyro_get(1, &c);
    TEST_ASSERT_TRUE_MESSAGE(c.open, "a fired igniter, checked after the pulse, reads open");
    pyro_continuity_t c2;
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE(c2.good);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mk1a_igniters_read_good);
    RUN_TEST(test_PYR_FIRE_01_mk1a_pulse_lasts_when_the_fire_clock_leads_the_loop);
    RUN_TEST(test_PYR_DEPLOY_02_mk1a_refuses_a_fire_mid_pulse);
    RUN_TEST(test_PYR_DEPLOY_02_mk1a_refuses_a_channel_out_of_range);
    RUN_TEST(test_PYR_VERIFY_01_mk1a_fired_channel_unknown_until_a_check_after_the_pulse);
    return UNITY_END();
}
