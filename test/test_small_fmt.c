/*
 * The flight path's formatter (small_fmt.c) against the C library's
 * snprintf, on every format the flight path writes, at the edges of each
 * type, and cut short.
 *
 * Verifies [FLT-RT-02].
 */
#include "unity.h"
#include "small_fmt.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static char want[512], got[512];

#define SAME(cap, ...)                                                                                                 \
    do {                                                                                                               \
        memset(want, 0x55, sizeof(want));                                                                              \
        memset(got, 0x55, sizeof(got));                                                                                \
        int w = snprintf(want, (cap), __VA_ARGS__);                                                                    \
        int g = small_snprintf(got, (cap), __VA_ARGS__);                                                               \
        TEST_ASSERT_EQUAL_INT(w, g);                                                                                   \
        TEST_ASSERT_EQUAL_MEMORY(want, got, sizeof(want));                                                             \
    } while (0)

static const long LONGS[] = {0, 1, -1, 9, 10, -10, 12345, -12345, 2147483647L, -2147483647L - 1};
static const unsigned long ULONGS[] = {0, 1, 9, 10, 255, 256, 4095, 65535, 4294967295UL};
static const int INTS[] = {0, 1, -1, 7, -7, 100, -100, INT_MAX, INT_MIN};

void test_FLT_RT_02_the_telemetry_sentences_are_the_librarys(void) {
    for (unsigned i = 0; i < sizeof(LONGS) / sizeof(LONGS[0]); i++) {
        for (unsigned j = 0; j < sizeof(ULONGS) / sizeof(ULONGS[0]); j++) {
            SAME(80, "PYRO_APO,%ld,%lu", LONGS[i], ULONGS[j]);
            SAME(80, "PYRO_FIRE,%u,%ld,%lu", (unsigned)ULONGS[j], LONGS[i], ULONGS[j]);
            SAME(80, "PYRO_LAND,%ld,%lu", LONGS[i], ULONGS[j]);
            SAME(160, "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%d,%d", (unsigned)ULONGS[j], 2u, 3u, LONGS[i],
                 -LONGS[i] / 2, LONGS[i] / 3, 7L, ULONGS[j], (unsigned)(ULONGS[j] & 0xffu), 1u, 0u, INTS[i % 9], -1);
        }
    }
    for (unsigned c = 0; c < 256; c++)
        SAME(80, "$%s*%02X\r\n", "PYRO,1,2,3", c);
}

void test_FLT_RT_02_the_log_and_diagnostic_rows_are_the_librarys(void) {
    for (unsigned i = 0; i < sizeof(LONGS) / sizeof(LONGS[0]); i++) {
        SAME(96, "%s cm=%ld cm/s=%ld explains=%d", "lumped", LONGS[i], -LONGS[i], INTS[i % 9]);
        SAME(96, "%s APOGEE", "constacc");
        SAME(96, "!PYRO %s ch=%u bus=%u vbat=%u\r\n", "FIRE", 2u, (unsigned)ULONGS[i % 9], 4095u);
        SAME(96, "!PYRO F10 ch=%u %s (%u, bus %u)\r\n", 1u, "fired", 0u, (unsigned)ULONGS[i % 9]);
        SAME(160, "!PYRO q[vbat=%u bus=%u a=%u b=%u] trk[bus=%u a=%u b=%u] vbat=%lumV seq=%u flt=0x%02x\r\n", 1u, 2u,
             3u, 4u, 5u, 6u, 7u, ULONGS[i % 9], 3u, (unsigned)(ULONGS[i % 9] & 0xffu));
        SAME(96, "running (%d out, %d in, %d serial, %d px)", INTS[i % 9], 0, 1, -1);
        SAME(160, "stopped: %s [ok=%lu req=%lu ack=%lu hb=%lu loc=%lu failloc=%lu preq=%lu stack=%lu]", "Lua stalled",
             ULONGS[i % 9], 1UL, 2UL, 3UL, 4UL, 5UL, 6UL, ULONGS[(i + 3) % 9]);
        SAME(64, "!FAULT");
        SAME(64, " %s", "p2_open");
        SAME(64, "pyro%u %s: released to Lua", 2u, "fire");
        SAME(64, "!MOCK %s\r\n", "pyro1 fire: bench flight");
    }
}

/* Cut short at every length: the same prefix, the same NUL, the same
 * return. */
void test_FLT_RT_02_a_short_buffer_is_cut_as_the_library_cuts_it(void) {
    for (size_t cap = 0; cap < 48; cap++) {
        SAME(cap, "PYRO_FIRE,%u,%ld,%lu", 2u, -123456L, 4294967295UL);
        SAME(cap, "%s cm=%ld", "lumped", -2147483647L - 1);
        SAME(cap, "x=%02X%%", 10u);
    }
}

void test_FLT_RT_02_the_rest_of_the_conversions(void) {
    SAME(64, "%c%c%c", 'a', 'b', 'c');
    SAME(64, "%i %x %X %lx %08X %5u %-3s|", -42, 0xbeefu, 0xbeefu, 0xdeadbeefUL, 0x1fu, 42u, "a");
    SAME(64, "%s", "");
    SAME(64, "100%%");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_RT_02_the_telemetry_sentences_are_the_librarys);
    RUN_TEST(test_FLT_RT_02_the_log_and_diagnostic_rows_are_the_librarys);
    RUN_TEST(test_FLT_RT_02_a_short_buffer_is_cut_as_the_library_cuts_it);
    RUN_TEST(test_FLT_RT_02_the_rest_of_the_conversions);
    return UNITY_END();
}
