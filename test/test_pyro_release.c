/*
 * Who owns a pyro channel [PYR-HEALTH-02]: the pad claim decides, once.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/pad_claim.h"
#include "../src/pyro_release.h"

static uint32_t pads_of(uint8_t channel) {
    return channel == 1 ? PAD(9) | PAD(10) : PAD(11) | PAD(10);
}

void setUp(void) {
    pad_claim_reset();
    pyro_release_init();
}
void tearDown(void) {}

void test_PYR_HEALTH_02_both_channels_are_the_flights_when_their_pads_are_free(void) {
    TEST_ASSERT_EQUAL_INT(2, pyro_release_claim(pads_of));
    TEST_ASSERT_FALSE(pyro_release_is_released(1));
    TEST_ASSERT_FALSE(pyro_release_is_released(2));
    TEST_ASSERT_FALSE(pyro_release_all());
}

void test_PYR_HEALTH_02_a_channel_whose_pad_the_script_holds_is_released(void) {
    TEST_ASSERT_TRUE(pad_claim_take(PAD(9), PAD_LUA));
    TEST_ASSERT_EQUAL_INT(1, pyro_release_claim(pads_of));
    TEST_ASSERT_TRUE(pyro_release_is_released(1));
    TEST_ASSERT_FALSE(pyro_release_is_released(2));
    TEST_ASSERT_FALSE(pyro_release_all());
}

void test_PYR_HEALTH_02_the_common_held_by_the_script_releases_both(void) {
    TEST_ASSERT_TRUE(pad_claim_take(PAD(10), PAD_LUA));
    TEST_ASSERT_EQUAL_INT(0, pyro_release_claim(pads_of));
    TEST_ASSERT_TRUE(pyro_release_all());
}

void test_PYR_HEALTH_02_a_board_that_names_no_pads_has_no_channel(void) {
    TEST_ASSERT_EQUAL_INT(0, pyro_release_claim(NULL));
    TEST_ASSERT_TRUE(pyro_release_all());
}

void test_PYR_HEALTH_02_a_channel_that_does_not_exist_is_released(void) {
    TEST_ASSERT_EQUAL_INT(2, pyro_release_claim(pads_of));
    TEST_ASSERT_TRUE(pyro_release_is_released(0));
    TEST_ASSERT_TRUE(pyro_release_is_released(3));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PYR_HEALTH_02_both_channels_are_the_flights_when_their_pads_are_free);
    RUN_TEST(test_PYR_HEALTH_02_a_channel_whose_pad_the_script_holds_is_released);
    RUN_TEST(test_PYR_HEALTH_02_the_common_held_by_the_script_releases_both);
    RUN_TEST(test_PYR_HEALTH_02_a_board_that_names_no_pads_has_no_channel);
    RUN_TEST(test_PYR_HEALTH_02_a_channel_that_does_not_exist_is_released);
    return UNITY_END();
}
