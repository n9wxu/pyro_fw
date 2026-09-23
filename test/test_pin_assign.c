/*
 * Pin assignment rules, against MK1A's real capability table.
 *
 * The release matrix is the point: one channel, the other channel, both,
 * neither -- and the common, which is the case a per-channel rule gets wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "pin_assign.h"
#include "pin_caps.h"
#include <string.h>

/* MK1A: FIRE1 is PG_CH1, FIRE2 is PG_CH2, PYRO_LOW is PG_COMMON. */
#define FIRE1 9
#define PYRO_LOW 10
#define FIRE2 11
#define USER_PAD 18
#define SENSE1 26

void setUp(void) {}
void tearDown(void) {}

static pin_assign_t base(void) {
    pin_assign_t a;
    pin_assign_defaults(&a);
    return a;
}

static void set_role(pin_assign_t *a, uint8_t pin, uint8_t role, const char *name) {
    a->role[pin] = role;
    if (name) {
        strncpy(a->name[pin], name, LUA_NAME_MAX - 1);
    }
}

/* ── Defaults ─────────────────────────────────────────────────────── */

void test_defaults_are_valid_and_release_nothing(void) {
    pin_assign_t a = base();
    TEST_ASSERT_FALSE(a.pyro1_released);
    TEST_ASSERT_FALSE(a.pyro2_released);
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

void test_a_user_pad_needs_no_release(void) {
    pin_assign_t a = base();
    set_role(&a, USER_PAD, LUA_ROLE_OUT, "led");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* ── The release matrix ───────────────────────────────────────────── */

void test_a_retained_channel_keeps_its_pin(void) {
    pin_assign_t a = base();
    set_role(&a, FIRE1, LUA_ROLE_OUT, "winch");
    pin_verdict_t v = pin_assign_validate(&a);
    TEST_ASSERT_EQUAL(PIN_ERR_PYRO_RETAINED, v.err);
    TEST_ASSERT_EQUAL(FIRE1, v.pin);
}

void test_releasing_one_channel_frees_its_own_pin(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    set_role(&a, FIRE1, LUA_ROLE_OUT, "winch");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* ...and only its own. Releasing channel 1 does not hand over channel 2. */
void test_releasing_one_channel_does_not_free_the_other(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    set_role(&a, FIRE2, LUA_ROLE_OUT, "other");
    pin_verdict_t v = pin_assign_validate(&a);
    TEST_ASSERT_EQUAL(PIN_ERR_PYRO_RETAINED, v.err);
    TEST_ASSERT_EQUAL(FIRE2, v.pin);
}

/* The common is the case a per-channel rule gets wrong: it stays held while
 * EITHER channel still needs it. */
void test_the_common_is_held_while_one_channel_is_retained(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    set_role(&a, PYRO_LOW, LUA_ROLE_OUT, "pump");
    pin_verdict_t v = pin_assign_validate(&a);
    TEST_ASSERT_EQUAL(PIN_ERR_COMMON_HELD, v.err);
    TEST_ASSERT_EQUAL(PYRO_LOW, v.pin);
}

void test_the_common_is_held_when_the_other_channel_is_retained(void) {
    pin_assign_t a = base();
    a.pyro2_released = true;
    set_role(&a, PYRO_LOW, LUA_ROLE_OUT, "pump");
    TEST_ASSERT_EQUAL(PIN_ERR_COMMON_HELD, pin_assign_validate(&a).err);
}

void test_releasing_both_channels_frees_the_common(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, PYRO_LOW, LUA_ROLE_OUT, "pump");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* Both released: three digital pins. */
void test_both_released_yields_three_digital_pins(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_OUT, "a");
    set_role(&a, FIRE2, LUA_ROLE_PWM, "b");
    set_role(&a, PYRO_LOW, LUA_ROLE_OUT, "c");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* ── Capability ───────────────────────────────────────────────────── */

void test_a_role_the_pin_cannot_take_is_refused(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    /* The common is a plain gate: digital and bridge, but no PIO serial. */
    set_role(&a, PYRO_LOW, LUA_ROLE_TX, "uart");
    TEST_ASSERT_EQUAL(PIN_ERR_NOT_CAPABLE, pin_assign_validate(&a).err);
}

void test_an_undeclared_pin_is_refused(void) {
    pin_assign_t a = base();
    set_role(&a, 2, LUA_ROLE_OUT, "nope"); /* GPIO2 has no row on MK1A */
    TEST_ASSERT_EQUAL(PIN_ERR_UNKNOWN_PIN, pin_assign_validate(&a).err);
}

void test_a_sense_pad_is_not_assignable(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, SENSE1, LUA_ROLE_OUT, "x");
    TEST_ASSERT_NOT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* ── The bridge ───────────────────────────────────────────────────── */

void test_a_bridge_needs_both_halves(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "motor");
    TEST_ASSERT_EQUAL(PIN_ERR_BRIDGE_INCOMPLETE, pin_assign_validate(&a).err);
}

void test_a_bridge_needs_the_common_not_two_channels(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "m1");
    set_role(&a, FIRE2, LUA_ROLE_BRIDGE, "m2");
    TEST_ASSERT_EQUAL(PIN_ERR_BRIDGE_INCOMPLETE, pin_assign_validate(&a).err);
}

void test_a_complete_bridge_is_accepted(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "motor");
    set_role(&a, PYRO_LOW, LUA_ROLE_BRIDGE, "motor_lo");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

/* A bridge plus the remaining channel as a digital output: the other half of
 * "both released yields one half-bridge plus one digital". */
void test_a_bridge_leaves_the_third_pin_usable(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "motor");
    set_role(&a, PYRO_LOW, LUA_ROLE_BRIDGE, "motor_lo");
    set_role(&a, FIRE2, LUA_ROLE_PWM, "fan");
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&a).err);
}

void test_a_bridge_still_needs_the_channel_released(void) {
    pin_assign_t a = base();
    a.pyro2_released = true; /* channel 1 retained */
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "motor");
    set_role(&a, PYRO_LOW, LUA_ROLE_BRIDGE, "motor_lo");
    TEST_ASSERT_EQUAL(PIN_ERR_PYRO_RETAINED, pin_assign_validate(&a).err);
}

/* ── Names ────────────────────────────────────────────────────────── */

void test_two_resources_cannot_share_a_name(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    set_role(&a, USER_PAD, LUA_ROLE_OUT, "same");
    set_role(&a, FIRE1, LUA_ROLE_OUT, "same");
    TEST_ASSERT_EQUAL(PIN_ERR_DUPLICATE_NAME, pin_assign_validate(&a).err);
}

/* ── pins.ini round trip ──────────────────────────────────────────── */

void test_round_trip_preserves_the_assignment(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    a.pyro2_released = true;
    set_role(&a, FIRE1, LUA_ROLE_BRIDGE, "motor");
    set_role(&a, PYRO_LOW, LUA_ROLE_BRIDGE, "motor_lo");
    set_role(&a, USER_PAD, LUA_ROLE_PIXEL, "strip");

    char buf[1024];
    int n = pin_assign_serialize_ini(&a, buf, (int)sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    pin_assign_t b;
    pin_assign_defaults(&b);
    pin_assign_parse_ini(buf, &b);

    TEST_ASSERT_TRUE(b.pyro1_released);
    TEST_ASSERT_TRUE(b.pyro2_released);
    TEST_ASSERT_EQUAL(LUA_ROLE_BRIDGE, b.role[FIRE1]);
    TEST_ASSERT_EQUAL_STRING("motor", b.name[FIRE1]);
    TEST_ASSERT_EQUAL(LUA_ROLE_BRIDGE, b.role[PYRO_LOW]);
    TEST_ASSERT_EQUAL(LUA_ROLE_PIXEL, b.role[USER_PAD]);
    TEST_ASSERT_EQUAL_STRING("strip", b.name[USER_PAD]);
    TEST_ASSERT_EQUAL(PIN_OK, pin_assign_validate(&b).err);
}

void test_unknown_keys_are_ignored(void) {
    pin_assign_t a = base();
    char buf[] = "[pins]\r\npyro1_released=true\r\nsomething_new=42\r\np18_role=out\r\n";
    pin_assign_parse_ini(buf, &a);
    TEST_ASSERT_TRUE(a.pyro1_released);
    TEST_ASSERT_EQUAL(LUA_ROLE_OUT, a.role[USER_PAD]);
}

void test_serialize_refuses_to_overflow(void) {
    pin_assign_t a = base();
    a.pyro1_released = true;
    char arena[96];
    const int usable = 24;
    memset(arena, 0x7f, sizeof(arena));
    TEST_ASSERT_LESS_OR_EQUAL(0, pin_assign_serialize_ini(&a, arena, usable));
    for (size_t i = (size_t)usable; i < sizeof(arena); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7f, (unsigned char)arena[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid_and_release_nothing);
    RUN_TEST(test_a_user_pad_needs_no_release);

    RUN_TEST(test_a_retained_channel_keeps_its_pin);
    RUN_TEST(test_releasing_one_channel_frees_its_own_pin);
    RUN_TEST(test_releasing_one_channel_does_not_free_the_other);
    RUN_TEST(test_the_common_is_held_while_one_channel_is_retained);
    RUN_TEST(test_the_common_is_held_when_the_other_channel_is_retained);
    RUN_TEST(test_releasing_both_channels_frees_the_common);
    RUN_TEST(test_both_released_yields_three_digital_pins);

    RUN_TEST(test_a_role_the_pin_cannot_take_is_refused);
    RUN_TEST(test_an_undeclared_pin_is_refused);
    RUN_TEST(test_a_sense_pad_is_not_assignable);

    RUN_TEST(test_a_bridge_needs_both_halves);
    RUN_TEST(test_a_bridge_needs_the_common_not_two_channels);
    RUN_TEST(test_a_complete_bridge_is_accepted);
    RUN_TEST(test_a_bridge_leaves_the_third_pin_usable);
    RUN_TEST(test_a_bridge_still_needs_the_channel_released);

    RUN_TEST(test_two_resources_cannot_share_a_name);

    RUN_TEST(test_round_trip_preserves_the_assignment);
    RUN_TEST(test_unknown_keys_are_ignored);
    RUN_TEST(test_serialize_refuses_to_overflow);
    return UNITY_END();
}
