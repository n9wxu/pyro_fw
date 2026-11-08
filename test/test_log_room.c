/*
 * The room a flight log has: what was free at its open, less the pages kept
 * for its OUT OF ROOM row, charged a page a sync.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-13].
 */
#include "unity.h"
#include "../src/log_room.h"

#define PAGE 248u

void setUp(void) {}
void tearDown(void) {}

void test_FLT_LOG_13_the_kept_pages_are_never_spent(void) {
    log_room_t r;
    log_room_open(&r, 10u * PAGE, PAGE);
    uint32_t written = 0;
    while (log_room_take(&r, 22)) {
        log_room_wrote(&r, 22);
        written += 22;
    }
    TEST_ASSERT_TRUE(r.out);
    TEST_ASSERT_TRUE(written <= (10u - LOG_ROOM_KEEP) * PAGE);
    TEST_ASSERT_TRUE(written > (10u - LOG_ROOM_KEEP) * PAGE - 22u);
}

void test_FLT_LOG_13_a_sync_costs_the_rest_of_its_page(void) {
    log_room_t r;
    log_room_open(&r, 10u * PAGE, PAGE);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE(log_room_take(&r, 22));
        log_room_wrote(&r, 22);
        log_room_synced(&r);
    }
    TEST_ASSERT_EQUAL_UINT32(8u * PAGE, r.spent);
    TEST_ASSERT_FALSE(log_room_take(&r, 22));
}

void test_FLT_LOG_13_a_sync_on_a_page_boundary_costs_nothing(void) {
    log_room_t r;
    log_room_open(&r, 10u * PAGE, PAGE);
    log_room_take(&r, PAGE);
    log_room_wrote(&r, PAGE);
    log_room_synced(&r);
    TEST_ASSERT_EQUAL_UINT32(PAGE, r.spent);
}

void test_FLT_LOG_13_a_store_with_no_more_than_the_kept_pages_has_no_room(void) {
    log_room_t r;
    log_room_open(&r, LOG_ROOM_KEEP * PAGE, PAGE);
    TEST_ASSERT_FALSE(log_room_take(&r, 1));
    log_room_open(&r, 0, PAGE);
    TEST_ASSERT_FALSE(log_room_take(&r, 1));
}

/* Once out, nothing more is taken, even what would fit. */
void test_FLT_LOG_13_out_of_room_stays_out(void) {
    log_room_t r;
    log_room_open(&r, 10u * PAGE, PAGE);
    TEST_ASSERT_FALSE(log_room_take(&r, 100u * PAGE));
    TEST_ASSERT_FALSE(log_room_take(&r, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_13_the_kept_pages_are_never_spent);
    RUN_TEST(test_FLT_LOG_13_a_sync_costs_the_rest_of_its_page);
    RUN_TEST(test_FLT_LOG_13_a_sync_on_a_page_boundary_costs_nothing);
    RUN_TEST(test_FLT_LOG_13_a_store_with_no_more_than_the_kept_pages_has_no_room);
    RUN_TEST(test_FLT_LOG_13_out_of_room_stays_out);
    return UNITY_END();
}
