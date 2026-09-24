/*
 * The beep vocabulary and its file format.
 *
 * The codes used to be #defines whose meaning lived only in the macro name,
 * so there was nothing to test. Now that an operator can change them, the two
 * ways a change can make a board lie have to be refused: a digit that cannot
 * be heard, and two reasons sharing one code.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "beep_codes.h"
#include "buzzer.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static beep_table_t base(void) {
    beep_table_t t;
    beep_codes_defaults(&t);
    return t;
}

/* ── The shipped table ────────────────────────────────────────────── */

void test_shipped_codes_are_valid(void) {
    beep_table_t t = base();
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL_MESSAGE(BEEP_OK, v.err, v.what);
}

void test_every_reason_has_a_key_and_a_description(void) {
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(beep_codes_key((beep_reason_t)i)[0] != '\0', "a reason with no key cannot be stored");
        TEST_ASSERT_TRUE_MESSAGE(strlen(beep_codes_description((beep_reason_t)i)) > 10,
                                 "a reason with no description is a code nobody can look up");
    }
}

void test_no_two_shipped_reasons_share_a_code(void) {
    /* The property the operator relies on: hear a code, get one answer. */
    beep_table_t t = base();
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        for (int j = i + 1; j < BEEP_REASON_COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(t.code[i], t.code[j], "two shipped reasons share a code");
        }
    }
}

/* ── Validation ───────────────────────────────────────────────────── */

void test_a_zero_digit_is_refused(void) {
    beep_table_t t = base();
    t.code[BR_ALL_GOOD] = BEEP_CODE(1, 0); /* zero beeps cannot be heard */
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL(BEEP_ERR_DIGIT_RANGE, v.err);
    TEST_ASSERT_EQUAL(BR_ALL_GOOD, v.reason);
}

void test_a_digit_above_nine_is_refused(void) {
    /* A nibble holds up to 15, but nobody counts 15 beeps correctly. */
    beep_table_t t = base();
    t.code[BR_P1_OPEN] = BEEP_CODE(2, 12);
    TEST_ASSERT_EQUAL(BEEP_ERR_DIGIT_RANGE, beep_codes_validate(&t).err);
}

void test_a_duplicate_code_is_refused(void) {
    beep_table_t t = base();
    t.code[BR_P2_OPEN] = t.code[BR_P1_OPEN];
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL(BEEP_ERR_DUPLICATE, v.err);
    TEST_ASSERT_EQUAL_MESSAGE(BR_P2_OPEN, v.reason, "the verdict names the second of the pair");
}

/* ── Lookup ───────────────────────────────────────────────────────── */

void test_get_returns_the_assigned_code(void) {
    beep_table_t t = base();
    t.code[BR_SENSOR_FAIL] = BEEP_CODE(7, 7);
    TEST_ASSERT_EQUAL_HEX8(BEEP_CODE(7, 7), beep_codes_get(&t, BR_SENSOR_FAIL));
}

void test_an_unset_entry_falls_back_to_the_shipped_code(void) {
    /* A board beeps its self-test result early, possibly before beep.ini has
       been read. Zero is not a valid code, so it means "not loaded". */
    beep_table_t t;
    memset(&t, 0, sizeof(t));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BEEP_CODE(4, 1), beep_codes_get(&t, BR_SENSOR_FAIL),
                                   "an unloaded table must still answer");
    TEST_ASSERT_NOT_EQUAL(0, beep_codes_get(NULL, BR_ALL_GOOD));
}

void test_an_unknown_reason_still_reports_something(void) {
    beep_table_t t = base();
    TEST_ASSERT_NOT_EQUAL(0, beep_codes_get(&t, (beep_reason_t)999));
    TEST_ASSERT_NOT_EQUAL(0, beep_codes_get(&t, (beep_reason_t)-1));
}

/* ── File format ──────────────────────────────────────────────────── */

void test_round_trip_preserves_the_table(void) {
    beep_table_t t = base();
    t.code[BR_ALL_GOOD] = BEEP_CODE(9, 9);
    t.code[BR_CRITICAL] = BEEP_CODE(8, 7);

    char buf[BEEP_REASON_COUNT * 32];
    int n = beep_codes_serialize_ini(&t, buf, (int)sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    beep_table_t back;
    beep_codes_defaults(&back);
    beep_codes_parse_ini(buf, &back);
    TEST_ASSERT_EQUAL_HEX8(BEEP_CODE(9, 9), back.code[BR_ALL_GOOD]);
    TEST_ASSERT_EQUAL_HEX8(BEEP_CODE(8, 7), back.code[BR_CRITICAL]);
    for (int i = 0; i < BEEP_REASON_COUNT; i++) {
        TEST_ASSERT_EQUAL_HEX8(t.code[i], back.code[i]);
    }
}

void test_the_file_carries_digits_not_the_packed_byte(void) {
    /* An operator edits this file and 41 is what they hear. */
    beep_table_t t = base();
    char buf[BEEP_REASON_COUNT * 32];
    TEST_ASSERT_GREATER_THAN(0, beep_codes_serialize_ini(&t, buf, (int)sizeof(buf)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "sensor_fail=41"), "the sensor code should read as its digits");
    TEST_ASSERT_NOT_NULL(strstr(buf, "all_good=11"));
}

void test_unknown_keys_are_ignored(void) {
    beep_table_t t = base();
    char in[] = "[beeps]\r\nnot_a_reason=99\r\nall_good=77\r\n";
    beep_codes_parse_ini(in, &t);
    TEST_ASSERT_EQUAL_HEX8(BEEP_CODE(7, 7), t.code[BR_ALL_GOOD]);
    TEST_ASSERT_EQUAL(BEEP_OK, beep_codes_validate(&t).err);
}

void test_a_malformed_value_leaves_the_entry_alone(void) {
    /* Not three digits, not one, not letters. The whole-table validate is
       what refuses a bad map; a garbled line must not silently zero a code. */
    beep_table_t t = base();
    uint8_t before = t.code[BR_ALL_GOOD];
    char in[] = "[beeps]\r\nall_good=123\r\np1_open=x\r\np2_open=4\r\n";
    beep_codes_parse_ini(in, &t);
    TEST_ASSERT_EQUAL_HEX8(before, t.code[BR_ALL_GOOD]);
    TEST_ASSERT_EQUAL(BEEP_OK, beep_codes_validate(&t).err);
}

void test_serialize_refuses_to_overflow(void) {
    beep_table_t t = base();
    char small[16];
    TEST_ASSERT_EQUAL(-1, beep_codes_serialize_ini(&t, small, (int)sizeof(small)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_shipped_codes_are_valid);
    RUN_TEST(test_every_reason_has_a_key_and_a_description);
    RUN_TEST(test_no_two_shipped_reasons_share_a_code);
    RUN_TEST(test_a_zero_digit_is_refused);
    RUN_TEST(test_a_digit_above_nine_is_refused);
    RUN_TEST(test_a_duplicate_code_is_refused);
    RUN_TEST(test_get_returns_the_assigned_code);
    RUN_TEST(test_an_unset_entry_falls_back_to_the_shipped_code);
    RUN_TEST(test_an_unknown_reason_still_reports_something);
    RUN_TEST(test_round_trip_preserves_the_table);
    RUN_TEST(test_the_file_carries_digits_not_the_packed_byte);
    RUN_TEST(test_unknown_keys_are_ignored);
    RUN_TEST(test_a_malformed_value_leaves_the_entry_alone);
    RUN_TEST(test_serialize_refuses_to_overflow);
    return UNITY_END();
}
