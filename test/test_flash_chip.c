/*
 * What a flash chip says of itself: its capacity from the JEDEC ID, the
 * length of its unique ID, and whether the image's flash map fits it.
 *
 * The chips are the bench's: MK1A's W25Q128JV, MK1B's W25Q16JV, MK1B-8MB's
 * BY25Q64ES and MK1C's XT25F128F. A chip that does not declare its size must
 * read as unknown, never as small: refusing on no evidence would stop every
 * board whose part is not in the table.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-BOOT-17, WEB-API-15].
 */
#include "unity.h"
#include "flash_chip.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define MB (1024u * 1024u)

static const uint8_t W25Q16JV[3] = {0xEF, 0x40, 0x15};
static const uint8_t W25Q128JV[3] = {0xEF, 0x40, 0x18};
static const uint8_t BY25Q64ES[3] = {0x68, 0x40, 0x17};
static const uint8_t XT25F128F[3] = {0x0B, 0x40, 0x18};

static void test_FC_01_capacity_from_the_third_byte(void) {
    TEST_ASSERT_EQUAL_UINT32(2 * MB, flash_chip_capacity(W25Q16JV));
    TEST_ASSERT_EQUAL_UINT32(16 * MB, flash_chip_capacity(W25Q128JV));
    TEST_ASSERT_EQUAL_UINT32(8 * MB, flash_chip_capacity(BY25Q64ES));
    TEST_ASSERT_EQUAL_UINT32(16 * MB, flash_chip_capacity(XT25F128F));
}

/* No chip, a bus that floats high, and a part whose third byte is not a
 * size (Adesto's AT25SF128A answers 1F 89 01). */
static void test_FC_02_a_byte_that_is_not_a_size_is_unknown(void) {
    static const uint8_t none[3] = {0x00, 0x00, 0x00};
    static const uint8_t floating[3] = {0xFF, 0xFF, 0xFF};
    static const uint8_t adesto[3] = {0x1F, 0x89, 0x01};
    TEST_ASSERT_EQUAL_UINT32(0, flash_chip_capacity(none));
    TEST_ASSERT_EQUAL_UINT32(0, flash_chip_capacity(floating));
    TEST_ASSERT_EQUAL_UINT32(0, flash_chip_capacity(adesto));
    TEST_ASSERT_EQUAL_UINT32(0, flash_chip_capacity(NULL));
}

/* Winbond's is 64 bits; XTX's and BYTe's are 128, of which the SDK reads
 * the first 64 -- the half two MK1Cs share. */
static void test_FC_03_unique_id_length_by_maker(void) {
    TEST_ASSERT_EQUAL_INT(8, flash_chip_uid_len(W25Q16JV));
    TEST_ASSERT_EQUAL_INT(8, flash_chip_uid_len(W25Q128JV));
    TEST_ASSERT_EQUAL_INT(16, flash_chip_uid_len(BY25Q64ES));
    TEST_ASSERT_EQUAL_INT(16, flash_chip_uid_len(XT25F128F));
}

static void test_FC_04_an_unknown_maker_reads_the_sdks_eight(void) {
    static const uint8_t adesto[3] = {0x1F, 0x89, 0x01};
    TEST_ASSERT_EQUAL_INT(8, flash_chip_uid_len(adesto));
    TEST_ASSERT_EQUAL_INT(8, flash_chip_uid_len(NULL));
}

/* MK1B-8MB's image on MK1B's chip: the larger map erased the smaller board. */
static void test_FC_05_a_map_larger_than_the_chip_does_not_fit(void) {
    TEST_ASSERT_EQUAL(FLASH_FIT_TOO_SMALL, flash_chip_fit(2 * MB, 8 * MB));
    TEST_ASSERT_EQUAL(FLASH_FIT_TOO_SMALL, flash_chip_fit(8 * MB, 16 * MB));
}

/* MK1B's image on MK1B-8MB's chip fits: it uses the first 2 MB. */
static void test_FC_06_a_map_no_larger_than_the_chip_fits(void) {
    TEST_ASSERT_EQUAL(FLASH_FIT_OK, flash_chip_fit(2 * MB, 2 * MB));
    TEST_ASSERT_EQUAL(FLASH_FIT_OK, flash_chip_fit(8 * MB, 2 * MB));
    TEST_ASSERT_EQUAL(FLASH_FIT_OK, flash_chip_fit(16 * MB, 16 * MB));
}

static void test_FC_07_an_unknown_chip_is_not_too_small(void) {
    TEST_ASSERT_EQUAL(FLASH_FIT_UNKNOWN, flash_chip_fit(0, 8 * MB));
}

/* What the boot read leaves for /api/status. */
static void test_FC_08_the_chip_read_at_boot_is_reported(void) {
    static const uint8_t uid[16] = {0x41, 0x50, 0x34, 0x59, 0x37, 0x33, 0x31, 0xFF,
                                    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    char hex[33];

    flash_chip_set(XT25F128F, uid);
    TEST_ASSERT_EQUAL_UINT32(16 * MB, flash_chip_bytes());
    flash_chip_jedec_hex(hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("0B4018", hex);
    flash_chip_uid_hex(hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("41503459373331FF0102030405060708", hex);
}

static void test_FC_09_a_winbond_id_is_reported_at_its_own_length(void) {
    static const uint8_t uid[16] = {0xE6, 0x63, 0x64, 0xD1, 0xE7, 0x25, 0x3A, 0x34,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    char hex[33];

    flash_chip_set(W25Q16JV, uid);
    flash_chip_uid_hex(hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("E66364D1E7253A34", hex);
}

/* Before the boot read, or on the host: nothing known, nothing refused. */
static void test_FC_10_no_read_is_unknown(void) {
    static const uint8_t none[3] = {0, 0, 0};
    static const uint8_t uid[16] = {0};
    char hex[33];

    flash_chip_set(none, uid);
    TEST_ASSERT_EQUAL_UINT32(0, flash_chip_bytes());
    flash_chip_jedec_hex(hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("000000", hex);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FC_01_capacity_from_the_third_byte);
    RUN_TEST(test_FC_02_a_byte_that_is_not_a_size_is_unknown);
    RUN_TEST(test_FC_03_unique_id_length_by_maker);
    RUN_TEST(test_FC_04_an_unknown_maker_reads_the_sdks_eight);
    RUN_TEST(test_FC_05_a_map_larger_than_the_chip_does_not_fit);
    RUN_TEST(test_FC_06_a_map_no_larger_than_the_chip_fits);
    RUN_TEST(test_FC_07_an_unknown_chip_is_not_too_small);
    RUN_TEST(test_FC_08_the_chip_read_at_boot_is_reported);
    RUN_TEST(test_FC_09_a_winbond_id_is_reported_at_its_own_length);
    RUN_TEST(test_FC_10_no_read_is_unknown);
    return UNITY_END();
}
