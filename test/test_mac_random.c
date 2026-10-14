/*
 * The board's MAC: derived from the flash chip's whole unique ID (DD-099),
 * unless /serial.txt holds one an operator assigned (mac_random.c,
 * WEB-NET-06). The pool's mixing is DD-072's; the ID is now all it is fed.
 *
 * Verifies [WEB-NET-06].
 */
#include "unity.h"
#include "mac_random.h"
#include <stdio.h>
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

/* What DD-072's firmware wrote for a drawn MAC: recognised, so it can give
 * way to the flash ID. */
void test_WEB_NET_06_a_drawn_file_is_recognised(void) {
    const uint8_t mac[MAC_BYTES] = {0x02, 0x37, 0x33, 0x31, 0xFF, 0x2A};
    uint8_t got[MAC_BYTES];
    bool drawn = false;
    TEST_ASSERT_TRUE(mac_file_parse("02373331FF2A\nrng\n", got, &drawn));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, got, MAC_BYTES);
    TEST_ASSERT_TRUE(drawn);
}

/* The bench's chips, read whole (DD-098). */
static const uint8_t MK1A_W25Q128JV[8] = {0xE4, 0x62, 0x14, 0xB0, 0x63, 0x2D, 0x47, 0x2F};
static const uint8_t MK1C_XT25F[16] = {0x41, 0x50, 0x34, 0x59, 0x37, 0x33, 0x31, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0x15, 0x00, 0x3D, 0x00, 0x25};
static const uint8_t MK1C_SD_XT25F[16] = {0x41, 0x50, 0x34, 0x59, 0x37, 0x33, 0x31, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0x15, 0x00, 0x64, 0x00, 0x27};
static const uint8_t MK1B_8MB_BY25[16] = {0x35, 0x34, 0x32, 0x39, 0x33, 0x05, 0x94, 0xF3,
                                          0xFF, 0xFC, 0xA5, 0x0E, 0xFF, 0x85, 0x1D, 0x20};
static const uint8_t MK1B_8MB_2_BY25[16] = {0x35, 0x34, 0x32, 0x39, 0x33, 0x06, 0x04, 0xF4,
                                            0xFF, 0xFC, 0x82, 0xD6, 0xFF, 0x36, 0x1D, 0x20};

static void assert_mac(const char *want, const uint8_t *uid, int len) {
    uint8_t m[MAC_BYTES];
    char got[13];
    mac_from_uid(uid, len, m);
    for (int i = 0; i < MAC_BYTES; i++)
        snprintf(got + i * 2, 3, "%02X", m[i]);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

/* Pinned: a change here moves every board's address. */
void test_WEB_NET_06_each_bench_chip_derives_its_own_mac(void) {
    assert_mac("02F2B4B8D80D", MK1A_W25Q128JV, 8);
    assert_mac("02BB65F06D47", MK1C_XT25F, 16);
    assert_mac("02F391188C48", MK1C_SD_XT25F, 16);
    assert_mac("0237CA1FB1A0", MK1B_8MB_BY25, 16);
    assert_mac("0265BA5C7AFC", MK1B_8MB_2_BY25, 16);
}

/* The MK1C case: the SDK's 8 bytes are the same, the whole IDs are not. */
void test_WEB_NET_06_two_ids_that_share_8_bytes_get_two_macs(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(MK1C_XT25F, 16, a);
    mac_from_uid(MK1C_SD_XT25F, 16, b);
    TEST_ASSERT_FALSE(memcmp(a, b, MAC_BYTES) == 0);
    TEST_ASSERT_NOT_EQUAL(a[5], b[5]);
}

/* No storage: a blank flash, or any boot, gives the same address. */
void test_WEB_NET_06_one_id_gives_one_mac_at_every_boot(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(MK1B_8MB_BY25, 16, a);
    mac_from_uid(MK1B_8MB_BY25, 16, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, MAC_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0x02, a[0]);
}

void test_WEB_NET_06_an_id_is_not_its_own_first_half(void) {
    uint8_t a[MAC_BYTES], b[MAC_BYTES];
    mac_from_uid(MK1C_XT25F, 8, a);
    mac_from_uid(MK1C_XT25F, 16, b);
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

/* An operator's MAC is kept: it is how two boards that meet on one subnet
 * are parted. */
void test_WEB_NET_06_an_assigned_file_wins(void) {
    const uint8_t want[MAC_BYTES] = {0x02, 0x37, 0x33, 0x31, 0xFF, 0x2A};
    uint8_t m[MAC_BYTES];
    TEST_ASSERT_TRUE(mac_choose("02373331FF2A", MK1C_SD_XT25F, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
}

void test_WEB_NET_06_a_drawn_file_gives_way_to_the_flash_id(void) {
    uint8_t m[MAC_BYTES], want[MAC_BYTES];
    mac_from_uid(MK1B_8MB_BY25, 16, want);
    TEST_ASSERT_FALSE(mac_choose("0244102D6F98\nrng\n", MK1B_8MB_BY25, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
}

void test_WEB_NET_06_no_file_or_a_bad_one_takes_the_flash_id(void) {
    uint8_t m[MAC_BYTES], want[MAC_BYTES];
    mac_from_uid(MK1C_XT25F, 16, want);
    TEST_ASSERT_FALSE(mac_choose(NULL, MK1C_XT25F, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
    TEST_ASSERT_FALSE(mac_choose("not a mac", MK1C_XT25F, 16, m));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m, MAC_BYTES);
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
    RUN_TEST(test_WEB_NET_06_a_drawn_file_is_recognised);
    RUN_TEST(test_WEB_NET_06_each_bench_chip_derives_its_own_mac);
    RUN_TEST(test_WEB_NET_06_two_ids_that_share_8_bytes_get_two_macs);
    RUN_TEST(test_WEB_NET_06_one_id_gives_one_mac_at_every_boot);
    RUN_TEST(test_WEB_NET_06_an_id_is_not_its_own_first_half);
    RUN_TEST(test_WEB_NET_06_a_derived_subnet_is_never_0_1_or_255);
    RUN_TEST(test_WEB_NET_06_an_assigned_file_wins);
    RUN_TEST(test_WEB_NET_06_a_drawn_file_gives_way_to_the_flash_id);
    RUN_TEST(test_WEB_NET_06_no_file_or_a_bad_one_takes_the_flash_id);
    RUN_TEST(test_WEB_NET_06_assigned_file_as_the_api_writes_it);
    RUN_TEST(test_WEB_NET_06_malformed_files_are_refused_whole);
    return UNITY_END();
}
