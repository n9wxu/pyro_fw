/*
 * A pyro channel's operations, as the flight path reaches them [FLT-RT-02].
 *
 * The flight path calls a channel through pyro_ch(). What it reads there is
 * the store's own, assembled at boot: a table the caller passed in may lie
 * in flash, which the flight path does not read from launch to landing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/pad_claim.h"
#include "../src/pyro_release.h"
#include <string.h>

static int fires, gets, faults, reports;

static void a_fire(uint8_t channel) {
    (void)channel;
    fires++;
}
static void a_get(uint8_t channel, hal_continuity_t *out) {
    (void)channel;
    out->good = true;
    gets++;
}
static bool a_fault(uint8_t channel) {
    (void)channel;
    faults++;
    return true;
}
static void a_report(uint8_t channel, const char *what) {
    (void)channel;
    (void)what;
    reports++;
}

/* Not const: a test takes it away after the boot, as flash is taken away
 * from the flight path while a log page programs. */
static pyro_ch_ops_t callers;

static uint32_t pads_of(uint8_t channel) {
    return channel == 1 ? PAD(9) | PAD(10) : PAD(11) | PAD(10);
}

static bool inside(const void *p, const void *base, size_t size) {
    return (const char *)p >= (const char *)base && (const char *)p < (const char *)base + size;
}

void setUp(void) {
    fires = gets = faults = reports = 0;
    callers = (pyro_ch_ops_t){a_fire, a_get, a_fault};
    pad_claim_reset();
    pyro_release_init(&callers, a_report);
}
void tearDown(void) {}

void test_FLT_RT_02_a_channel_is_not_reached_through_the_callers_table(void) {
    TEST_ASSERT_EQUAL_INT(2, pyro_release_claim(pads_of));
    for (uint8_t ch = 1; ch <= 2; ch++)
        TEST_ASSERT_FALSE(inside(pyro_ch(ch), &callers, sizeof(callers)));
}

void test_FLT_RT_02_the_operations_stand_without_the_callers_table(void) {
    TEST_ASSERT_EQUAL_INT(2, pyro_release_claim(pads_of));
    memset(&callers, 0, sizeof(callers));
    hal_continuity_t c = {0};
    for (uint8_t ch = 1; ch <= 2; ch++) {
        const pyro_ch_ops_t *o = pyro_ch(ch);
        TEST_ASSERT_NOT_NULL(o->fire);
        TEST_ASSERT_NOT_NULL(o->get);
        TEST_ASSERT_NOT_NULL(o->fault);
        o->fire(ch);
        o->get(ch, &c);
        TEST_ASSERT_TRUE(o->fault(ch));
    }
    TEST_ASSERT_EQUAL_INT(2, fires);
    TEST_ASSERT_EQUAL_INT(2, gets);
    TEST_ASSERT_EQUAL_INT(2, faults);
    TEST_ASSERT_EQUAL_INT(0, reports);
}

void test_FLT_RT_02_they_stand_from_the_init_before_any_claim(void) {
    memset(&callers, 0, sizeof(callers));
    TEST_ASSERT_NOT_NULL(pyro_ch(1)->fire);
    TEST_ASSERT_NOT_NULL(pyro_ch(2)->fire);
    pyro_ch(1)->fire(1);
    pyro_ch(2)->fire(2);
    TEST_ASSERT_EQUAL_INT(2, fires);
}

void test_a_released_channel_is_mocked_and_the_other_is_real(void) {
    TEST_ASSERT_TRUE(pad_claim_take(PAD(9), PAD_LUA)); /* Lua holds channel 1's own pad */
    TEST_ASSERT_EQUAL_INT(1, pyro_release_claim(pads_of));
    TEST_ASSERT_TRUE(pyro_release_is_released(1));
    TEST_ASSERT_FALSE(pyro_release_is_released(2));
    pyro_ch(1)->fire(1);
    TEST_ASSERT_EQUAL_INT(0, fires);
    TEST_ASSERT_EQUAL_INT(1, reports);
    pyro_ch(2)->fire(2);
    TEST_ASSERT_EQUAL_INT(1, fires);
    TEST_ASSERT_FALSE(pyro_release_all());
}

void test_a_channel_out_of_range_is_mocked(void) {
    pyro_ch(0)->fire(0);
    pyro_ch(3)->fire(3);
    TEST_ASSERT_EQUAL_INT(0, fires);
    TEST_ASSERT_EQUAL_INT(2, reports);
    TEST_ASSERT_FALSE(pyro_ch(3)->fault(3));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_RT_02_a_channel_is_not_reached_through_the_callers_table);
    RUN_TEST(test_FLT_RT_02_the_operations_stand_without_the_callers_table);
    RUN_TEST(test_FLT_RT_02_they_stand_from_the_init_before_any_claim);
    RUN_TEST(test_a_released_channel_is_mocked_and_the_other_is_real);
    RUN_TEST(test_a_channel_out_of_range_is_mocked);
    return UNITY_END();
}
