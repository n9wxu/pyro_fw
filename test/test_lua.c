/*
 * Lua sandbox and resource-limit tests.
 *
 * These are the tests that decide whether the feature is safe to ship. The
 * bindings are the security boundary — RP2040 has no MPU — so every claim
 * made about what a script cannot do is asserted here rather than reasoned
 * about.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "pyro_lua.h"
#include "lua_platform.h"
#include <string.h>

/* Simulator-side hooks */
void sim_lua_set_flight(int state, int32_t alt_cm, int32_t speed_cms, int32_t pressure_pa, int32_t max_alt_cm,
                        uint32_t time_ms);
void sim_lua_set_pyro(int channel, int status);
void sim_lua_set_input(int idx, int value);
int sim_lua_output_value(int idx);
int sim_lua_pixel_r(int i);
int sim_lua_pixel_g(int i);
int sim_lua_pixel_b(int i);
const char *sim_lua_uart_tx(void);
void sim_lua_uart_tx_clear(void);
void sim_lua_uart_rx_push(const char *s);
const char *sim_lua_console(void);
void sim_lua_console_clear(void);

static bool run(const char *src) {
    return pyro_lua_load("=test", src, strlen(src));
}

void setUp(void) {
    pyro_lua_init();
    sim_lua_console_clear();
    sim_lua_uart_tx_clear();
}

void tearDown(void) {
    pyro_lua_shutdown();
}

/* ── The API works at all ─────────────────────────────────────────── */

static void test_output_set_and_get(void) {
    TEST_ASSERT_TRUE(run("output.set('beacon', 42)"));
    TEST_ASSERT_EQUAL_INT(42, sim_lua_output_value(0));
    TEST_ASSERT_TRUE(run("output.set('beacon', true)"));
    TEST_ASSERT_EQUAL_INT(100, sim_lua_output_value(0));
    TEST_ASSERT_TRUE(run("output.set('beacon', false)"));
    TEST_ASSERT_EQUAL_INT(0, sim_lua_output_value(0));
}

static void test_output_list_is_exactly_configured(void) {
    TEST_ASSERT_TRUE(run("n = #output.list()"));
    TEST_ASSERT_TRUE(run("print(#output.list())"));
    TEST_ASSERT_EQUAL_STRING("3\n", sim_lua_console());
}

static void test_non_dimmable_rejects_partial(void) {
    /* 'aux' is digital-only; a partial duty must be refused rather than
     * silently rounded, so a script author learns the pin cannot dim. */
    TEST_ASSERT_FALSE(run("output.set('aux', 50)"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "not dimmable"));
    TEST_ASSERT_TRUE(run("output.set('aux', true)"));
}

/* ── The sandbox: what a script must NOT be able to do ────────────── */

static void test_unknown_output_name_is_refused(void) {
    TEST_ASSERT_FALSE(run("output.set('pyro1', true)"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "no output named"));
}

static void test_no_pin_numbers_anywhere(void) {
    /* The API takes names, never pin numbers. A script cannot reach a pin by
     * guessing an integer, because no binding accepts one. */
    TEST_ASSERT_FALSE(run("output.set(17, true)"));
    TEST_ASSERT_FALSE(run("output.set(6, true)"));
    TEST_ASSERT_FALSE(run("output.set(11, true)"));
}

static void test_no_gpio_or_sdk_globals(void) {
    static const char *forbidden[] = {"gpio", "uart", "i2c", "spi", "pio", "dma", "flash", "os", "io", "package"};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        char src[64];
        snprintf(src, sizeof(src), "assert(%s == nil)", forbidden[i]);
        TEST_ASSERT_TRUE_MESSAGE(run(src), forbidden[i]);
    }
}

static void test_dangerous_base_functions_removed(void) {
    static const char *banned[] = {"dofile", "loadfile", "load", "require", "collectgarbage"};
    for (unsigned i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        char src[64];
        snprintf(src, sizeof(src), "assert(%s == nil)", banned[i]);
        TEST_ASSERT_TRUE_MESSAGE(run(src), banned[i]);
    }
}

static void test_pyro_is_read_only(void) {
    /* Reading is allowed; there is no setter to call. DESIGN.md 7.1 says a
     * fire command names an authority and there is no third source. */
    sim_lua_set_pyro(1, LUA_PYRO_CONTINUITY | LUA_PYRO_ARMED);
    TEST_ASSERT_TRUE(run("s = pyro.status(1) assert(s.continuity) assert(s.armed) assert(not s.fired)"));
    TEST_ASSERT_TRUE(run("assert(pyro.fire == nil)"));
    TEST_ASSERT_TRUE(run("assert(pyro.arm == nil)"));
    TEST_ASSERT_TRUE(run("assert(pyro.disarm == nil)"));
}

static void test_pyro_channel_validated(void) {
    TEST_ASSERT_FALSE(run("pyro.status(3)"));
    TEST_ASSERT_FALSE(run("pyro.status(0)"));
}

/* ── Resource limits ──────────────────────────────────────────────── */

static void test_infinite_loop_is_aborted(void) {
    /* The whole point: a runaway script must end, not hang the caller. */
    TEST_ASSERT_FALSE(run("while true do end"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "budget"));
}

static void test_infinite_loop_inside_pcall_still_aborted(void) {
    /* A script must not be able to swallow its own budget error and continue. */
    TEST_ASSERT_FALSE(run("pcall(function() while true do end end) while true do end"));
}

static void test_memory_exhaustion_fails_cleanly(void) {
    /* Allocating without bound must fail the script, not the host. */
    TEST_ASSERT_FALSE(run("t = {} for i = 1, 1000000 do t[i] = ('x'):rep(64) end"));
    lua_arena_stats_t st;
    pyro_lua_get_stats(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.failures > 0 || strstr(pyro_lua_last_error(), "budget") != NULL,
                             "expected an arena refusal or a budget abort");
}

static void test_vm_usable_after_a_failure(void) {
    TEST_ASSERT_FALSE(run("while true do end"));
    /* The host must be able to keep going with a fresh script. */
    TEST_ASSERT_TRUE(run("output.set('beacon', 7)"));
    TEST_ASSERT_EQUAL_INT(7, sim_lua_output_value(0));
}

static void test_syntax_error_is_reported_not_fatal(void) {
    TEST_ASSERT_FALSE(run("this is not lua"));
    TEST_ASSERT_TRUE(run("output.set('beacon', 1)"));
}

/* ── State access ─────────────────────────────────────────────────── */

static void test_flight_state_readable(void) {
    sim_lua_set_flight(2 /* ASCENT */, 123400, 5000, 95000, 200000, 4321);
    TEST_ASSERT_TRUE(run("assert(flight.state() == flight.ASCENT)"));
    TEST_ASSERT_TRUE(run("assert(sensor.altitude_cm() == 123400)"));
    TEST_ASSERT_TRUE(run("assert(sensor.speed_cms() == 5000)"));
    TEST_ASSERT_TRUE(run("assert(sensor.pressure_pa() == 95000)"));
    TEST_ASSERT_TRUE(run("assert(flight.max_altitude_cm() == 200000)"));
    TEST_ASSERT_TRUE(run("assert(flight.time_ms() == 4321)"));
}

/* ── Serial ───────────────────────────────────────────────────────── */

static void test_serial_write_reaches_the_wire(void) {
    TEST_ASSERT_TRUE(run("serial.write('radio', 'HELLO')"));
    TEST_ASSERT_EQUAL_STRING("HELLO", sim_lua_uart_tx());
}

static void test_serial_read_returns_nil_when_idle(void) {
    TEST_ASSERT_TRUE(run("assert(serial.read('radio') == nil)"));
}

static void test_serial_read_receives(void) {
    sim_lua_uart_rx_push("PING");
    TEST_ASSERT_TRUE(run("assert(serial.read('radio', 4) == 'PING')"));
}

static void test_unknown_serial_name_refused(void) {
    TEST_ASSERT_FALSE(run("serial.write('telemetry', 'x')"));
    TEST_ASSERT_FALSE(run("serial.write('uart0', 'x')"));
}

/* ── Addressable LEDs ─────────────────────────────────────────────── */

static void test_pixel_buffer_needs_show_to_reach_the_wire(void) {
    /* The wire protocol reclocks the whole string, so writes are buffered and
     * only show() transmits. A script that forgets show() must light nothing,
     * and the test says so rather than leaving it to be discovered at night. */
    TEST_ASSERT_TRUE(run("pixel.set(1, 255, 128, 64)"));
    TEST_ASSERT_EQUAL_INT(0, sim_lua_pixel_r(0));
    TEST_ASSERT_TRUE(run("pixel.set(1, 255, 128, 64) pixel.show()"));
    TEST_ASSERT_EQUAL_INT(255, sim_lua_pixel_r(0));
    TEST_ASSERT_EQUAL_INT(128, sim_lua_pixel_g(0));
    TEST_ASSERT_EQUAL_INT(64, sim_lua_pixel_b(0));
}

static void test_pixel_index_is_one_based_and_bounded(void) {
    TEST_ASSERT_FALSE(run("pixel.set(0, 1, 1, 1)"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "out of range"));
    TEST_ASSERT_FALSE(run("pixel.set(9999, 1, 1, 1)"));
    TEST_ASSERT_TRUE(run("pixel.set(pixel.count(), 1, 1, 1)"));
}

static void test_pixel_channels_clamped(void) {
    TEST_ASSERT_TRUE(run("pixel.set(1, 999, -50, 300) pixel.show()"));
    TEST_ASSERT_EQUAL_INT(255, sim_lua_pixel_r(0));
    TEST_ASSERT_EQUAL_INT(0, sim_lua_pixel_g(0));
    TEST_ASSERT_EQUAL_INT(255, sim_lua_pixel_b(0));
}

static void test_pixel_fill_and_clear(void) {
    TEST_ASSERT_TRUE(run("pixel.fill(10, 20, 30) pixel.show()"));
    TEST_ASSERT_EQUAL_INT(10, sim_lua_pixel_r(0));
    TEST_ASSERT_EQUAL_INT(30, sim_lua_pixel_b(5));
    TEST_ASSERT_TRUE(run("pixel.clear() pixel.show()"));
    TEST_ASSERT_EQUAL_INT(0, sim_lua_pixel_r(0));
    TEST_ASSERT_EQUAL_INT(0, sim_lua_pixel_b(5));
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void test_init_and_tick_are_called(void) {
    TEST_ASSERT_TRUE(run("n = 0 function init() output.set('beacon', 1) end function tick() n = n + 1 end"));
    TEST_ASSERT_EQUAL_INT(1, sim_lua_output_value(0));
    TEST_ASSERT_TRUE(pyro_lua_tick());
    TEST_ASSERT_TRUE(pyro_lua_tick());
    TEST_ASSERT_TRUE(run("print(n)")); /* fresh chunk: n survives in globals */
}

static void test_event_dispatch(void) {
    TEST_ASSERT_TRUE(run("got = nil function on_event(e) got = e end"));
    TEST_ASSERT_TRUE(pyro_lua_event("APOGEE"));
    TEST_ASSERT_TRUE(run("assert(got == 'APOGEE')"));
}

static void test_script_without_hooks_is_fine(void) {
    TEST_ASSERT_TRUE(run("x = 1"));
    TEST_ASSERT_TRUE(pyro_lua_tick());
    TEST_ASSERT_TRUE(pyro_lua_event("LAUNCH"));
}

/* ── Console ──────────────────────────────────────────────────────── */

static void test_print_goes_to_console(void) {
    TEST_ASSERT_TRUE(run("print('hi', 42)"));
    TEST_ASSERT_EQUAL_STRING("hi\t42\n", sim_lua_console());
}

static void test_eval_error_does_not_kill_the_program(void) {
    TEST_ASSERT_TRUE(run("function tick() output.set('beacon', 3) end"));
    TEST_ASSERT_FALSE(pyro_lua_eval("this is garbage"));
    TEST_ASSERT_TRUE(pyro_lua_tick());
    TEST_ASSERT_EQUAL_INT(3, sim_lua_output_value(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_output_set_and_get);
    RUN_TEST(test_output_list_is_exactly_configured);
    RUN_TEST(test_non_dimmable_rejects_partial);
    RUN_TEST(test_unknown_output_name_is_refused);
    RUN_TEST(test_no_pin_numbers_anywhere);
    RUN_TEST(test_no_gpio_or_sdk_globals);
    RUN_TEST(test_dangerous_base_functions_removed);
    RUN_TEST(test_pyro_is_read_only);
    RUN_TEST(test_pyro_channel_validated);
    RUN_TEST(test_infinite_loop_is_aborted);
    RUN_TEST(test_infinite_loop_inside_pcall_still_aborted);
    RUN_TEST(test_memory_exhaustion_fails_cleanly);
    RUN_TEST(test_vm_usable_after_a_failure);
    RUN_TEST(test_syntax_error_is_reported_not_fatal);
    RUN_TEST(test_flight_state_readable);
    RUN_TEST(test_serial_write_reaches_the_wire);
    RUN_TEST(test_serial_read_returns_nil_when_idle);
    RUN_TEST(test_serial_read_receives);
    RUN_TEST(test_unknown_serial_name_refused);
    RUN_TEST(test_pixel_buffer_needs_show_to_reach_the_wire);
    RUN_TEST(test_pixel_index_is_one_based_and_bounded);
    RUN_TEST(test_pixel_channels_clamped);
    RUN_TEST(test_pixel_fill_and_clear);
    RUN_TEST(test_init_and_tick_are_called);
    RUN_TEST(test_event_dispatch);
    RUN_TEST(test_script_without_hooks_is_fine);
    RUN_TEST(test_print_goes_to_console);
    RUN_TEST(test_eval_error_does_not_kill_the_program);
    return UNITY_END();
}
