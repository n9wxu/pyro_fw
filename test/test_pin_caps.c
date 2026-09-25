/*
 * Pin capability table tests.
 *
 * Built against MK1A's real table, because a table that only a test writes is
 * a test of the test. MK1A is the board with the fullest shape: two switched
 * high sides, a common low side, and two ADC sense pads.
 *
 * The structural invariants -- analog only on ADC pads, one PG_COMMON, the
 * sensor bus never Lua-assignable -- are _Static_assert in src/pin_model.h
 * and fail the build, so they are not repeated here. What these cover is the
 * runtime surface every later phase will call.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "pin_model.h"
#include "pin_caps.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── The table itself ─────────────────────────────────────────────── */

void test_table_is_not_empty(void) {
    int n = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_find_returns_the_row_for_a_declared_pin(void) {
    const pin_cap_t *c = pin_caps_find(9); /* FIRE1 */
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(9, c->pin);
    TEST_ASSERT_TRUE(c->functions & FN_PYRO_FIRE);
    TEST_ASSERT_EQUAL(PG_CH1, c->group);
}

/* A pin with no row is not assignable to anything. GPIO2 is not wired to
 * anything this board exposes. */
void test_find_returns_null_for_an_undeclared_pin(void) {
    TEST_ASSERT_NULL(pin_caps_find(2));
}

void test_every_pin_appears_at_most_once(void) {
    int n = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(t[i].pin, t[j].pin, "a pin is declared twice");
        }
    }
}

/* ── The released-pyro model ──────────────────────────────────────── */

/* A firing pad advertises BOTH its pyro function and what it may become once
 * the channel is released. Holding both at once is an assignment-time rule,
 * not a property of the hardware, so the table must express the pairing. */
void test_a_firing_pad_also_offers_what_it_becomes_when_released(void) {
    const pin_cap_t *c = pin_caps_find(9);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(c->functions & FN_PYRO_FIRE);
    TEST_ASSERT_TRUE(c->functions & FN_DIGITAL);
    TEST_ASSERT_TRUE(c->functions & FN_BRIDGE);
}

/* ...and is therefore NOT Lua's by default. It takes a release to get it. */
void test_a_firing_pad_is_not_lua_by_default(void) {
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(9)));
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(10)));
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(11)));
}

void test_a_user_pad_is_lua_by_default(void) {
    TEST_ASSERT_TRUE(pin_caps_is_default_lua(pin_caps_find(18)));
    TEST_ASSERT_TRUE(pin_caps_is_default_lua(pin_caps_find(19)));
}

void test_reserved_pads_are_never_lua_by_default(void) {
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(0)));  /* UART TX */
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(20))); /* I2C SDA */
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(25))); /* LED     */
    TEST_ASSERT_FALSE(pin_caps_is_default_lua(pin_caps_find(26))); /* sense   */
}

/* ── Power group ──────────────────────────────────────────────────── */

void test_the_group_has_two_channels_and_one_common(void) {
    int n = 0, ch1 = 0, ch2 = 0, common = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    for (int i = 0; i < n; i++) {
        if (t[i].group == PG_CH1)
            ch1++;
        else if (t[i].group == PG_CH2)
            ch2++;
        else if (t[i].group == PG_COMMON)
            common++;
    }
    TEST_ASSERT_EQUAL(1, ch1);
    TEST_ASSERT_EQUAL(1, ch2);
    TEST_ASSERT_EQUAL(1, common);
}

/* MK1A switches the high side per channel and shares the low side, so a
 * released FIRE plus a released PYRO_LOW is a genuine bridge. */
void test_bridge_is_possible_on_this_board(void) {
    TEST_ASSERT_EQUAL(PYRO_TOPO_HIGH_SWITCHED, BOARD_PYRO_TOPOLOGY);
    TEST_ASSERT_TRUE(pin_caps_bridge_possible());
}

/* F1 is 8 A and does not come back, which is what the UI warning has to say
 * on this board and not on MK1B. */
void test_protection_class_is_declared(void) {
    TEST_ASSERT_EQUAL(PYRO_PROT_FUSE_ONESHOT, BOARD_PYRO_PROTECTION);
}

/* ── LUA_PIN_LIST agrees with the table ───────────────────────────── */

void test_the_default_lua_list_is_consistent(void) {
    TEST_ASSERT_EQUAL_MESSAGE(-1, pin_caps_check_lua_list(),
                              "a pin in LUA_PIN_LIST is reserved or not Lua-capable in the table");
}

/* [PIN-LABEL-01] Every row carries a connector designator. The build-time
 * assertion catches an empty one; this catches a table that has drifted away
 * from the board -- a label is only useful if it is the one on the silkscreen,
 * and nothing but a human can check that. What is checkable is that they are
 * present, distinct, and short enough to render. */
void test_PIN_LABEL_01_every_row_is_labelled(void) {
    int n = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, n, "the board declares no pins at all");
    for (int i = 0; i < n; i++) {
        char m[96];
        snprintf(m, sizeof(m), "GPIO%u has no connector label", (unsigned)t[i].pin);
        TEST_ASSERT_NOT_NULL_MESSAGE(t[i].label, m);
        TEST_ASSERT_TRUE_MESSAGE(t[i].label[0] != '\0', m);
        snprintf(m, sizeof(m), "GPIO%u's label is too long for the pin table", (unsigned)t[i].pin);
        TEST_ASSERT_TRUE_MESSAGE(strlen(t[i].label) < 24, m);
    }
}

/* A label that names two different pads is worse than none: it sends the
 * operator to the wrong screw terminal. The two J6 user pads on MK1A are the
 * deliberate exception -- they really are one connector. */
void test_PIN_LABEL_02_labels_are_distinct(void) {
    int n = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(t[i].label, "J6 user pad") == 0) {
                continue;
            }
            char m[128];
            snprintf(m, sizeof(m), "GPIO%u and GPIO%u both claim \"%s\"", (unsigned)t[i].pin, (unsigned)t[j].pin,
                     t[i].label);
            TEST_ASSERT_TRUE_MESSAGE(strcmp(t[i].label, t[j].label) != 0, m);
        }
    }
}

/* pin_caps_label() must never hand a caller NULL to print. */
void test_PIN_LABEL_03_unknown_pin_is_empty_not_null(void) {
    const char *l = pin_caps_label(2); /* no row on any board here */
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_STRING("", l);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_is_not_empty);
    RUN_TEST(test_find_returns_the_row_for_a_declared_pin);
    RUN_TEST(test_find_returns_null_for_an_undeclared_pin);
    RUN_TEST(test_every_pin_appears_at_most_once);
    RUN_TEST(test_a_firing_pad_also_offers_what_it_becomes_when_released);
    RUN_TEST(test_a_firing_pad_is_not_lua_by_default);
    RUN_TEST(test_a_user_pad_is_lua_by_default);
    RUN_TEST(test_reserved_pads_are_never_lua_by_default);
    RUN_TEST(test_the_group_has_two_channels_and_one_common);
    RUN_TEST(test_bridge_is_possible_on_this_board);
    RUN_TEST(test_protection_class_is_declared);
    RUN_TEST(test_the_default_lua_list_is_consistent);
    RUN_TEST(test_PIN_LABEL_01_every_row_is_labelled);
    RUN_TEST(test_PIN_LABEL_02_labels_are_distinct);
    RUN_TEST(test_PIN_LABEL_03_unknown_pin_is_empty_not_null);
    return UNITY_END();
}
