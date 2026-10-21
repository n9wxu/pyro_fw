/*
 * The flight log held for the host (log_live.c): bytes read by offset, the
 * oldest overwritten once more than the ring holds is written.
 *
 * Verifies [USB-09, WEB-API-16].
 */
#include "unity.h"
#include "log_live.h"
#include <string.h>

static uint8_t src[3 * LOG_LIVE_BYTES];
static uint8_t dst[LOG_LIVE_BYTES + 64];

static uint8_t byte_at(uint32_t offset) {
    return (uint8_t)(offset * 7u + offset / 251u);
}

static void write_from(uint32_t offset, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        src[i] = byte_at(offset + i);
    log_live_write(src, n);
}

static void check(const uint8_t *d, uint32_t first, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != byte_at(first + i))
            TEST_FAIL_MESSAGE("a byte read is not the byte written at its offset");
}

void setUp(void) {
    log_live_reset();
}
void tearDown(void) {}

/* What was written is read whole, from the offset asked for. */
void test_WEB_API_16_what_was_written_is_read_whole(void) {
    write_from(0, 1000);
    uint32_t first, end;
    TEST_ASSERT_EQUAL_UINT32(1000, log_live_read(0, dst, sizeof(dst), &first, &end));
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(1000, end);
    check(dst, 0, 1000);
}

/* A host that has read up to N asks from N and gets only what is new. */
void test_WEB_API_16_a_read_from_n_returns_what_came_after(void) {
    write_from(0, 500);
    write_from(500, 300);
    uint32_t first, end;
    TEST_ASSERT_EQUAL_UINT32(300, log_live_read(500, dst, sizeof(dst), &first, &end));
    TEST_ASSERT_EQUAL_UINT32(500, first);
    TEST_ASSERT_EQUAL_UINT32(800, end);
    check(dst, 500, 300);
    TEST_ASSERT_EQUAL_UINT32(0, log_live_read(800, dst, sizeof(dst), &first, &end));
    TEST_ASSERT_EQUAL_UINT32(800, first);
}

/* A read is bounded by the room it is given; the next picks up after it. */
void test_WEB_API_16_a_bounded_read_continues_where_it_stopped(void) {
    write_from(0, 3000);
    uint32_t first, end, got = 0;
    while (got < 3000) {
        uint32_t n = log_live_read(got, dst, 700, &first, &end);
        TEST_ASSERT_EQUAL_UINT32(got, first);
        TEST_ASSERT_TRUE(n > 0 && n <= 700);
        check(dst, first, n);
        got += n;
    }
    TEST_ASSERT_EQUAL_UINT32(3000, end);
}

/* More than the ring holds: the oldest is gone, and a read from before it
 * starts at the oldest byte held, so the host can count what it lost. */
void test_USB_09_the_oldest_bytes_go_when_the_ring_is_full(void) {
    uint32_t total = 2u * LOG_LIVE_BYTES + 123u;
    for (uint32_t at = 0; at < total; at += 1000u)
        write_from(at, total - at < 1000u ? total - at : 1000u);
    uint32_t first, end;
    uint32_t n = log_live_read(0, dst, sizeof(dst), &first, &end);
    TEST_ASSERT_EQUAL_UINT32(total, end);
    TEST_ASSERT_EQUAL_UINT32(total - LOG_LIVE_BYTES, first);
    TEST_ASSERT_EQUAL_UINT32(LOG_LIVE_BYTES, n);
    check(dst, first, n);
}

/* One write longer than the ring keeps its last LOG_LIVE_BYTES. */
void test_USB_09_a_write_longer_than_the_ring_keeps_its_end(void) {
    write_from(0, 2u * LOG_LIVE_BYTES);
    uint32_t first, end;
    uint32_t n = log_live_read(0, dst, sizeof(dst), &first, &end);
    TEST_ASSERT_EQUAL_UINT32(2u * LOG_LIVE_BYTES, end);
    TEST_ASSERT_EQUAL_UINT32(LOG_LIVE_BYTES, first);
    check(dst, first, n);
}

/* A new flight's log starts at 0; a host still asking from the last one's
 * offset is given the new log from its oldest byte. */
void test_USB_09_a_new_log_starts_at_zero(void) {
    write_from(0, 5000);
    log_live_reset();
    write_from(0, 200);
    uint32_t first, end;
    TEST_ASSERT_EQUAL_UINT32(200, log_live_read(5000, dst, sizeof(dst), &first, &end));
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(200, end);
    check(dst, 0, 200);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WEB_API_16_what_was_written_is_read_whole);
    RUN_TEST(test_WEB_API_16_a_read_from_n_returns_what_came_after);
    RUN_TEST(test_WEB_API_16_a_bounded_read_continues_where_it_stopped);
    RUN_TEST(test_USB_09_the_oldest_bytes_go_when_the_ring_is_full);
    RUN_TEST(test_USB_09_a_write_longer_than_the_ring_keeps_its_end);
    RUN_TEST(test_USB_09_a_new_log_starts_at_zero);
    return UNITY_END();
}
