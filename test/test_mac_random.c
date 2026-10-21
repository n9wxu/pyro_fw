/*
 * The board's MAC: derived from the flash chip's whole unique ID (DD-099),
 * unless /serial.txt holds one an operator assigned (mac_random.c,
 * WEB-NET-06). The pool is fed the ID alone.
 *
 * Verifies [WEB-NET-06].
 */
#include "unity.h"
#include "mac_random.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Made up, as every ID here is: the tests hold for any chip on the bench. */
#define SEED 0x0123456789ABCDEFull

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

/* One flash ID, so one seed: the samples decide. */
void test_WEB_NET_06_same_seed_different_samples_differ(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    uint32_t s[256];
    for (int i = 0; i < 256; i++)
        s[i] = 0x800u + (uint32_t)(i & 3);
    draw_from(SEED, s, 256, a);
    s[200] ^= 1u; /* one noisy bit */
    draw_from(SEED, s, 256, b);
    TEST_ASSERT_FALSE(memcmp(a, b, MAC_BYTES) == 0);
}

/* 10,000 boards: 40 random bits make a repeat about 5e-5 likely, so any
 * repeat here is a fault in the mixing. */
void test_WEB_NET_06_no_repeat_across_many_boards(void) {
    enum { N = 10000 };
    static uint64_t seen[N];
    for (int k = 0; k < N; k++) {
        uint8_t m[MAC_BYTES];
        draw_counter(SEED, (uint32_t)k << 12, m);
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

/* A file holding a MAC drawn at random: recognised, so it can give way to
 * the flash ID (DD-099). */
void test_WEB_NET_06_a_drawn_file_is_recognised(void) {
    const uint8_t mac[MAC_BYTES] = {0x02, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    uint8_t got[MAC_BYTES];
    bool drawn = false;
    TEST_ASSERT_TRUE(mac_file_parse("021A2B3C4D5E\nrng\n", got, &drawn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, got, MAC_BYTES);
    TEST_ASSERT_TRUE(drawn);
}

/* An 8-byte ID, Winbond's length, and two 16-byte IDs that share their first
 * 12 bytes, as two XT25F128F chips do (DD-098). */
static const uint8_t ID_8[8] = {0x8E, 0x21, 0x4C, 0x07, 0xD3, 0x95, 0x6A, 0xB0};
static const uint8_t ID_A[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                 0xFF, 0xFF, 0xFF, 0x15, 0x00, 0x11, 0x00, 0x22};
static const uint8_t ID_B[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                 0xFF, 0xFF, 0xFF, 0x15, 0x00, 0x33, 0x00, 0x44};

static void derive_differs_for_every_bit(const uint8_t *id, int len) {
    uint8_t base[MAC_BYTES], flip[16];
    mac_from_uid(id, len, base);
    for (int bit = 0; bit < len * 8; bit++) {
        uint8_t m[MAC_BYTES];
        memcpy(flip, id, (size_t)len);
        flip[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        mac_from_uid(flip, len, m);
        TEST_ASSERT_FALSE(memcmp(base, m, MAC_BYTES) == 0);
    }
}

/* Every bit of the ID reaches the MAC, whichever chip it came from. */
void test_WEB_NET_06_every_bit_of_the_id_moves_the_mac(void) {
    derive_differs_for_every_bit(ID_8, 8);
    derive_differs_for_every_bit(ID_A, 16);
}

/* The SDK's 8 bytes are the same, the whole IDs are not. */
void test_WEB_NET_06_two_ids_that_share_8_bytes_get_two_macs(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(ID_A, 16, a);
    mac_from_uid(ID_B, 16, b);
    TEST_ASSERT_FALSE(memcmp(a, b, MAC_BYTES) == 0);
}

/* No storage: a blank flash, or any boot, gives the same address. */
void test_WEB_NET_06_one_id_gives_one_mac_at_every_boot(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(ID_A, 16, a);
    mac_from_uid(ID_A, 16, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, MAC_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0x02, a[0]);
}

void test_WEB_NET_06_an_id_is_not_its_own_first_half(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(ID_A, 8, a);
    mac_from_uid(ID_A, 16, b);
    TEST_ASSERT_FALSE(memcmp(a, b, MAC_BYTES) == 0);
}

void test_WEB_NET_06_a_derived_subnet_is_never_0_1_or_255(void) {
    uint8_t uid[16] = {0};
    for (uint32_t k = 0; k < 20000; k++) {
        uint8_t m[MAC_BYTES];
        memcpy(uid + 12, &k, sizeof(k));
        mac_from_uid(uid, 16, m);
        TEST_ASSERT_NOT_EQUAL(0, m[5]);
        TEST_ASSERT_NOT_EQUAL(1, m[5]);
        TEST_ASSERT_NOT_EQUAL(255, m[5]);
    }
}

/* IDs that differ only in their last bytes, as a batch of chips does, still
 * spread over all 253 subnets: the collision odds register_board.py quotes
 * assume it. */
void test_WEB_NET_06_derived_subnets_spread_evenly(void) {
    static uint32_t count[256];
    memset(count, 0, sizeof(count));
    const int per = 400;
    uint8_t uid[16];
    memcpy(uid, ID_A, sizeof(uid));
    for (uint32_t k = 0; k < 253u * per; k++) {
        uint8_t m[MAC_BYTES];
        memcpy(uid + 12, &k, sizeof(k));
        mac_from_uid(uid, 16, m);
        count[m[5]]++;
    }
    for (int v = 2; v < 255; v++) {
        TEST_ASSERT_GREATER_THAN_UINT32(per / 2, count[v]);
        TEST_ASSERT_LESS_THAN_UINT32(per * 3 / 2, count[v]);
    }
}

/* An operator's MAC is kept: it is how two boards that meet on one subnet
 * are parted. */
void test_WEB_NET_06_an_assigned_file_wins(void) {
    const uint8_t want[MAC_BYTES] = {0x02, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    uint8_t m[MAC_BYTES];
    TEST_ASSERT_TRUE(mac_choose("021A2B3C4D5E", ID_B, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
}

void test_WEB_NET_06_a_drawn_file_gives_way_to_the_flash_id(void) {
    uint8_t m[MAC_BYTES], want[MAC_BYTES];
    mac_from_uid(ID_A, 16, want);
    TEST_ASSERT_FALSE(mac_choose("021A2B3C4D5E\nrng\n", ID_A, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
}

void test_WEB_NET_06_no_file_or_a_bad_one_takes_the_flash_id(void) {
    uint8_t m[MAC_BYTES], want[MAC_BYTES];
    mac_from_uid(ID_A, 16, want);
    TEST_ASSERT_FALSE(mac_choose(NULL, ID_A, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
    TEST_ASSERT_FALSE(mac_choose("not a mac", ID_A, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
}

/* What /api/serial writes: twelve digits, nothing else. */
void test_WEB_NET_06_assigned_file_as_the_api_writes_it(void) {
    uint8_t got[MAC_BYTES];
    bool drawn = true;
    TEST_ASSERT_TRUE(mac_file_parse("021A2B3C4D5E", got, &drawn));
    TEST_ASSERT_FALSE(drawn);
    TEST_ASSERT_EQUAL_HEX8(0x5E, got[5]);
    TEST_ASSERT_TRUE(mac_file_parse("021a2b3c4d5e\r\n", got, &drawn));
}

void test_WEB_NET_06_malformed_files_are_refused_whole(void) {
    uint8_t got[MAC_BYTES] = {9, 9, 9, 9, 9, 9};
    const uint8_t untouched[MAC_BYTES] = {9, 9, 9, 9, 9, 9};
    TEST_ASSERT_FALSE(mac_file_parse("021A2B3C4D5", got, NULL));           /* short */
    TEST_ASSERT_FALSE(mac_file_parse("021A2B3C4G5E", got, NULL));          /* not hex */
    TEST_ASSERT_FALSE(mac_file_parse("031A2B3C4D5E", got, NULL));          /* multicast */
    TEST_ASSERT_FALSE(mac_file_parse("021A2B3C4D5E9", got, NULL));         /* long */
    TEST_ASSERT_FALSE(mac_file_parse("021A2B3C4D5E\nmaybe\n", got, NULL)); /* junk */
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
    RUN_TEST(test_WEB_NET_06_a_drawn_file_is_recognised);
    RUN_TEST(test_WEB_NET_06_every_bit_of_the_id_moves_the_mac);
    RUN_TEST(test_WEB_NET_06_two_ids_that_share_8_bytes_get_two_macs);
    RUN_TEST(test_WEB_NET_06_one_id_gives_one_mac_at_every_boot);
    RUN_TEST(test_WEB_NET_06_an_id_is_not_its_own_first_half);
    RUN_TEST(test_WEB_NET_06_a_derived_subnet_is_never_0_1_or_255);
    RUN_TEST(test_WEB_NET_06_derived_subnets_spread_evenly);
    RUN_TEST(test_WEB_NET_06_an_assigned_file_wins);
    RUN_TEST(test_WEB_NET_06_a_drawn_file_gives_way_to_the_flash_id);
    RUN_TEST(test_WEB_NET_06_no_file_or_a_bad_one_takes_the_flash_id);
    RUN_TEST(test_WEB_NET_06_assigned_file_as_the_api_writes_it);
    RUN_TEST(test_WEB_NET_06_malformed_files_are_refused_whole);
    return UNITY_END();
}
