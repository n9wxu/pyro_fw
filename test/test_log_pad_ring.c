/*
 * The pad's last seconds, as the flight log opens with them [DAT-10, DAT-11].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "standard_air.h"
#include "../src/log_pad_ring.h"
#include "../src/atmosphere.h"

#define PAD_PA 101325
#define STATE 3

static log_pad_ring_t ring;
static air_t standard;

void setUp(void) {
    standard = air_over_pad((float)PAD_PA, 0.0f);
}
void tearDown(void) {}

static void fill(uint32_t from_ms, int n, uint32_t step_ms) {
    for (int i = 0; i < n; i++)
        log_pad_push(&ring, from_ms + (uint32_t)i * step_ms, PAD_PA - i, PAD_PA - i + 3, (int16_t)(215 + i % 3));
}

void test_DAT_11_rows_come_oldest_first_timed_from_t0(void) {
    log_pad_clear(&ring);
    fill(10000, 100, 20);
    TEST_ASSERT_EQUAL_UINT16(100, log_pad_count(&ring));
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 11000, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(-1000, s.time_ms);
    TEST_ASSERT_EQUAL_INT32(PAD_PA, s.pressure_pa);
    TEST_ASSERT_EQUAL_INT32(PAD_PA + 3, s.raw_pa);
    TEST_ASSERT_EQUAL_INT32(0, s.altitude_cm);
    TEST_ASSERT_EQUAL_UINT8(STATE, s.state);
    TEST_ASSERT_TRUE(log_pad_row(&ring, 99, 11000, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(980, s.time_ms);
    TEST_ASSERT_FALSE(log_pad_row(&ring, 100, 11000, &standard, STATE, &s));
}

void test_DAT_11_a_full_ring_keeps_the_newest(void) {
    log_pad_clear(&ring);
    fill(0, LOG_PAD_N + 50, 20);
    TEST_ASSERT_EQUAL_UINT16(LOG_PAD_N, log_pad_count(&ring));
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 0, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(50 * 20, s.time_ms);
    TEST_ASSERT_EQUAL_INT32(PAD_PA - 50, s.pressure_pa);
}

void test_DAT_11_times_are_placed_across_the_16_bit_wrap(void) {
    log_pad_clear(&ring);
    fill(65536u * 3u - 500u, 50, 20);
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 65536u * 3u + 200u, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(-700, s.time_ms);
    TEST_ASSERT_TRUE(log_pad_row(&ring, 49, 65536u * 3u + 200u, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(280, s.time_ms);
}

void test_DAT_11_a_gap_in_the_pads_samples_empties_the_ring(void) {
    log_pad_clear(&ring);
    fill(0, 100, 20);
    log_pad_push(&ring, 100000, PAD_PA, PAD_PA, 215);
    TEST_ASSERT_EQUAL_UINT16(1, log_pad_count(&ring));
}

void test_DAT_11_a_row_is_its_height_above_the_launchs_ground(void) {
    log_pad_clear(&ring);
    int32_t up_10_m = (int32_t)(standard_pressure_above_pa((float)PAD_PA, 10.0f) + 0.5f);
    log_pad_push(&ring, 0, up_10_m, up_10_m, 215);
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 0, &standard, STATE, &s));
    TEST_ASSERT_INT32_WITHIN(15, 1000, s.altitude_cm);
}

/* [DD-114] In a warm pad's air the same pressure is higher. */
void test_DAT_11_a_row_is_its_height_in_the_pads_air(void) {
    log_pad_clear(&ring);
    int32_t up_10_m = (int32_t)(standard_pressure_above_pa((float)PAD_PA, 10.0f) + 0.5f);
    log_pad_push(&ring, 0, up_10_m, up_10_m, 215);
    air_t warm = air_over_pad((float)PAD_PA, 288.15f + 30.0f);
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 0, &warm, STATE, &s));
    TEST_ASSERT_INT32_WITHIN(15, 1104, s.altitude_cm);
}

void test_DAT_11_a_raw_reading_far_from_the_filtered_is_held_to_the_field(void) {
    log_pad_clear(&ring);
    log_pad_push(&ring, 0, 90000, 90000 + 40000, 215);
    flog_sample_t s;
    TEST_ASSERT_TRUE(log_pad_row(&ring, 0, 0, &standard, STATE, &s));
    TEST_ASSERT_EQUAL_INT32(90000 + INT16_MAX, s.raw_pa);
}

void test_DAT_10_each_row_keeps_the_temperature_measured_with_it(void) {
    log_pad_clear(&ring);
    fill(0, 6, 20);
    flog_sample_t s;
    for (uint16_t i = 0; i < 6; i++) {
        TEST_ASSERT_TRUE(log_pad_row(&ring, i, 0, &standard, STATE, &s));
        TEST_ASSERT_EQUAL_INT16(215 + i % 3, s.temp_dc);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_DAT_11_rows_come_oldest_first_timed_from_t0);
    RUN_TEST(test_DAT_11_a_full_ring_keeps_the_newest);
    RUN_TEST(test_DAT_11_times_are_placed_across_the_16_bit_wrap);
    RUN_TEST(test_DAT_11_a_gap_in_the_pads_samples_empties_the_ring);
    RUN_TEST(test_DAT_11_a_row_is_its_height_above_the_launchs_ground);
    RUN_TEST(test_DAT_11_a_row_is_its_height_in_the_pads_air);
    RUN_TEST(test_DAT_11_a_raw_reading_far_from_the_filtered_is_held_to_the_field);
    RUN_TEST(test_DAT_10_each_row_keeps_the_temperature_measured_with_it);
    return UNITY_END();
}
