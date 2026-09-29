/*
 * A MAC drawn from the RNG, and the /serial.txt that keeps it (mac_random.c,
 * DD-072, WEB-NET-06). Two MK1Cs whose flash chips report one id took one
 * derived MAC; a drawn one depends on what the pool was fed, not on the flash.
 */
#include "unity.h"
#include "mac_random.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void draw_from(uint64_t seed, const uint32_t *samples, int n, uint8_t out[MAC_BYTES]) {
    mac_pool_t p;
    mac_pool_init(&p, seed);
    for (int i = 0; i < n; i++)
        mac_pool_add(&p, samples[i]);
    mac_pool_draw(&p, out);
}

static void draw_counter(uint64_t seed, uint32_t first, uint8_t out[MAC_BYTES]) {
    uint32_t s[64];
    for (int i = 0; i < 64; i++)
        s[i] = first + (uint32_t)i;
    draw_from(seed, s, 64, out);
}

void test_WEB_NET_06_drawn_mac_is_local_unicast(void) {
    for (uint32_t k = 0; k < 2000; k++) {
        uint8_t m[MAC_BYTES];
        draw_counter(k, k * 7u, m);
        TEST_ASSERT_EQUAL_HEX8(0x02, m[0]);
    }
}

void test_WEB_NET_06_subnet_is_never_0_1_or_255(void) {
    for (uint32_t k = 0; k < 20000; k++) {
        uint8_t m[MAC_BYTES];
        draw_counter(0, k, m);
        TEST_ASSERT_NOT_EQUAL(0, m[5]);
        TEST_ASSERT_NOT_EQUAL(1, m[5]);
        TEST_ASSERT_NOT_EQUAL(255, m[5]);
    }
}

/* The MK1C case: one flash id, so one seed. The samples decide. */
void test_WEB_NET_06_same_seed_different_samples_differ(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    uint32_t s[256];
    for (int i = 0; i < 256; i++)
        s[i] = 0x800u + (uint32_t)(i & 3);
    draw_from(0x41503459373331FFull, s, 256, a);
    s[200] ^= 1u; /* one noisy bit */
    draw_from(0x41503459373331FFull, s, 256, b);
    TEST_ASSERT_FALSE(memcmp(a, b, MAC_BYTES) == 0);
}

/* 10,000 boards: 40 random bits make a repeat about 5e-5 likely, so any
 * repeat here is a fault in the mixing. */
void test_WEB_NET_06_no_repeat_across_many_boards(void) {
    enum { N = 10000 };
    static uint64_t seen[N];
    for (int k = 0; k < N; k++) {
        uint8_t m[MAC_BYTES];
        draw_counter(0x41503459373331FFull, (uint32_t)k << 12, m);
        uint64_t v = 0;
        for (int i = 1; i < MAC_BYTES; i++)
            v = (v << 8) | m[i];
        for (int j = 0; j < k; j++)
            TEST_ASSERT_TRUE(seen[j] != v);
        seen[k] = v;
    }
}

/* The subnet byte spread evenly: every one of the 253 allowed values within
 * a generous band of its expectation over 253 * 400 draws. */
void test_WEB_NET_06_subnet_spreads_evenly(void) {
    static uint32_t count[256];
    memset(count, 0, sizeof(count));
    const int per = 400;
    for (int k = 0; k < 253 * per; k++) {
        uint8_t m[MAC_BYTES];
        draw_counter((uint64_t)k * 0x9E37u, (uint32_t)k, m);
        count[m[5]]++;
    }
    for (int v = 2; v < 255; v++) {
        TEST_ASSERT_GREATER_THAN_UINT32(per / 2, count[v]);
        TEST_ASSERT_LESS_THAN_UINT32(per * 3 / 2, count[v]);
    }
}

void test_WEB_NET_06_file_round_trips_drawn_and_assigned(void) {
    const uint8_t mac[MAC_BYTES] = {0x02, 0x37, 0x33, 0x31, 0xFF, 0x2A};
    char buf[MAC_FILE_MAX];
    uint8_t got[MAC_BYTES];
    bool drawn = false;

    TEST_ASSERT_GREATER_THAN(0, mac_file_format(mac, true, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("02373331FF2A\nrng\n", buf);
    TEST_ASSERT_TRUE(mac_file_parse(buf, got, &drawn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, got, MAC_BYTES);
    TEST_ASSERT_TRUE(drawn);

    TEST_ASSERT_GREATER_THAN(0, mac_file_format(mac, false, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(mac_file_parse(buf, got, &drawn));
    TEST_ASSERT_FALSE(drawn);
}

/* What /api/serial writes: twelve digits, nothing else. */
void test_WEB_NET_06_assigned_file_as_the_api_writes_it(void) {
    uint8_t got[MAC_BYTES];
    bool drawn = true;
    TEST_ASSERT_TRUE(mac_file_parse("02373331FF2A", got, &drawn));
    TEST_ASSERT_FALSE(drawn);
    TEST_ASSERT_EQUAL_HEX8(0x2A, got[5]);
    TEST_ASSERT_TRUE(mac_file_parse("02373331ff2a\r\n", got, &drawn));
}

void test_WEB_NET_06_malformed_files_are_refused_whole(void) {
    uint8_t got[MAC_BYTES] = {9, 9, 9, 9, 9, 9};
    const uint8_t untouched[MAC_BYTES] = {9, 9, 9, 9, 9, 9};
    TEST_ASSERT_FALSE(mac_file_parse("02373331FF2", got, NULL));           /* short */
    TEST_ASSERT_FALSE(mac_file_parse("02373331FG2A", got, NULL));          /* not hex */
    TEST_ASSERT_FALSE(mac_file_parse("03373331FF2A", got, NULL));          /* multicast */
    TEST_ASSERT_FALSE(mac_file_parse("02373331FF2A9", got, NULL));         /* long */
    TEST_ASSERT_FALSE(mac_file_parse("02373331FF2A\nmaybe\n", got, NULL)); /* junk */
    TEST_ASSERT_FALSE(mac_file_parse("", got, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, got, MAC_BYTES);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WEB_NET_06_drawn_mac_is_local_unicast);
    RUN_TEST(test_WEB_NET_06_subnet_is_never_0_1_or_255);
    RUN_TEST(test_WEB_NET_06_same_seed_different_samples_differ);
    RUN_TEST(test_WEB_NET_06_no_repeat_across_many_boards);
    RUN_TEST(test_WEB_NET_06_subnet_spreads_evenly);
    RUN_TEST(test_WEB_NET_06_file_round_trips_drawn_and_assigned);
    RUN_TEST(test_WEB_NET_06_assigned_file_as_the_api_writes_it);
    RUN_TEST(test_WEB_NET_06_malformed_files_are_refused_whole);
    return UNITY_END();
}
