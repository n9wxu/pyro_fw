/*
 * Brownout recovery: the decision matrix, on the host.
 *
 * Every case here would otherwise need a rocket and a loose battery
 * connector to reproduce, which is exactly why the decision was written as a
 * pure function.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "brownout.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── The marker ───────────────────────────────────────────────────── */

void test_BRN_MARK_01_round_trip(void) {
    pad_marker_t m;
    pad_marker_fill(&m, 101325, 1200u);
    TEST_ASSERT_TRUE_MESSAGE(pad_marker_valid(&m), "a marker this firmware just wrote must validate");
    TEST_ASSERT_EQUAL_INT32(101325, m.ground_pressure_pa);
    TEST_ASSERT_EQUAL_UINT32(1200u, m.sigma_mpa);
}

/* A half-written or erased sector must not read back as a plausible ground
 * reference -- restoring a wrong one is worse than not recovering at all. */
void test_BRN_MARK_02_corruption_rejected(void) {
    pad_marker_t m;
    pad_marker_fill(&m, 101325, 1200u);

    pad_marker_t bad = m;
    bad.ground_pressure_pa = 90000; /* changed without re-summing */
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&bad), "a tampered pressure must fail the sum");

    bad = m;
    bad.sigma_mpa = 5000; /* [DD-048] the fit's noise, restored to a recovered flight */
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&bad), "a tampered sigma must fail the sum");

    bad = m;
    bad.magic = 0;
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&bad), "an erased sector must not validate");

    bad = m;
    bad.version = PAD_MARKER_VERSION + 1;
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&bad), "a future version must not be trusted");

    memset(&bad, 0xFF, sizeof(bad));
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&bad), "erased flash must not validate");
}

/* A pressure the sensor could never see means the marker did not survive,
 * whatever its sum says about itself. */
void test_BRN_MARK_03_implausible_pressure_rejected(void) {
    pad_marker_t m;
    pad_marker_fill(&m, 5, 1200u);
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&m), "5 Pa is not a ground pressure");
    pad_marker_fill(&m, 200000, 1200u);
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&m), "200 kPa is not a ground pressure");
    pad_marker_fill(&m, 101325, 0u);
    TEST_ASSERT_FALSE_MESSAGE(pad_marker_valid(&m), "a sensor with no noise was never measured");
}

/* ── The verdict ──────────────────────────────────────────────────── */

/* The reboot button and the OTA path both reset through the watchdog. The
 * board is on the pad with somebody at a keyboard: recalibrate. */
void test_BRN_01_software_reset_never_recovers(void) {
    TEST_ASSERT_EQUAL(RECOVER_COLD, brownout_assess(RESET_SOFTWARE, true, 50000, 2000));
}

void test_BRN_02_no_marker_never_recovers(void) {
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_COLD, brownout_assess(RESET_POWER_EVENT, false, 50000, 2000),
                              "without a marker there is no ground reference to recover");
}

/* The ordinary case: battery connected on the pad. */
void test_BRN_03_power_on_at_the_pad_is_cold(void) {
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_COLD, brownout_assess(RESET_POWER_EVENT, true, 0, 0),
                              "a board sitting on the pad must calibrate normally");
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_COLD, brownout_assess(RESET_POWER_EVENT, true, RECOVER_ALT_CM - 1, 0),
                              "just under the altitude threshold is still the pad");
}

/* The case this whole mechanism exists for: a connector bounces at launch. */
void test_BRN_04_climbing_recovers_into_ascent(void) {
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_ASCENT, brownout_assess(RESET_POWER_EVENT, true, 12000, 9000),
                              "120 m up and climbing at 90 m/s is a launch brownout");
}

void test_BRN_05_falling_recovers_into_descent(void) {
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_DESCENT, brownout_assess(RESET_POWER_EVENT, true, 50000, -3000),
                              "500 m up and descending is a flight to rejoin");
}

/* The dangerous one. Weather can move the pressure by more than 30 m of
 * apparent altitude between the marker being written and the board being
 * switched on again. Without the motion test that would read as "airborne"
 * and arm the pyros of a rocket somebody is standing next to. */
void test_BRN_06_high_but_still_does_not_arm(void) {
    recovery_t r = brownout_assess(RESET_POWER_EVENT, true, 60000, 0);
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_AMBIGUOUS, r, "a stationary board must never be treated as in flight");
    TEST_ASSERT_TRUE_MESSAGE(r != RECOVER_ASCENT && r != RECOVER_DESCENT,
                             "an ambiguous verdict must not reach a flight state");
}

/* Drift alone must not trip it either way, in either direction. */
void test_BRN_07_slow_drift_is_ambiguous_not_flight(void) {
    TEST_ASSERT_EQUAL(RECOVER_AMBIGUOUS, brownout_assess(RESET_POWER_EVENT, true, 40000, RECOVER_SPEED_CMS - 1));
    TEST_ASSERT_EQUAL(RECOVER_AMBIGUOUS, brownout_assess(RESET_POWER_EVENT, true, 40000, -(RECOVER_SPEED_CMS - 1)));
    TEST_ASSERT_EQUAL(RECOVER_ASCENT, brownout_assess(RESET_POWER_EVENT, true, 40000, RECOVER_SPEED_CMS));
    TEST_ASSERT_EQUAL(RECOVER_DESCENT, brownout_assess(RESET_POWER_EVENT, true, 40000, -RECOVER_SPEED_CMS));
}

/* The RUN pin and a debugger are not power events. */
void test_BRN_08_other_causes_are_cold(void) {
    TEST_ASSERT_EQUAL(RECOVER_COLD, brownout_assess(RESET_RUN_PIN, true, 50000, -3000));
    TEST_ASSERT_EQUAL(RECOVER_COLD, brownout_assess(RESET_DEBUG, true, 50000, -3000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_BRN_MARK_01_round_trip);
    RUN_TEST(test_BRN_MARK_02_corruption_rejected);
    RUN_TEST(test_BRN_MARK_03_implausible_pressure_rejected);
    RUN_TEST(test_BRN_01_software_reset_never_recovers);
    RUN_TEST(test_BRN_02_no_marker_never_recovers);
    RUN_TEST(test_BRN_03_power_on_at_the_pad_is_cold);
    RUN_TEST(test_BRN_04_climbing_recovers_into_ascent);
    RUN_TEST(test_BRN_05_falling_recovers_into_descent);
    RUN_TEST(test_BRN_06_high_but_still_does_not_arm);
    RUN_TEST(test_BRN_07_slow_drift_is_ambiguous_not_flight);
    RUN_TEST(test_BRN_08_other_causes_are_cold);
    return UNITY_END();
}
