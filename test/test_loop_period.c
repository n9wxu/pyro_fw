/*
 * The periods the flight task's alarm skips when it fires late
 * (loop_period.h), each one a period the flight task missed.
 *
 * Verifies [FLT-RT-05].
 */
#include "unity.h"
#include "loop_period.h"

void setUp(void) {}
void tearDown(void) {}

#define T 1000000ull /* a period's target, as a time since boot */
#define P LOOP_PERIOD_US

void test_FLT_RT_05_an_alarm_on_time_skips_nothing(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, loop_periods_skipped(T, T));
    TEST_ASSERT_EQUAL_UINT32(0u, loop_periods_skipped(T, T + 30u));
}

/* Late, but the next alarm can still be armed ahead of its time. */
void test_FLT_RT_05_a_late_alarm_that_leaves_time_to_arm_skips_nothing(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, loop_periods_skipped(T, T + P - LOOP_ARM_MIN_US));
}

/* Too close to the next period to arm for it: that period is skipped. */
void test_FLT_RT_05_an_alarm_too_late_to_arm_the_next_skips_it(void) {
    TEST_ASSERT_EQUAL_UINT32(1u, loop_periods_skipped(T, T + P - LOOP_ARM_MIN_US + 1u));
    TEST_ASSERT_EQUAL_UINT32(1u, loop_periods_skipped(T, T + P));
}

/* A core whose interrupts were off for a 45 ms sector erase. */
void test_FLT_RT_05_an_alarm_held_off_by_an_erase_counts_every_period_it_passed(void) {
    TEST_ASSERT_EQUAL_UINT32(4u, loop_periods_skipped(T, T + 45000u));
    TEST_ASSERT_EQUAL_UINT32(5u, loop_periods_skipped(T, T + 5u * P));
}

/* Whatever the lateness, the alarm is armed far enough ahead: on the grid
 * when nothing was skipped, a period from now when something was. */
void test_FLT_RT_05_the_next_alarm_is_always_far_enough_ahead(void) {
    for (uint64_t late = 0; late < 12u * P; late += 137u) {
        uint64_t now = T + late;
        uint64_t next = loop_periods_skipped(T, now) ? now + P : T + P;
        TEST_ASSERT_TRUE(next >= now + LOOP_ARM_MIN_US);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_RT_05_an_alarm_on_time_skips_nothing);
    RUN_TEST(test_FLT_RT_05_a_late_alarm_that_leaves_time_to_arm_skips_nothing);
    RUN_TEST(test_FLT_RT_05_an_alarm_too_late_to_arm_the_next_skips_it);
    RUN_TEST(test_FLT_RT_05_an_alarm_held_off_by_an_erase_counts_every_period_it_passed);
    RUN_TEST(test_FLT_RT_05_the_next_alarm_is_always_far_enough_ahead);
    return UNITY_END();
}
