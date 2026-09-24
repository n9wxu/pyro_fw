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
#include "lua_check.h"
#include "lua_platform_cfg.h"
#include "pyro_lua.h"
#include "lua_platform.h"
#include "pad_claim.h"
/* For the bytecode test: it compiles a chunk with a throwaway plain state,
 * exactly the way an attacker would produce one. */
#include "lua.h"
#include "lauxlib.h"
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

/* Assert against globals a yielded tick() left behind, without disturbing the
 * coroutine: pyro_lua_eval runs on L, not on the work-unit thread. */
static bool run_eval_ok(const char *src) {
    return pyro_lua_eval(src);
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
    TEST_ASSERT_TRUE(pyro_lua_event("APOGEE", 0));
    TEST_ASSERT_TRUE(run("assert(got == 'APOGEE')"));
}

static void test_script_without_hooks_is_fine(void) {
    TEST_ASSERT_TRUE(run("x = 1"));
    TEST_ASSERT_TRUE(pyro_lua_tick());
    TEST_ASSERT_TRUE(pyro_lua_event("LAUNCH", 0));
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

/* ── Pattern matching is bounded ──────────────────────────────────
 *
 * The matcher is the one reachable C function that is neither preemptible
 * (no VM instructions run inside it, so the count hook never fires) nor
 * bounded by the arena (matching allocates nothing). Backtracking is
 * quadratic, so a few KB of subject is seconds of un-interruptible work. */

static void test_gsub_and_gmatch_are_bounded(void) {
    /* Not restartable, so capped whether or not the caller could yield --
     * they are the same quadratic hazard as find, and leaving them unbounded
     * because they are awkward would be the wrong way round. */
    TEST_ASSERT_FALSE(run("local s = ('a'):rep(500) s:gsub('a', 'b')"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "gsub"));
    TEST_ASSERT_FALSE(run("local s = ('a'):rep(500) for w in s:gmatch('%a') do end"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "gmatch"));
}

static void test_gsub_and_gmatch_work_below_the_cap(void) {
    TEST_ASSERT_TRUE(run("assert(('a,b'):gsub(',', ';') == 'a;b')"));
    TEST_ASSERT_TRUE(run("local n = 0 for w in ('x y'):gmatch('%a') do n = n + 1 end assert(n == 2)"));
}

static void test_pattern_on_a_long_subject_is_refused(void) {
    TEST_ASSERT_FALSE(run("local s = ('a'):rep(3000) string.match(s, '(a-)*$')"));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "exceeds"));
}

static void test_pattern_on_a_short_subject_still_works(void) {
    TEST_ASSERT_TRUE(run("assert(string.match('temp=42', '(%d+)') == '42')"));
    TEST_ASSERT_TRUE(run("assert(('a,b,c'):gsub(',', ';') == 'a;b;c')"));
    TEST_ASSERT_TRUE(run("local n = 0 for w in ('x y z'):gmatch('%a') do n = n + 1 end assert(n == 3)"));
    TEST_ASSERT_TRUE(run("assert(('hello'):find('ell') == 2)"));
}

static void test_plain_find_is_not_capped(void) {
    /* find(s, p, init, true) is a substring search, not pattern
     * interpretation, so length is not a hazard and must not be refused. */
    TEST_ASSERT_TRUE_MESSAGE(run("local s = ('a'):rep(500) .. 'b' assert(s:find('b', 1, true) == 501)"),
                             pyro_lua_last_error());
}

static void test_non_pattern_string_ops_are_untouched(void) {
    /* The cap applies to matching only. These are linear and arena-bounded. */
    TEST_ASSERT_TRUE(run("local s = ('ab'):rep(1000) assert(#s:upper() == 2000)"));
    TEST_ASSERT_TRUE(run("local s = ('ab'):rep(1000) assert(#s:sub(1, 500) == 500)"));
    TEST_ASSERT_TRUE(run("assert(string.format('%d-%s', 7, 'x') == '7-x')"));
}

/* ── Restartable pattern matching ─────────────────────────────────
 *
 * Above the direct-call threshold the search loops over start positions in C
 * and suspends between them via lua_yieldk. These check that it produces the
 * same answers as the builtin, and that a long search actually yields and
 * resumes rather than running to completion in one grant. */

static void test_long_pattern_gives_the_same_answer(void) {
    /* 400-byte subject, well past the direct-call threshold. Run inside a
     * work unit, which is where the restartable path is reachable. */
    TEST_ASSERT_TRUE_MESSAGE(run("res = nil\n"
                         "function tick()\n"
                         "  local s = ('a'):rep(400) .. 'needle' .. ('b'):rep(50)\n"
                         "  res = { s:find('needle') }\n"
                         "end\n"),
                             pyro_lua_last_error());
    pyro_lua_status_t st;
    int guard = 0;
    do {
        st = pyro_lua_tick_slice(2000);
    } while (st == PYRO_LUA_YIELD && ++guard < 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(PYRO_LUA_DONE, st, pyro_lua_last_error());
    TEST_ASSERT_TRUE_MESSAGE(pyro_lua_eval("assert(res[1] == 401, 'start '..tostring(res[1]))"),
                             pyro_lua_last_error());
    TEST_ASSERT_TRUE(pyro_lua_eval("assert(res[2] == 406, 'end '..tostring(res[2]))"));
}

static void test_long_pattern_with_captures(void) {
    TEST_ASSERT_TRUE(run("a, b = nil, nil\n"
                         "function tick()\n"
                         "  local s = ('x'):rep(300) .. 'temp=42C'\n"
                         "  a, b = s:match('(%a+)=(%d+)')\n"
                         "end\n"));
    pyro_lua_status_t st;
    int guard = 0;
    do {
        st = pyro_lua_tick_slice(2000);
    } while (st == PYRO_LUA_YIELD && ++guard < 1000);
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, st);
    TEST_ASSERT_TRUE(pyro_lua_eval("assert(b == '42', tostring(b))"));
}

static void test_a_pathological_pattern_yields_instead_of_blocking(void) {
    /* Quadratic backtracking over a subject that fits the arena. Nothing in
     * the VM can interrupt the matcher, so this must suspend. */
    TEST_ASSERT_TRUE(run("function tick()\n"
                         "  local s = ('a'):rep(1200)\n"
                         "  s:match('(a-)*$')\n"
                         "end\n"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(PYRO_LUA_YIELD, pyro_lua_tick_slice(1000),
                                  "a quadratic match must suspend, not run to completion");
    /* And it finishes if given enough grants, rather than looping forever. */
    pyro_lua_status_t st;
    int grants = 0;
    do {
        st = pyro_lua_tick_slice(2000);
    } while (st == PYRO_LUA_YIELD && ++grants < 5000);
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, st);
}

/* A suspended search must survive anything that runs while it is suspended.
 *
 * The restartable path must keep its anchored pattern in the activation. An
 * upvalue of the string.find closure is one object shared by every caller,
 * however the call stack is arranged, and core1 runs on_event() before it
 * resumes tick(): a handler matching on a 65..128 byte subject would
 * overwrite the pattern the suspended search is using. The search then
 * finishes against the wrong pattern and reports a match, with no error
 * anywhere. */
static void test_a_suspended_search_survives_an_event_handler(void) {
    TEST_ASSERT_TRUE_MESSAGE(run("hit = nil\n"
                                 "S = ('a'):rep(2000) .. 'zz'\n"
                                 "function tick() hit = S:find('zz') end\n"
                                 "function on_event(e)\n"
                                 "  local t = ('a'):rep(100)\n" /* over the direct-call threshold */
                                 "  t:find('aa')\n"
                                 "end\n"),
                             pyro_lua_last_error());

    /* A one-microsecond grant: the search cannot finish, so it suspends
     * somewhere in the middle of its 2000 start positions. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(PYRO_LUA_YIELD, pyro_lua_tick_slice(1),
                                  "the search must suspend for this test to mean anything");

    pyro_lua_event("apogee", 0);

    pyro_lua_status_t st;
    int guard = 0;
    do {
        st = pyro_lua_tick_slice(2000);
    } while (st == PYRO_LUA_YIELD && ++guard < 10000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(PYRO_LUA_DONE, st, pyro_lua_last_error());
    TEST_ASSERT_TRUE_MESSAGE(pyro_lua_eval("assert(hit == 2001, 'found at '..tostring(hit))"),
                             pyro_lua_last_error());
}

static void test_no_match_on_a_long_subject_returns_nil(void) {
    TEST_ASSERT_TRUE(run("got = 'unset'\n"
                         "function tick() got = ('a'):rep(500):find('zzz') end\n"));
    pyro_lua_status_t st;
    int guard = 0;
    do {
        st = pyro_lua_tick_slice(2000);
    } while (st == PYRO_LUA_YIELD && ++guard < 1000);
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, st);
    TEST_ASSERT_TRUE(pyro_lua_eval("assert(got == nil, tostring(got))"));
}

/* ── Time-boxed work units ────────────────────────────────────────
 *
 * The dispatch model: core0 grants core1 a slice of the loop period, the VM
 * runs until the box expires, yields, and RESUMES there on the next grant.
 * A script that never returns is a legitimate program under this model, not
 * a runaway -- which is the behaviour these pin down. */

static void test_tick_yields_when_the_time_box_expires(void) {
    TEST_ASSERT_TRUE(run("n = 0 function tick() while true do n = n + 1 end end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_YIELD, pyro_lua_tick_slice(2000));
}

static void test_a_yielded_tick_resumes_rather_than_restarts(void) {
    /* Yielding keeps the work already done. A script that restarted each
     * grant would never climb n past one slice. */
    TEST_ASSERT_TRUE(run("n = 0 function tick() while true do n = n + 1 end end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_YIELD, pyro_lua_tick_slice(2000));
    TEST_ASSERT_TRUE(run_eval_ok("first = n"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_YIELD, pyro_lua_tick_slice(2000));
    TEST_ASSERT_TRUE(run_eval_ok("assert(n > first, 'tick restarted instead of resuming')"));
}

static void test_a_short_tick_completes_within_its_box(void) {
    TEST_ASSERT_TRUE(run("done = 0 function tick() done = done + 1 end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, pyro_lua_tick_slice(5000));
    TEST_ASSERT_TRUE(run_eval_ok("assert(done == 1)"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, pyro_lua_tick_slice(5000));
    TEST_ASSERT_TRUE(run_eval_ok("assert(done == 2)"));
}

static void test_the_box_is_actually_honoured(void) {
    /* Generous bounds: a host machine is not a flight computer. The claim is
     * that the grant bounds the call at all, not that it is precise. */
    TEST_ASSERT_TRUE(run("function tick() while true do end end"));
    uint32_t t0 = lua_plat_now_us();
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_YIELD, pyro_lua_tick_slice(5000));
    uint32_t took = lua_plat_now_us() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took < 200000u, "a 5 ms grant ran for over 200 ms");
}

static void test_an_error_in_tick_is_reported_and_recoverable(void) {
    /* A coroutine that raised is dead and cannot be resumed. If the host did
     * not build a fresh one, a single fault would break tick() forever. */
    TEST_ASSERT_TRUE(run("function tick() error('boom') end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_ERROR, pyro_lua_tick_slice(5000));
    TEST_ASSERT_NOT_NULL(strstr(pyro_lua_last_error(), "boom"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_ERROR, pyro_lua_tick_slice(5000));
    TEST_ASSERT_TRUE(run("ok = 0 function tick() ok = 1 end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, pyro_lua_tick_slice(5000));
    TEST_ASSERT_TRUE(run_eval_ok("assert(ok == 1)"));
}

static void test_tick_defined_inside_init_is_found(void) {
    /* Do not cache the tick() lookup from before init() runs, or a tick()
     * that init() defined never gets called. */
    TEST_ASSERT_TRUE(run("ran = 0 function init() function tick() ran = ran + 1 end end"));
    TEST_ASSERT_EQUAL_INT(PYRO_LUA_DONE, pyro_lua_tick_slice(5000));
    TEST_ASSERT_TRUE(run_eval_ok("assert(ran == 1, 'tick defined in init was not called')"));
}

/* ── Bytecode is refused ──────────────────────────────────────────
 *
 * luaL_loadbuffer()'s NULL mode means "bt" -- text OR precompiled bytecode --
 * and Lua 5.4 does not verify bytecode: lundump.c checks a header and trusts
 * the rest. A crafted blob would run with arbitrary load/store over the whole
 * address space, which on this board includes the pyro GPIO registers and
 * core0's stack. Every other sandbox test in this file is a property of the
 * BINDINGS, and bytecode never reaches them, so this one guards all of them.
 *
 * The chunk is compiled by a throwaway plain state, exactly the way an
 * attacker would produce one -- no fixture, no hand-written header. */
typedef struct {
    char buf[4096];
    size_t len;
} dump_sink_t;

/* Plain C sink, deliberately not luaL_Buffer: that pushes onto the Lua stack
 * and moves the function off the top, where lua_dump() expects to find it. */
static int dump_writer(lua_State *Ls, const void *p, size_t sz, void *ud) {
    (void)Ls;
    dump_sink_t *d = (dump_sink_t *)ud;
    if (d->len + sz > sizeof(d->buf))
        return 1;
    memcpy(d->buf + d->len, p, sz);
    d->len += sz;
    return 0;
}

static void test_precompiled_bytecode_is_refused(void) {
    lua_State *T = luaL_newstate();
    TEST_ASSERT_NOT_NULL(T);
    const char *src = "beacon_fired = true";
    TEST_ASSERT_EQUAL_INT(LUA_OK, luaL_loadstring(T, src));

    static dump_sink_t d;
    d.len = 0;
    TEST_ASSERT_EQUAL_INT(0, lua_dump(T, dump_writer, &d, 0));
    lua_close(T);

    TEST_ASSERT_TRUE_MESSAGE(d.len > 0, "nothing dumped");
    char *copy = d.buf;
    size_t blen = d.len;

    /* LUA_SIGNATURE is "\x1bLua"; a text chunk can never start with it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x1b, (unsigned char)copy[0], "not a binary chunk");

    TEST_ASSERT_FALSE_MESSAGE(pyro_lua_load("=bc", copy, blen), "binary chunk must be refused");
    TEST_ASSERT_FALSE_MESSAGE(pyro_lua_eval(copy), "binary chunk must be refused by eval too");

    /* And the VM is still usable afterwards -- a refusal, not a wedge. */
    TEST_ASSERT_TRUE(run("output.set('beacon', 1)"));
}

static void test_text_chunks_still_load(void) {
    TEST_ASSERT_TRUE(run("x = 1 + 1 assert(x == 2)"));
}

/* ── Telemetry data and the log outlet ────────────────────────────
 *
 * A script is meant to be able to format the same telemetry the built-in
 * formatter does. These pin down that it can see everything that sentence
 * carries, not a subset -- the raw ADC counts especially, since the booleans
 * round a degraded connector to "good". */

void sim_lua_set_pyro_adc(int channel, int counts);
void sim_lua_set_thrust(int under_thrust, int apogee_detected);
const char *sim_lua_log(void);
void sim_lua_log_clear(void);
uint32_t sim_lua_log_dropped(void);

static void test_flight_exposes_thrust_and_apogee(void) {
    sim_lua_set_thrust(1, 0);
    TEST_ASSERT_TRUE(run("assert(flight.under_thrust() == true)"));
    TEST_ASSERT_TRUE(run("assert(flight.apogee() == false)"));
    sim_lua_set_thrust(0, 1);
    TEST_ASSERT_TRUE(run("assert(flight.under_thrust() == false)"));
    TEST_ASSERT_TRUE(run("assert(flight.apogee() == true)"));
}

static void test_pyro_status_carries_the_raw_count(void) {
    /* The number is the point: a dirty connector sits between the thresholds
     * and only the count shows it, which is why telemetry carries it. */
    sim_lua_set_pyro_adc(1, 1873);
    sim_lua_set_pyro_adc(2, 42);
    TEST_ASSERT_TRUE(run("assert(pyro.status(1).adc == 1873)"));
    TEST_ASSERT_TRUE(run("assert(pyro.status(2).adc == 42)"));
}

static void test_pyro_is_still_read_only_with_adc_added(void) {
    /* Invariant L11 -- adding a field must not have added a setter. */
    TEST_ASSERT_TRUE(run("assert(pyro.fire == nil)"));
    TEST_ASSERT_TRUE(run("assert(pyro.arm == nil)"));
    TEST_ASSERT_TRUE(run("assert(pyro.set == nil)"));
    TEST_ASSERT_TRUE(run("assert(pyro.write == nil)"));
}

static void test_log_write_and_line(void) {
    sim_lua_log_clear();
    TEST_ASSERT_TRUE(run("log.write('abc')"));
    TEST_ASSERT_EQUAL_STRING("abc", sim_lua_log());
    sim_lua_log_clear();
    TEST_ASSERT_TRUE(run("log.line('hello')"));
    TEST_ASSERT_EQUAL_STRING("hello\n", sim_lua_log());
}

static void test_log_never_blocks_when_flooded(void) {
    /* A script that outruns the consumer must lose output, not stall: on the
     * target this ring is the only thing between core1 and a core0 it must
     * never wait for. */
    sim_lua_log_clear();
    uint32_t before = sim_lua_log_dropped();
    TEST_ASSERT_TRUE(run("for i = 1, 400 do log.line('flooding the ring') end"));
    TEST_ASSERT_TRUE_MESSAGE(sim_lua_log_dropped() > before, "flood should have dropped, not blocked");
    TEST_ASSERT_TRUE(run("log.line('still alive')"));
}

static void test_script_cannot_reach_the_filesystem(void) {
    /* log.* is the ONLY route to a file, and it is one-way. A script that
     * could open or read one would be reaching past core0, which owns flash. */
    TEST_ASSERT_TRUE(run("assert(log.read == nil)"));
    TEST_ASSERT_TRUE(run("assert(log.open == nil)"));
    TEST_ASSERT_TRUE(run("assert(io == nil)"));
    TEST_ASSERT_TRUE(run("assert(os == nil)"));
}

/* ── Static config check (src/lua/lua_check.c) ───────────────────
 *
 * The checker answers "are this script and this configuration a matched
 * pair?" before flight. It is not a security boundary -- the sandbox is --
 * so it is allowed to over-approximate, and these tests pin down which
 * direction it errs in. */

static lua_chk_result_t chk;

static bool chk_has(lua_chk_kind_t kind) {
    for (int i = 0; i < chk.count; i++)
        if (chk.items[i].kind == kind)
            return true;
    return false;
}

static void test_check_accepts_a_matching_script(void) {
    const char *src = "function tick()\n"
                      "  output.set('beacon', 50)\n"
                      "  if input.get('sense') then serial.write('radio','hi') end\n"
                      "end\n";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_TRUE(chk.green);
    TEST_ASSERT_FALSE(chk_has(LUA_CHK_MISSING));
    TEST_ASSERT_FALSE(chk_has(LUA_CHK_UNKNOWN));
}

static void test_check_reports_syntax_without_running(void) {
    const char *src = "function tick( end";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_FALSE(chk.green);
    TEST_ASSERT_TRUE(chk_has(LUA_CHK_SYNTAX));
}

static void test_check_flags_a_typo_as_unknown(void) {
    /* 'beacn' is identifier-shaped and names nothing. A warning, not a red:
     * the shape test is a heuristic. */
    const char *src = "function tick() output.set('beacn', 1) end";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_TRUE(chk_has(LUA_CHK_UNKNOWN));
}

static void test_check_does_not_flag_message_text(void) {
    /* A sentence sent over the radio must not be mistaken for a resource
     * name, or every script that reports anything would go yellow. */
    const char *src = "function tick() serial.write('radio', 'apogee reached ok') end";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_FALSE(chk_has(LUA_CHK_UNKNOWN));
}

static void test_check_finds_nested_function_constants(void) {
    /* The walk must recurse through Proto->p, or a name used inside a nested
     * function is invisible and the check silently passes everything.
     * 'beacom' is one edit from 'beacon', so it is exactly the near-miss the
     * checker reports -- and it only reports it if the recursion works. */
    const char *src = "function tick()\n"
                      "  local function inner() output.set('beacom', 1) end\n"
                      "  inner()\n"
                      "end\n";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_TRUE(chk_has(LUA_CHK_UNKNOWN));
}

static void test_check_catches_the_unassigned_resource(void) {
    /* The gap this exists to find: the script uses the LED string, but no pin
     * was assigned one. This is a red, not a warning. */
    /* Each entry carries its own pin now, so the array is not positional. */
    const lua_pin_cfg_t pins[4] = {
        {18, LUA_ROLE_PWM, "beacon"},
        {19, LUA_ROLE_IN, "sense"},
        {20, LUA_ROLE_TX, "radio"},
        {21, LUA_ROLE_OFF, ""},
    };
    lua_plat_configure(pins, 4, 9600, 0); /* no LED string */

    const char *src = "function tick() pixel.set(1,255,0,0) pixel.show() end";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_FALSE(chk.green);
    TEST_ASSERT_TRUE(chk_has(LUA_CHK_MISSING));

    lua_plat_configure(NULL, 0, 9600, 16); /* restore the demo set */
}

/* Drive a published output the way a binding does, through its vtable. */
static void set_output(const char *name, int value) {
    const lua_resource_t *r = lua_iface_at(lua_iface_find(name, LUA_IF_OUTPUT));
    TEST_ASSERT_NOT_NULL(r);
    ((const lua_if_output_t *)r->vt)->set(r->ctx, value);
}

static void test_check_does_not_execute_the_script(void) {
    /* Validation must be static. If the checker ran this, the output would
     * move and the UART would receive. */
    sim_lua_uart_tx_clear();
    set_output("beacon", 0);
    const char *src = "output.set('beacon', 99) serial.write('radio','x')\n"
                      "function tick() end\n";
    do {
        lua_chk_env_t env;
        lua_chk_env_from_platform(&env);
        lua_check(src, strlen(src), &env, &chk);
    } while (0);
    TEST_ASSERT_EQUAL_INT(0, sim_lua_output_value(0));
    TEST_ASSERT_EQUAL_STRING("", sim_lua_uart_tx());
}

/* ── The exclusion, from the interface table's side ───────────────
 *
 * lua_iface_publish() spends a claim, so a resource on a pad the flight
 * software kept cannot be created. This is the half that keeps a script from
 * reaching a live pyro pad. */

static const lua_if_output_t probe_vt = {NULL, NULL, false};

/* These three drive the table directly, so each starts from an empty one and
 * puts the demo set back -- the other tests resolve names against it. */
static void iface_scratch(void) {
    lua_iface_reset();
    pad_claim_reset();
}
static void iface_restore(void) {
    pad_claim_reset();
    lua_plat_configure(NULL, 0, 9600, 16);
}

static void test_publish_refused_on_a_pad_the_flight_software_holds(void) {
    iface_scratch();
    TEST_ASSERT_TRUE(pad_claim_take(PAD(9), PAD_FLIGHT));

    TEST_ASSERT_LESS_THAN_MESSAGE(0, lua_iface_publish(PAD(9), "winch", LUA_IF_OUTPUT, &probe_vt, NULL),
                                  "a pad the flight software holds must not become a Lua resource");
    TEST_ASSERT_EQUAL_MESSAGE(-1, lua_iface_find("winch", LUA_IF_OUTPUT), "and no entry may exist for it");
    iface_restore();
}

static void test_publish_of_a_pair_is_all_or_nothing(void) {
    iface_scratch();
    TEST_ASSERT_TRUE(pad_claim_take(PAD(10), PAD_FLIGHT));

    /* A bridge across a free pad and a held one. Publishing the half that is
       available would leave a resource driving one gate it owns and one it
       does not. */
    TEST_ASSERT_LESS_THAN(0, lua_iface_publish(PAD(9) | PAD(10), "motor", LUA_IF_OUTPUT, &probe_vt, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(PAD_FREE, pad_claim_owner(9), "the free half must not have been claimed");
    iface_restore();
}

static void test_publish_claims_the_pad_it_takes(void) {
    iface_scratch();
    TEST_ASSERT_GREATER_OR_EQUAL(0, lua_iface_publish(PAD(9), "winch", LUA_IF_OUTPUT, &probe_vt, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(PAD_LUA, pad_claim_owner(9), "publishing is what claims it");
    TEST_ASSERT_FALSE_MESSAGE(pad_claim_take(PAD(9), PAD_FLIGHT), "so the flight software can no longer have it");
    iface_restore();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_publish_refused_on_a_pad_the_flight_software_holds);
    RUN_TEST(test_publish_of_a_pair_is_all_or_nothing);
    RUN_TEST(test_publish_claims_the_pad_it_takes);
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

    RUN_TEST(test_long_pattern_gives_the_same_answer);
    RUN_TEST(test_long_pattern_with_captures);
    RUN_TEST(test_a_pathological_pattern_yields_instead_of_blocking);
    RUN_TEST(test_a_suspended_search_survives_an_event_handler);
    RUN_TEST(test_no_match_on_a_long_subject_returns_nil);
    RUN_TEST(test_gsub_and_gmatch_are_bounded);
    RUN_TEST(test_gsub_and_gmatch_work_below_the_cap);
    RUN_TEST(test_pattern_on_a_long_subject_is_refused);
    RUN_TEST(test_pattern_on_a_short_subject_still_works);
    RUN_TEST(test_plain_find_is_not_capped);
    RUN_TEST(test_non_pattern_string_ops_are_untouched);
    RUN_TEST(test_tick_yields_when_the_time_box_expires);
    RUN_TEST(test_a_yielded_tick_resumes_rather_than_restarts);
    RUN_TEST(test_a_short_tick_completes_within_its_box);
    RUN_TEST(test_the_box_is_actually_honoured);
    RUN_TEST(test_an_error_in_tick_is_reported_and_recoverable);
    RUN_TEST(test_tick_defined_inside_init_is_found);
    RUN_TEST(test_precompiled_bytecode_is_refused);
    RUN_TEST(test_text_chunks_still_load);
    RUN_TEST(test_flight_exposes_thrust_and_apogee);
    RUN_TEST(test_pyro_status_carries_the_raw_count);
    RUN_TEST(test_pyro_is_still_read_only_with_adc_added);
    RUN_TEST(test_log_write_and_line);
    RUN_TEST(test_log_never_blocks_when_flooded);
    RUN_TEST(test_script_cannot_reach_the_filesystem);

    RUN_TEST(test_check_accepts_a_matching_script);
    RUN_TEST(test_check_reports_syntax_without_running);
    RUN_TEST(test_check_flags_a_typo_as_unknown);
    RUN_TEST(test_check_does_not_flag_message_text);
    RUN_TEST(test_check_finds_nested_function_constants);
    RUN_TEST(test_check_catches_the_unassigned_resource);
    RUN_TEST(test_check_does_not_execute_the_script);
    return UNITY_END();
}
