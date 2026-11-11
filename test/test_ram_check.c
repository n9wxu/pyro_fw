/*
 * The flight code in RAM against the image's copy [FLT-RT-08].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/ram_check.h"
#include <string.h>

#define LEN 1000u
#define PIECE 64u

static uint8_t image[LEN], ram[LEN];
static ram_check_t check;

void setUp(void) {
    for (uint32_t i = 0; i < LEN; i++)
        image[i] = (uint8_t)(i * 7u + 3u);
    memcpy(ram, image, LEN);
    memset(&check, 0, sizeof(check));
}
void tearDown(void) {}

static bool steps(uint32_t n) {
    bool same = true;
    for (uint32_t i = 0; i < n; i++)
        same = ram_check_step(&check, ram, image, LEN, PIECE);
    return same;
}

#define STEPS_A_SWEEP ((LEN + PIECE - 1u) / PIECE)

void test_FLT_RT_08_code_as_loaded_is_the_same_sweep_after_sweep(void) {
    TEST_ASSERT_TRUE(steps(10u * STEPS_A_SWEEP));
    TEST_ASSERT_EQUAL_UINT32(10u, check.sweeps);
}

void test_FLT_RT_08_one_changed_bit_anywhere_is_found_within_a_sweep(void) {
    for (uint32_t at = 0; at < LEN; at += 37u) {
        setUp();
        TEST_ASSERT_TRUE(steps(5));
        ram[at] ^= 0x10u;
        TEST_ASSERT_FALSE_MESSAGE(steps(STEPS_A_SWEEP), "a sweep passed over it");
    }
    setUp();
    ram[LEN - 1u] ^= 1u;
    TEST_ASSERT_FALSE_MESSAGE(steps(STEPS_A_SWEEP), "the last byte, in the short last piece");
}

void test_FLT_RT_08_a_difference_once_seen_stays(void) {
    ram[500] ^= 1u;
    TEST_ASSERT_FALSE(steps(STEPS_A_SWEEP));
    ram[500] ^= 1u;
    TEST_ASSERT_FALSE(steps(3u * STEPS_A_SWEEP));
}

void test_FLT_RT_08_an_image_with_no_flight_code_in_ram_has_nothing_to_differ(void) {
    TEST_ASSERT_TRUE(ram_check_step(&check, ram, image, 0, PIECE));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_RT_08_code_as_loaded_is_the_same_sweep_after_sweep);
    RUN_TEST(test_FLT_RT_08_one_changed_bit_anywhere_is_found_within_a_sweep);
    RUN_TEST(test_FLT_RT_08_a_difference_once_seen_stays);
    RUN_TEST(test_FLT_RT_08_an_image_with_no_flight_code_in_ram_has_nothing_to_differ);
    return UNITY_END();
}
