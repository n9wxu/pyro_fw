/*
 * MK1B's pyro backend, boards/mk1b/pyro_board.c, run on the host against
 * test/fake_sdk. The loop is the only clock (DD-053): a continuity reading
 * waits out its settle across loop iterations, never inside one.
 */
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

static int reads, reads_unstimulated, reads_unsettled;

static void on_read(uint8_t channel) {
    (void)channel;
    reads++;
    if (!fake_level[COMMON])
        reads_unstimulated++;
    else if (fake_now_ms - fake_rose_ms[COMMON] < SETTLE_MS)
        reads_unsettled++;
}

static uint32_t common_on_ms;

/* The flight loop: pyro_update() every 10 ms, and the once-a-second
 * continuity check's pyro_sample(). */
static void loops(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10u) {
        fake_now_ms += 10u;
        if (fake_level[COMMON])
            common_on_ms += 10u;
        pyro_update(fake_now_ms);
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
    fake_adc[0] = fake_adc[1] = 2000u;
    fake_on_adc_read = on_read;
    fake_slept = NULL;
    reads = reads_unstimulated = reads_unsettled = 0;
    common_on_ms = 0;
    fake_now_ms = 100000u;
    pyro_init();
}

void tearDown(void) {}

void test_mk1b_continuity_never_sleeps(void) {
    loops(3000u);
    TEST_ASSERT_TRUE(reads > 0);
}

/* Each read comes with the stimulus on, and a settle after it came on. */
void test_mk1b_reads_after_the_settle(void) {
    loops(3000u);
    TEST_ASSERT_TRUE(reads > 0);
    TEST_ASSERT_EQUAL(0, reads_unstimulated);
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

void test_mk1b_not_good_before_a_reading(void) {
    pyro_continuity_t c;
    pyro_get(1, &c);
    TEST_ASSERT_FALSE(c.good);
    TEST_ASSERT_TRUE(c.open);
}

void test_mk1b_classifies(void) {
    fake_adc[0] = 4000u; /* open */
    fake_adc[1] = 20u;   /* short */
    loops(1100u);
    pyro_continuity_t c1, c2;
    pyro_get(1, &c1);
    pyro_get(2, &c2);
    TEST_ASSERT_TRUE(c1.open);
    TEST_ASSERT_FALSE(c1.good);
    TEST_ASSERT_TRUE(c2.shorted);
    TEST_ASSERT_FALSE(c2.good);
    fake_adc[0] = fake_adc[1] = 2000u;
    loops(1100u);
    pyro_get(1, &c1);
    TEST_ASSERT_TRUE(c1.good);
    TEST_ASSERT_EQUAL_UINT16(2000u, c1.raw_adc);
}

/* The pulse runs its 500 ms; then a fresh reading lands inside the post-fire
 * verify window, which opens as the pulse ends. */
void test_mk1b_fire_then_a_fresh_reading(void) {
    loops(1500u);
    pyro_fire(1);
    TEST_ASSERT_TRUE(pyro_is_firing());
    TEST_ASSERT_TRUE(fake_level[EN1] && fake_level[COMMON]);
    loops(490u);
    TEST_ASSERT_TRUE(pyro_is_firing());
    TEST_ASSERT_TRUE(fake_level[EN1] && fake_level[COMMON]);
    fake_adc[0] = 4000u; /* the bridgewire is gone */
    loops(20u);
    TEST_ASSERT_FALSE(pyro_is_firing());
    TEST_ASSERT_FALSE(fake_level[EN1]);
    loops(30u);
    pyro_continuity_t c;
    pyro_get(1, &c);
    TEST_ASSERT_TRUE_MESSAGE(c.open, "no fresh reading within 50 ms of the pulse");
    TEST_ASSERT_EQUAL(0, reads_unsettled);
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
    RUN_TEST(test_mk1b_not_good_before_a_reading);
    RUN_TEST(test_mk1b_classifies);
    RUN_TEST(test_mk1b_fire_then_a_fresh_reading);
    RUN_TEST(test_mk1b_released_enable_left_alone);
    return UNITY_END();
}
