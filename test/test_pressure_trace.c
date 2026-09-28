/*
 * The pressure trace's ring, as the bench reads it: in order, numbered, and
 * saying when records were lost to the ring wrapping.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/pressure_trace.h"
#include <string.h>

static uint8_t buf[8192];

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint32_t first, next;
    int n;
    const uint8_t *recs;
} got_t;

static got_t read_since(uint32_t since, int cap) {
    int len = ptrace_read(since, buf, cap);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(12, len);
    TEST_ASSERT_EQUAL_MEMORY(PTRACE_MAGIC, buf, 4);
    TEST_ASSERT_EQUAL_INT(0, (len - 12) % (int)sizeof(ptrace_rec_t));
    got_t g = {u32(buf + 4), u32(buf + 8), (len - 12) / (int)sizeof(ptrace_rec_t), buf + 12};
    return g;
}

/* Record i's raw code is i, so order and numbering can be checked. */
static void note(uint32_t i) {
    ptrace_note(i * 10000u, i * 10000u + 300u, i, 0, 100000 + (int32_t)i, PTRACE_PRESSURE);
}

void setUp(void) {
    ptrace_reset();
}

void tearDown(void) {
}

void test_PTRACE_01_records_come_back_in_order_and_numbered(void) {
    for (uint32_t i = 0; i < 10; i++)
        note(i);
    got_t g = read_since(0, (int)sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, g.first);
    TEST_ASSERT_EQUAL_UINT32(10, g.next);
    TEST_ASSERT_EQUAL_INT(10, g.n);
    for (int i = 0; i < g.n; i++) {
        const uint8_t *r = g.recs + i * sizeof(ptrace_rec_t);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i * 10000u, u32(r));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i * 10000u + 300u, u32(r + 4));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, u32(r + 8));
        TEST_ASSERT_EQUAL_UINT32(100000u + (uint32_t)i, u32(r + 16));
        TEST_ASSERT_EQUAL_UINT8('P', r[20]);
    }
    g = read_since(7, (int)sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(7, g.first);
    TEST_ASSERT_EQUAL_INT(3, g.n);
    g = read_since(10, (int)sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, g.n);
    TEST_ASSERT_EQUAL_UINT32(10, g.next);
}

void test_PTRACE_02_a_wrapped_ring_says_what_it_lost(void) {
    for (uint32_t i = 0; i < PTRACE_N + 40; i++)
        note(i);
    got_t g = read_since(5, (int)sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(40, g.first);
    TEST_ASSERT_EQUAL_UINT32(PTRACE_N + 40, g.next);
    const uint8_t *last = g.recs + (g.n - 1) * sizeof(ptrace_rec_t);
    TEST_ASSERT_EQUAL_UINT32(g.first + (uint32_t)g.n - 1u, u32(last + 8));
}

void test_PTRACE_03_a_small_buffer_takes_the_oldest_first(void) {
    for (uint32_t i = 0; i < 50; i++)
        note(i);
    int cap = 12 + 7 * (int)sizeof(ptrace_rec_t) + 5;
    got_t g = read_since(0, cap);
    TEST_ASSERT_EQUAL_INT(7, g.n);
    TEST_ASSERT_EQUAL_UINT32(0, g.first);
    TEST_ASSERT_EQUAL_UINT32(6, u32(g.recs + 6 * sizeof(ptrace_rec_t) + 8));
    TEST_ASSERT_EQUAL_INT(0, ptrace_read(0, buf, 11));
}

void test_PTRACE_04_a_record_is_24_bytes(void) {
    TEST_ASSERT_EQUAL_INT(24, (int)sizeof(ptrace_rec_t));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PTRACE_01_records_come_back_in_order_and_numbered);
    RUN_TEST(test_PTRACE_02_a_wrapped_ring_says_what_it_lost);
    RUN_TEST(test_PTRACE_03_a_small_buffer_takes_the_oldest_first);
    RUN_TEST(test_PTRACE_04_a_record_is_24_bytes);
    return UNITY_END();
}
