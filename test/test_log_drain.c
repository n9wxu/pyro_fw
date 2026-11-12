/*
 * The flight log's ring against a writer as slow as a board's: a growing
 * file stores a page in three periods, and holds a kilobyte before it
 * stores any.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-07].
 */
#include "unity.h"
#include "../src/log_drain.h"

#define RING 4096u
#define SAMPLE 22u
#define FILE_BLOCK 1024u
#define PAGE_DATA 236u
#define PAGE_MS 30u
#define HOLDOFF_MS 2000u
#define FLUSH_MS 200u
#define OPENING (60u + 110u * SAMPLE + 3u * 71u) /* the header, a full pad ring, the start-up's rows */

void setUp(void) {}
void tearDown(void) {}

static uint32_t head, tail, dropped, now_ms, next_sample_ms, sample_ms, block;

static void pass_ms(uint32_t ms) {
    for (uint32_t end = now_ms + ms; now_ms != end; now_ms++) {
        if (now_ms != next_sample_ms) {
            continue;
        }
        next_sample_ms += sample_ms;
        if (SAMPLE > RING - (head - tail)) {
            dropped += SAMPLE;
        } else {
            head += SAMPLE;
        }
    }
}

static void file_write(uint32_t n) {
    block += n;
    if (block < FILE_BLOCK) {
        return;
    }
    uint32_t pages = block / PAGE_DATA;
    block -= pages * PAGE_DATA;
    pass_ms(pages * PAGE_MS);
}

static void write_out(void) {
    uint32_t h = head;
    while (tail != h) {
        uint32_t n = log_drain_piece(h, tail, RING);
        file_write(n);
        tail += n;
    }
}

/* A flight's first `seconds`, a sample each `period_ms`; the bytes dropped. */
static uint32_t fly(uint32_t period_ms, uint32_t seconds) {
    head = tail = dropped = now_ms = next_sample_ms = block = 0;
    sample_ms = period_ms;
    while (now_ms < HOLDOFF_MS && !log_drain_full(head - tail, RING, OPENING)) {
        pass_ms(10);
    }
    for (uint32_t left = OPENING; left;) {
        uint32_t n = left < LOG_DRAIN_PIECE ? left : LOG_DRAIN_PIECE;
        file_write(n);
        left -= n;
    }
    while (now_ms < seconds * 1000u) {
        write_out();
        uint32_t due = now_ms + FLUSH_MS;
        while (now_ms < due && !log_drain_full(head - tail, RING, 0)) {
            pass_ms(10);
        }
    }
    return dropped;
}

void test_FLT_LOG_07_no_sample_is_dropped_while_the_first_write_is_stored(void) {
    TEST_ASSERT_EQUAL_UINT32(0, fly(14, 10)); /* the fastest sensor: the holdoff ends on its timer */
    TEST_ASSERT_EQUAL_UINT32(0, fly(18, 10));
}

void test_FLT_LOG_07_a_ring_that_fills_in_the_holdoff_has_room_for_the_opening(void) {
    TEST_ASSERT_EQUAL_UINT32(0, fly(7, 10));
}

void test_FLT_LOG_07_a_piece_is_one_run_of_the_buffer(void) {
    TEST_ASSERT_EQUAL_UINT32(LOG_DRAIN_PIECE, log_drain_piece(1000, 0, RING));
    TEST_ASSERT_EQUAL_UINT32(40, log_drain_piece(40, 0, RING));
    TEST_ASSERT_EQUAL_UINT32(16, log_drain_piece(RING + 500u, RING - 16u, RING)); /* to the buffer's end */
    TEST_ASSERT_EQUAL_UINT32(0, log_drain_piece(77, 77, RING));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_07_no_sample_is_dropped_while_the_first_write_is_stored);
    RUN_TEST(test_FLT_LOG_07_a_ring_that_fills_in_the_holdoff_has_room_for_the_opening);
    RUN_TEST(test_FLT_LOG_07_a_piece_is_one_run_of_the_buffer);
    return UNITY_END();
}
