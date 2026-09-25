/*
 * The beep vocabulary, the personalities, and the file format.
 *
 * The shipped personality follows Eggtimer Rocketry, whose convention most
 * fliers already have in their ear. These tests hold the two properties that
 * make a beep map trustworthy: that the good case is recognised rather than
 * counted, and that no two outcomes sound alike.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "beep_codes.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static beep_table_t base(void) {
    beep_table_t t;
    beep_codes_defaults(&t);
    return t;
}

/* ── The shipped personality ──────────────────────────────────────── */

void test_shipped_table_is_valid(void) {
    beep_table_t t = base();
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL_MESSAGE(BEEP_OK, v.err, v.what);
}

void test_ok_to_fly_is_a_chirp_not_a_count(void) {
    /* Eggtimer Quantum/Quark: ready to fly is a rapid chirp. Asking an
       operator to count the case that means "everything is fine" is how it
       gets misheard in the wind. */
    beep_table_t t = base();
    TEST_ASSERT_EQUAL_MESSAGE(BK_CHIRP, beep_codes_spec(&t, BR_OK_TO_FLY).kind,
                              "the good case should be recognised, not counted");
}

void test_shipped_pyro_codes_follow_eggtimer(void) {
    /* Eggtimer Quark: 4 = no Main continuity, 5 = no Drogue. Our pyro1 fires
       at apogee (drogue) and pyro2 at altitude (main) by default. */
    beep_table_t t = base();
    TEST_ASSERT_EQUAL(BK_CODE, beep_codes_spec(&t, BR_CHECK_PYRO_1).kind);
    TEST_ASSERT_EQUAL_MESSAGE(5, beep_codes_spec(&t, BR_CHECK_PYRO_1).d1, "channel 1 is the drogue: 5 beeps");
    TEST_ASSERT_EQUAL_MESSAGE(4, beep_codes_spec(&t, BR_CHECK_PYRO_2).d1, "channel 2 is the main: 4 beeps");
    /* Eggtimer Classic/TRS: 2 = Altimeter Sensor or Hardware Error. */
    TEST_ASSERT_EQUAL(2, beep_codes_spec(&t, BR_SYSTEM_FAILURE).d1);
}

void test_shipped_cadence_keeps_talking(void) {
    /* A board that says its state once and falls silent is indistinguishable
       from one whose battery died a second later. */
    beep_table_t t = base();
    const beep_personality_t *p = beep_codes_active(&t);
    TEST_ASSERT_EQUAL_MESSAGE(0, p->repeat, "the default repeats until launch");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, p->gap_ms, "with a gap between announcements");
}

void test_three_personalities_all_named(void) {
    beep_table_t t = base();
    for (int i = 0; i < BEEP_PERSONALITY_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(t.p[i].name[0] != '\0', "an unnamed slot cannot be chosen from a menu");
    }
    TEST_ASSERT_EQUAL_STRING("Default", t.p[0].name);
    TEST_ASSERT_EQUAL_MESSAGE(0, t.active, "the Eggtimer personality is the one in use");
}

/* ── Validation ───────────────────────────────────────────────────── */

void test_a_zero_beep_count_is_refused(void) {
    beep_table_t t = base();
    t.p[0].spec[BR_SYSTEM_FAILURE] = (beep_spec_t){BK_CODE, 0, 0};
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL(BEEP_ERR_DIGIT_RANGE, v.err);
    TEST_ASSERT_EQUAL(0, v.personality);
    TEST_ASSERT_EQUAL(BR_SYSTEM_FAILURE, v.reason);
}

void test_a_count_above_nine_is_refused(void) {
    beep_table_t t = base();
    t.p[1].spec[BR_CHECK_PYRO_1] = (beep_spec_t){BK_CODE, 12, 0};
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL(BEEP_ERR_DIGIT_RANGE, v.err);
    TEST_ASSERT_EQUAL_MESSAGE(1, v.personality, "the verdict names which personality");
}

void test_two_outcomes_that_sound_alike_are_refused(void) {
    /* "Check the pyro" and "OK to fly" sounding the same is the worst case
       this rule exists for. */
    beep_table_t t = base();
    t.p[0].spec[BR_CHECK_PYRO_1] = t.p[0].spec[BR_SYSTEM_FAILURE];
    TEST_ASSERT_EQUAL(BEEP_ERR_DUPLICATE, beep_codes_validate(&t).err);
}

void test_two_chirps_are_a_duplicate(void) {
    /* Not just counts: two outcomes both set to chirp are indistinguishable. */
    beep_table_t t = base();
    t.p[0].spec[BR_SYSTEM_FAILURE] = (beep_spec_t){BK_CHIRP, 0, 0};
    TEST_ASSERT_EQUAL(BEEP_ERR_DUPLICATE, beep_codes_validate(&t).err);
}

void test_silence_is_not_a_duplicate(void) {
    /* An outcome deliberately muted is a choice, not a collision. */
    beep_table_t t = base();
    t.p[0].spec[BR_CHECK_PYRO_1] = (beep_spec_t){BK_SILENT, 0, 0};
    t.p[0].spec[BR_CHECK_PYRO_2] = (beep_spec_t){BK_SILENT, 0, 0};
    TEST_ASSERT_EQUAL(BEEP_OK, beep_codes_validate(&t).err);
}

void test_a_wholly_silent_personality_is_refused(void) {
    beep_table_t t = base();
    for (int r = 0; r < BEEP_REASON_COUNT; r++) {
        t.p[2].spec[r] = (beep_spec_t){BK_SILENT, 0, 0};
    }
    beep_verdict_t v = beep_codes_validate(&t);
    TEST_ASSERT_EQUAL(BEEP_ERR_ALL_SILENT, v.err);
    TEST_ASSERT_EQUAL(2, v.personality);
}

void test_an_active_slot_that_does_not_exist_is_refused(void) {
    beep_table_t t = base();
    t.active = BEEP_PERSONALITY_COUNT;
    TEST_ASSERT_EQUAL(BEEP_ERR_NO_ACTIVE, beep_codes_validate(&t).err);
}

/* ── Merged pyro channels ─────────────────────────────────────────── */

void test_merging_the_channels_makes_them_one_sound(void) {
    beep_table_t t = base();
    t.p[0].split_pyro = false;
    TEST_ASSERT_EQUAL_MESSAGE(beep_codes_spec(&t, BR_CHECK_PYRO_1).d1, beep_codes_spec(&t, BR_CHECK_PYRO_2).d1,
                              "with the channels merged, either one says the same thing");
}

void test_merged_channels_may_share_a_code(void) {
    /* Channel 2 is never played when merged, so its spec must not be
       compared against anything -- otherwise merging would look like a
       duplicate. */
    beep_table_t t = base();
    t.p[0].split_pyro = false;
    t.p[0].spec[BR_CHECK_PYRO_2] = t.p[0].spec[BR_CHECK_PYRO_1];
    TEST_ASSERT_EQUAL(BEEP_OK, beep_codes_validate(&t).err);
}

/* ── Lookup ───────────────────────────────────────────────────────── */

void test_the_active_personality_is_the_one_used(void) {
    beep_table_t t = base();
    t.p[1].spec[BR_SYSTEM_FAILURE] = (beep_spec_t){BK_CODE, 9, 0};
    t.active = 1;
    TEST_ASSERT_EQUAL(9, beep_codes_spec(&t, BR_SYSTEM_FAILURE).d1);
}

void test_an_unreadable_table_still_answers(void) {
    /* A board beeps its self-test early, possibly before beep.ini is read. */
    TEST_ASSERT_EQUAL_MESSAGE(BK_CHIRP, beep_codes_spec(NULL, BR_OK_TO_FLY).kind, "a null table must still answer");
    beep_table_t t;
    memset(&t, 0, sizeof(t));
    t.active = 250;
    TEST_ASSERT_EQUAL(BK_CHIRP, beep_codes_spec(&t, BR_OK_TO_FLY).kind);
}

void test_an_unknown_outcome_says_leave_the_pad(void) {
    beep_table_t t = base();
    beep_spec_t sp = beep_codes_spec(&t, (beep_reason_t)999);
    TEST_ASSERT_EQUAL_MESSAGE(t.p[0].spec[BR_SYSTEM_FAILURE].d1, sp.d1, "the safe thing to say is 'leave the pad'");
}

/* ── File format ──────────────────────────────────────────────────── */

void test_round_trip_preserves_everything(void) {
    beep_table_t t = base();
    t.active = 2;
    snprintf(t.p[2].name, sizeof(t.p[2].name), "Loud");
    t.p[2].spec[BR_OK_TO_FLY] = (beep_spec_t){BK_TONE, 0, 0};
    t.p[2].spec[BR_SYSTEM_FAILURE] = (beep_spec_t){BK_CODE, 3, 7};
    t.p[2].gap_ms = 1500;
    t.p[2].repeat = 4;
    t.p[2].split_pyro = false;

    char buf[1024];
    int n = beep_codes_serialize_ini(&t, buf, (int)sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    beep_table_t back;
    beep_codes_defaults(&back);
    beep_codes_parse_ini(buf, &back);
    TEST_ASSERT_EQUAL(2, back.active);
    TEST_ASSERT_EQUAL_STRING("Loud", back.p[2].name);
    TEST_ASSERT_EQUAL(BK_TONE, back.p[2].spec[BR_OK_TO_FLY].kind);
    TEST_ASSERT_EQUAL(3, back.p[2].spec[BR_SYSTEM_FAILURE].d1);
    TEST_ASSERT_EQUAL(7, back.p[2].spec[BR_SYSTEM_FAILURE].d2);
    TEST_ASSERT_EQUAL(1500, back.p[2].gap_ms);
    TEST_ASSERT_EQUAL(4, back.p[2].repeat);
    TEST_ASSERT_FALSE(back.p[2].split_pyro);
}

void test_the_file_reads_the_way_it_sounds(void) {
    beep_table_t t = base();
    char buf[1024];
    TEST_ASSERT_GREATER_THAN(0, beep_codes_serialize_ini(&t, buf, (int)sizeof(buf)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "p0_ok_to_fly=chirp"), "a chirp should say chirp");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "p0_check_pyro_1=code:5"), "five beeps should say code:5");
    TEST_ASSERT_NOT_NULL(strstr(buf, "p0_name=Default"));
}

void test_unknown_keys_are_ignored(void) {
    beep_table_t t = base();
    char in[] = "[beeps]\r\nnot_a_key=99\r\np9_name=Nope\r\np0_ok_to_fly=tone\r\n";
    beep_codes_parse_ini(in, &t);
    TEST_ASSERT_EQUAL(BK_TONE, t.p[0].spec[BR_OK_TO_FLY].kind);
}

void test_a_malformed_value_leaves_the_entry_alone(void) {
    /* A garbled line must not silently mute an outcome. */
    beep_table_t t = base();
    uint8_t before = t.p[0].spec[BR_CHECK_PYRO_1].d1;
    char in[] = "[beeps]\r\np0_check_pyro_1=code:\r\np0_system_failure=warble\r\np0_ok_to_fly=code:1-2-3\r\n";
    beep_codes_parse_ini(in, &t);
    TEST_ASSERT_EQUAL(before, t.p[0].spec[BR_CHECK_PYRO_1].d1);
    TEST_ASSERT_EQUAL(BK_CHIRP, t.p[0].spec[BR_OK_TO_FLY].kind);
    TEST_ASSERT_EQUAL(BEEP_OK, beep_codes_validate(&t).err);
}

void test_serialize_refuses_to_overflow(void) {
    beep_table_t t = base();
    char small[32];
    TEST_ASSERT_EQUAL(-1, beep_codes_serialize_ini(&t, small, (int)sizeof(small)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_shipped_table_is_valid);
    RUN_TEST(test_ok_to_fly_is_a_chirp_not_a_count);
    RUN_TEST(test_shipped_pyro_codes_follow_eggtimer);
    RUN_TEST(test_shipped_cadence_keeps_talking);
    RUN_TEST(test_three_personalities_all_named);
    RUN_TEST(test_a_zero_beep_count_is_refused);
    RUN_TEST(test_a_count_above_nine_is_refused);
    RUN_TEST(test_two_outcomes_that_sound_alike_are_refused);
    RUN_TEST(test_two_chirps_are_a_duplicate);
    RUN_TEST(test_silence_is_not_a_duplicate);
    RUN_TEST(test_a_wholly_silent_personality_is_refused);
    RUN_TEST(test_an_active_slot_that_does_not_exist_is_refused);
    RUN_TEST(test_merging_the_channels_makes_them_one_sound);
    RUN_TEST(test_merged_channels_may_share_a_code);
    RUN_TEST(test_the_active_personality_is_the_one_used);
    RUN_TEST(test_an_unreadable_table_still_answers);
    RUN_TEST(test_an_unknown_outcome_says_leave_the_pad);
    RUN_TEST(test_round_trip_preserves_everything);
    RUN_TEST(test_the_file_reads_the_way_it_sounds);
    RUN_TEST(test_unknown_keys_are_ignored);
    RUN_TEST(test_a_malformed_value_leaves_the_entry_alone);
    RUN_TEST(test_serialize_refuses_to_overflow);
    return UNITY_END();
}
