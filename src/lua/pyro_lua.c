/*
 * Lua VM host for user programs.
 *
 * The bindings are the security boundary: RP2040 has no MPU, so no hardware
 * enforces anything below this line. Read them as a syscall table.
 *
 * [LUA-SAFE-01] Every load passes mode "t" (Lua 5.4 manual §6.1, load). luaL_loadbuffer()
 * passes NULL, meaning "bt", and Lua does not verify bytecode -- lundump.c
 * checks a header and trusts the rest -- so a crafted blob POSTed to
 * /api/lua/script would execute arbitrary loads and stores over the whole
 * address space. Every invariant below is a property of the bindings, which
 * bytecode never reaches.
 *
 * [LUA-SAFE-02] Every entry from the host into the VM runs under lua_pcall, so nothing a
 * script does can raise outside protection (§4.4).
 *
 * Implements invariants L4, L5, L7, L8 and L11 of
 * thoughts/shared/plans/2026-09-21-lua-user-programs-core1.md
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro_lua.h"
#include "flight_states.h"
#include "lua_arena.h"
#include "lua_platform.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(lua_Number) == 4 && sizeof(lua_Integer) == 4,
               "Lua is not the 32-bit build: src/lua/lua_patch.cmake was not applied");

/* 32 kB on the 32-bit target. A 64-bit host build -- the native simulator and
 * the tests -- gets half as much again, because its pointers and block
 * headers are twice the size, so a script fits on the host when it fits on
 * the board. */
#ifndef PYRO_LUA_ARENA_BYTES
#define PYRO_LUA_ARENA_BYTES (32 * 1024 * (sizeof(void *) == 8 ? 3 : 2) / 2)
#endif

/* VM instructions, or matcher steps, between two looks at the limits. */
#ifndef PYRO_LUA_HOOK_COUNT
#define PYRO_LUA_HOOK_COUNT 1000
#endif

/* [LUA-SAFE-03] Hook periods one protected call may use: about two million instructions. */
#ifndef PYRO_LUA_BUDGET
#define PYRO_LUA_BUDGET 2000
#endif

/* [LUA-SAFE-04] How far past its box a tick() may run where it cannot be suspended -- a
 * sort comparator, a metamethod, one pattern match. Longer is a runaway. */
#ifndef PYRO_LUA_OVERRUN_US
#define PYRO_LUA_OVERRUN_US 5000u
#endif

/* [LUA-SAFE-09] lua_arena.c aligns every block relative to this base. */
static uint8_t arena_buf[PYRO_LUA_ARENA_BYTES] __attribute__((aligned(8)));
static lua_arena_t arena;
static lua_State *L;
static char last_error[160];

/* tick() runs in a coroutine, because lua_pcall cannot yield and yielding is
 * how a time box suspends a script mid-loop for the next grant to resume.
 * Anchored at index 1 of L's stack, against the collector. */
static lua_State *co;
static bool slice_running; /* a tick() is suspended, awaiting its next grant */
static uint32_t slice_deadline_us;
static bool slice_boxed; /* false for a grant of 0: run to completion */

/* Load, init(), on_event() and the console eval run under lua_pcall, which
 * cannot yield, so they are bounded by raising: an instruction budget, and
 * for an event also a deadline. */
static uint32_t budget_left;
static uint32_t hard_deadline_us;
static bool hard_boxed;

/* ── Limits ───────────────────────────────────────────────────────── */

/* [LUA-SAFE-05] Raised as light userdata, which no script can construct, and remembered:
 * pcall and xpcall re-raise while one is set, so a script cannot catch the
 * error that stops it (see l_pcall). */
static const char BUDGET_EXHAUSTED[] = "instruction budget exhausted";
static const char TIME_BOX_EXHAUSTED[] = "time box exhausted";
static const char *limit_hit;

static int raise_limit(lua_State *Ls, const char *which) {
    limit_hit = which;
    lua_pushlightuserdata(Ls, (void *)which);
    return lua_error(Ls);
}

static bool past(uint32_t deadline_us) {
    return (int32_t)(lua_plat_now_us() - deadline_us) >= 0;
}

static void count_hook(lua_State *Ls, lua_Debug *ar);
static bool hook_hastened;

/* One hook period of work is done; decide whether the script may go on.
 *
 * Inside the pattern matcher nothing can yield, so a slice that expires
 * there makes the hook fire on the next VM instruction instead, which is the
 * first point that can. Elsewhere a slice that cannot yield is let run
 * PYRO_LUA_OVERRUN_US past its box and is then raised. */
static void charge(lua_State *Ls, bool in_matcher) {
    if (slice_boxed) {
        if (!past(slice_deadline_us)) {
            return;
        }
        if (!in_matcher && lua_isyieldable(Ls)) {
            lua_yield(Ls, 0); /* from a hook this returns; Lua yields after it (§4.7) */
            return;
        }
        if (past(slice_deadline_us + PYRO_LUA_OVERRUN_US)) {
            raise_limit(Ls, TIME_BOX_EXHAUSTED);
        }
        if (in_matcher && !hook_hastened) {
            hook_hastened = true;
            lua_sethook(Ls, count_hook, LUA_MASKCOUNT, 1);
        }
        return;
    }
    if (budget_left == 0) {
        raise_limit(Ls, BUDGET_EXHAUSTED);
    }
    budget_left--;
    if (hard_boxed && past(hard_deadline_us)) {
        raise_limit(Ls, TIME_BOX_EXHAUSTED);
    }
}

/* A count hook, because Lua lets only count and line hooks yield (§4.7). */
static void count_hook(lua_State *Ls, lua_Debug *ar) {
    (void)ar;
    if (hook_hastened) {
        hook_hastened = false;
        lua_sethook(Ls, count_hook, LUA_MASKCOUNT, PYRO_LUA_HOOK_COUNT);
    }
    charge(Ls, false);
}

/* The matcher runs no VM instructions, so the count hook never sees it, and
 * its cost grows with the pattern as well as the subject: each optional item
 * doubles it. lua_patch.cmake calls this from lstrlib.c's match(), and a hook
 * period of matcher steps is charged like one of instructions. */
static uint32_t match_steps;

void pyro_lua_match_step(lua_State *Ls) {
    if (++match_steps < PYRO_LUA_HOOK_COUNT) {
        return;
    }
    match_steps = 0;
    charge(Ls, true);
}

static void limits_start(uint32_t budget_us, bool boxed_slice) {
    limit_hit = NULL;
    match_steps = 0;
    slice_boxed = boxed_slice && budget_us != 0u;
    slice_deadline_us = lua_plat_now_us() + budget_us;
    hard_boxed = !boxed_slice && budget_us != 0u;
    hard_deadline_us = slice_deadline_us;
    budget_left = PYRO_LUA_BUDGET;
}

/* Never converts the error object in place: lua_tostring on a number would
 * allocate, outside protection. */
static void record_error(lua_State *Ls) {
    if (limit_hit) {
        snprintf(last_error, sizeof(last_error), "%s", limit_hit);
    } else if (lua_type(Ls, -1) == LUA_TSTRING) {
        snprintf(last_error, sizeof(last_error), "%s", lua_tostring(Ls, -1));
    } else if (lua_isinteger(Ls, -1)) {
        snprintf(last_error, sizeof(last_error), "%ld", (long)lua_tointeger(Ls, -1));
    } else if (lua_type(Ls, -1) == LUA_TNUMBER) {
        snprintf(last_error, sizeof(last_error), "%.7g", (double)lua_tonumber(Ls, -1));
    } else {
        snprintf(last_error, sizeof(last_error), "(error object is a %s value)", luaL_typename(Ls, -1));
    }
    lua_pop(Ls, 1);
}

/* ── pcall and xpcall that cannot outlast a limit ─────────────────
 *
 * lbaselib.c's luaB_pcall and luaB_xpcall, plus one check: once a limit has
 * been raised the call re-raises it, whatever the error object became. */

static int finish_pcall(lua_State *Ls, int status, lua_KContext extra) {
    if (limit_hit) {
        lua_pushlightuserdata(Ls, (void *)limit_hit);
        return lua_error(Ls);
    }
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(Ls, 0);
        lua_pushvalue(Ls, -2);
        return 2;
    }
    return lua_gettop(Ls) - (int)extra;
}

static int l_pcall(lua_State *Ls) {
    luaL_checkany(Ls, 1);
    lua_pushboolean(Ls, 1);
    lua_insert(Ls, 1);
    int status = lua_pcallk(Ls, lua_gettop(Ls) - 2, LUA_MULTRET, 0, 0, finish_pcall);
    return finish_pcall(Ls, status, 0);
}

static int l_xpcall(lua_State *Ls) {
    int n = lua_gettop(Ls);
    luaL_checktype(Ls, 2, LUA_TFUNCTION);
    lua_pushboolean(Ls, 1);
    lua_pushvalue(Ls, 1);
    lua_rotate(Ls, 3, 2);
    int status = lua_pcallk(Ls, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
    return finish_pcall(Ls, status, 2);
}

/* [LUA-SAFE-07] Finalisers run with hooks off, so a __gc would escape every limit. Lua
 * marks an object for finalisation only if its metatable has __gc when
 * setmetatable is called (§2.5.3), so refusing it here is sufficient. */
static int l_setmetatable(lua_State *Ls) {
    if (lua_type(Ls, 2) == LUA_TTABLE) {
        lua_pushliteral(Ls, "__gc");
        if (lua_rawget(Ls, 2) != LUA_TNIL) {
            return luaL_error(Ls, "__gc metamethods are not allowed (finalisers run outside the limits)");
        }
        lua_pop(Ls, 1);
    }
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(1));
    return orig(Ls);
}

/* ── Panic ────────────────────────────────────────────────────────
 *
 * Unreachable while every host call is protected. If it is reached anyway,
 * Lua calls abort() when this returns (§4.4), which on the target is a
 * HardFault that takes core1's other tasks with it. So it jumps back to the
 * entry point instead, which abandons the VM: its state is unknown, so it is
 * not closed, and the next pyro_lua_init() reclaims the arena. */
static jmp_buf *panic_exit;

static int on_panic(lua_State *Ls) {
    const char *m = lua_type(Ls, -1) == LUA_TSTRING ? lua_tostring(Ls, -1) : "?";
    snprintf(last_error, sizeof(last_error), "panic: %s", m);
    if (panic_exit) {
        longjmp(*panic_exit, 1);
    }
    return 0;
}

static void abandon_vm(void) {
    panic_exit = NULL;
    L = NULL;
    co = NULL;
    slice_running = false;
}

/* ── print() redirected to the platform console ───────────────────── */

static int l_print(lua_State *Ls) {
    int n = lua_gettop(Ls);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(Ls, i, &len);
        if (i > 1)
            lua_plat_console_out("\t", 1);
        lua_plat_console_out(s, (int)len);
        lua_pop(Ls, 1);
    }
    lua_plat_console_out("\n", 1);
    return 0;
}

/* ── Name lookup ──────────────────────────────────────────────────── */

/* Invariant L5: a script can say only a name, and only a name configuration
 * granted resolves. The kind is part of the lookup; see lua_iface.h. */
static const lua_resource_t *need(lua_State *Ls, lua_iface_kind_t kind, const char *what, const char *name) {
    int idx = lua_iface_find(name, kind);
    if (idx < 0) {
        luaL_error(Ls, "no %s named '%s'", what, name);
    }
    return lua_iface_at(idx);
}

static int push_names(lua_State *Ls, lua_iface_kind_t kind) {
    int n = lua_iface_count_kind(kind);
    lua_createtable(Ls, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushstring(Ls, lua_iface_nth_of_kind(kind, i)->name);
        lua_rawseti(Ls, -2, i + 1);
    }
    return 1;
}

/* ── output.* ─────────────────────────────────────────────────────── */

static int l_output_set(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    const lua_resource_t *r = need(Ls, LUA_IF_OUTPUT, "output", name);
    const lua_if_output_t *vt = r->vt;

    int v;
    if (lua_isboolean(Ls, 2)) {
        v = lua_toboolean(Ls, 2) ? 100 : 0;
    } else {
        v = (int)luaL_checkinteger(Ls, 2);
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        if (v != 0 && v != 100 && !vt->dimmable)
            return luaL_error(Ls, "output '%s' is not dimmable", name);
    }
    vt->set(r->ctx, v);
    return 0;
}

static int l_output_get(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    const lua_resource_t *r = need(Ls, LUA_IF_OUTPUT, "output", name);
    lua_pushinteger(Ls, ((const lua_if_output_t *)r->vt)->get(r->ctx));
    return 1;
}

static int l_output_list(lua_State *Ls) {
    return push_names(Ls, LUA_IF_OUTPUT);
}

/* ── input.* ──────────────────────────────────────────────────────── */

static int l_input_get(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    const lua_resource_t *r = need(Ls, LUA_IF_INPUT, "input", name);
    lua_pushboolean(Ls, ((const lua_if_input_t *)r->vt)->get(r->ctx));
    return 1;
}

static int l_input_list(lua_State *Ls) {
    return push_names(Ls, LUA_IF_INPUT);
}

/* ── serial.* ─────────────────────────────────────────────────────── */

#define SERIAL_READ_DEFAULT 64
#define SERIAL_READ_MAX 256

static int l_serial_write(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    size_t len;
    const char *s = luaL_checklstring(Ls, 2, &len);
    const lua_resource_t *r = need(Ls, LUA_IF_SERIAL, "serial", name);
    lua_pushinteger(Ls, ((const lua_if_serial_t *)r->vt)->write(r->ctx, s, (int)len));
    return 1;
}

static int l_serial_read(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    lua_Integer max = luaL_optinteger(Ls, 2, SERIAL_READ_DEFAULT);
    const lua_resource_t *r = need(Ls, LUA_IF_SERIAL, "serial", name);
    if (max < 1)
        max = 1;
    if (max > SERIAL_READ_MAX)
        max = SERIAL_READ_MAX;
    char buf[SERIAL_READ_MAX];
    int n = ((const lua_if_serial_t *)r->vt)->read(r->ctx, buf, (int)max);
    if (n <= 0) {
        lua_pushnil(Ls);
        return 1;
    }
    lua_pushlstring(Ls, buf, (size_t)n);
    return 1;
}

/* ── pixel.*  (addressable LED string) ────────────────────────────── */

/* A script says pixel.set(3, ...) and never names the string, so the first
 * one published is the string; a board publishing two would make the second
 * unreachable, which is why none does. */
static const lua_resource_t *the_string(void) {
    return lua_iface_nth_of_kind(LUA_IF_PIXEL, 0);
}

static int px_len(void) {
    const lua_resource_t *r = the_string();
    return r ? ((const lua_if_pixel_t *)r->vt)->count(r->ctx) : 0;
}

static int l_pixel_count(lua_State *Ls) {
    lua_pushinteger(Ls, px_len());
    return 1;
}

static int check_channel(lua_State *Ls, int arg) {
    lua_Integer v = luaL_checkinteger(Ls, arg);
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (int)v;
}

static int l_pixel_set(lua_State *Ls) {
    lua_Integer i = luaL_checkinteger(Ls, 1); /* 1-based, as Lua tables are */
    int n = px_len();
    if (i < 1 || i > n)
        return luaL_error(Ls, "pixel %d out of range (1..%d)", (int)i, n);
    const lua_resource_t *r = the_string();
    ((const lua_if_pixel_t *)r->vt)
        ->set(r->ctx, (int)i - 1, (uint8_t)check_channel(Ls, 2), (uint8_t)check_channel(Ls, 3),
              (uint8_t)check_channel(Ls, 4));
    return 0;
}

static void fill(int r_, int g, int b) {
    const lua_resource_t *r = the_string();
    if (!r)
        return;
    const lua_if_pixel_t *vt = r->vt;
    for (int i = 0, n = vt->count(r->ctx); i < n; i++)
        vt->set(r->ctx, i, (uint8_t)r_, (uint8_t)g, (uint8_t)b);
}

static int l_pixel_fill(lua_State *Ls) {
    fill(check_channel(Ls, 1), check_channel(Ls, 2), check_channel(Ls, 3));
    return 0;
}

static int l_pixel_clear(lua_State *Ls) {
    (void)Ls;
    fill(0, 0, 0);
    return 0;
}

static int l_pixel_show(lua_State *Ls) {
    (void)Ls;
    const lua_resource_t *r = the_string();
    if (r)
        ((const lua_if_pixel_t *)r->vt)->show(r->ctx);
    return 0;
}

/* ── sensor.* / flight.* / pyro.*  (all read-only, invariant L11) ─── */

static int l_sensor_pressure(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_pressure_pa());
    return 1;
}
static int l_sensor_altitude(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_altitude_cm());
    return 1;
}
static int l_sensor_speed(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_speed_cms());
    return 1;
}
static int l_flight_state(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_flight_state());
    return 1;
}
static int l_flight_time(lua_State *Ls) {
    lua_pushinteger(Ls, (lua_Integer)lua_plat_time_ms());
    return 1;
}
static int l_flight_max_alt(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_max_altitude_cm());
    return 1;
}
static int l_flight_under_thrust(lua_State *Ls) {
    lua_pushboolean(Ls, lua_plat_under_thrust());
    return 1;
}
static int l_flight_apogee(lua_State *Ls) {
    lua_pushboolean(Ls, lua_plat_apogee_detected());
    return 1;
}
static int l_flight_seq(lua_State *Ls) {
    lua_pushinteger(Ls, (lua_Integer)lua_plat_telem_seq());
    return 1;
}

#define PYRO_STATUS_FIELDS 6

static int l_pyro_status(lua_State *Ls) {
    lua_Integer ch = luaL_checkinteger(Ls, 1);
    if (ch != 1 && ch != 2)
        return luaL_error(Ls, "pyro channel must be 1 or 2");
    int st = lua_plat_pyro_status((int)ch);
    lua_createtable(Ls, 0, PYRO_STATUS_FIELDS);
    lua_pushboolean(Ls, st & LUA_PYRO_CONTINUITY);
    lua_setfield(Ls, -2, "continuity");
    lua_pushboolean(Ls, st & LUA_PYRO_FIRED);
    lua_setfield(Ls, -2, "fired");
    lua_pushboolean(Ls, st & LUA_PYRO_FAULT);
    lua_setfield(Ls, -2, "fault");
    lua_pushboolean(Ls, st & LUA_PYRO_ARMED);
    lua_setfield(Ls, -2, "armed");
    /* The raw count rides with the booleans, so a script judging a channel
     * gets both. It is sampled after a release too, when on MK1A it reads
     * the bridge midpoint a script drives; continuity and armed then
     * describe a pyro channel that no longer exists, and `released` says
     * which case this is. */
    lua_pushinteger(Ls, lua_plat_pyro_adc((int)ch));
    lua_setfield(Ls, -2, "adc");
    lua_pushboolean(Ls, lua_plat_pyro_released((int)ch));
    lua_setfield(Ls, -2, "released");
    return 1;
}

/* ── log.* ────────────────────────────────────────────────────────
 *
 * Hands bytes to the application, which owns the file: a script never writes
 * flash (invariant L6). Never blocks -- a flooding script loses output rather
 * than stalling its task. */
static int l_log_write(lua_State *Ls) {
    size_t len;
    const char *s = luaL_checklstring(Ls, 1, &len);
    lua_plat_log_write(s, (int)len);
    return 0;
}

static int l_log_line(lua_State *Ls) {
    size_t len;
    const char *s = luaL_checklstring(Ls, 1, &len);
    lua_plat_log_write(s, (int)len);
    lua_plat_log_write("\n", 1);
    return 0;
}

/* ── Restartable pattern matching ─────────────────────────────────
 *
 * A search for an unanchored pattern tries every start position, so its cost
 * is N attempts on an N-byte subject. Measured with
 * string.match(("a"):rep(N), "(a-)*$") on x86:
 *
 *      N=400   1.0 ms        N=1600  13.7 ms
 *      N=800   4.0 ms        N=3200  39.6 ms
 *
 * Above PATTERN_DIRECT_MAX the loop over start positions moves here, each
 * attempt anchored, so a tick() can suspend between attempts. Checked against
 * string.find across 20 cases covering anchors, captures, character classes,
 * %b, %f and init offsets. The longest unsuspendable span drops from 37 ms to
 * 0.02 ms at N=3200; total work stays quadratic. What one attempt may cost is
 * bounded by pyro_lua_match_step().
 *
 * State lives in the activation, never in a static or an upvalue: a gsub
 * replacement can call gsub again, gmatch iterators interleave, and a
 * suspended search resumes after other code has run.
 *
 * Load, init() and the console eval cannot yield, so they take a length cap
 * instead. */
#ifndef PYRO_LUA_PATTERN_MAX
#define PYRO_LUA_PATTERN_MAX 128
#endif

/* Below this the per-position loop costs more call overhead than it saves. */
#define PATTERN_DIRECT_MAX 64

/* Upvalues on every wrapper. */
#define PU_ORIG 1  /* the original C function, as light userdata */
#define PU_PLAIN 2 /* arg index that disables patterns, or 0     */
#define PU_NAME 3  /* the function's name, for the cap's message  */

static int pattern_loop(lua_State *Ls, lua_Integer pos);

/* Resumed with the activation's stack as it was -- subject at 1, anchored
 * pattern at 2 -- so only the position travels in ctx. */
static int pattern_k(lua_State *Ls, int status, lua_KContext ctx) {
    (void)status;
    return pattern_loop(Ls, (lua_Integer)ctx);
}

static int pattern_loop(lua_State *Ls, lua_Integer pos) {
    size_t len = 0;
    (void)lua_tolstring(Ls, 1, &len);
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(PU_ORIG));

    for (; pos <= (lua_Integer)len + 1; pos++) {
        if (slice_boxed && past(slice_deadline_us) && lua_isyieldable(Ls)) {
            return lua_yieldk(Ls, 0, (lua_KContext)pos, pattern_k); /* resumes in pattern_k */
        }

        /* One anchored attempt at this position. Only the init offset at 3
         * is rewritten, so a resumed call sees the stack a fresh one does;
         * top stays at 3 so the original reads no stale `plain` flag. */
        lua_settop(Ls, 3);
        lua_pushinteger(Ls, pos);
        lua_replace(Ls, 3);

        int n = orig(Ls);
        if (n > 0 && !lua_isnil(Ls, -n)) {
            return n;
        }
        lua_settop(Ls, 3);
    }
    lua_pushnil(Ls);
    return 1;
}

static int refuse_long_subject(lua_State *Ls, size_t len) {
    return luaL_error(Ls, "pattern on %d bytes exceeds the %d byte limit outside a work unit", (int)len,
                      PYRO_LUA_PATTERN_MAX);
}

static int l_pattern_guard(lua_State *Ls) {
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(PU_ORIG));
    const int plain_arg = (int)lua_tointeger(Ls, lua_upvalueindex(PU_PLAIN));

    size_t len = 0;
    if (lua_isstring(Ls, 1)) {
        (void)lua_tolstring(Ls, 1, &len);
    }

    /* find(s, p, init, true) is a substring search, not pattern matching. */
    if (plain_arg && lua_toboolean(Ls, plain_arg)) {
        return orig(Ls);
    }
    if (len <= PATTERN_DIRECT_MAX) {
        return orig(Ls);
    }

    size_t plen = 0;
    const char *p = lua_tolstring(Ls, 2, &plen);
    if (!p) {
        return orig(Ls);
    }
    if (!lua_isyieldable(Ls) && len > (size_t)PYRO_LUA_PATTERN_MAX) {
        return refuse_long_subject(Ls, len);
    }
    /* An anchored pattern tries one position already; a second "^" would
     * change its meaning. */
    if (plen > 0 && p[0] == '^') {
        return orig(Ls);
    }

    lua_Integer init = luaL_optinteger(Ls, 3, 1);
    if (init < 0) {
        init = (lua_Integer)len + init + 1;
    }
    if (init < 1) {
        init = 1;
    }

    /* Built once, not per position, and installed at argument 2 of THIS
     * activation: string.find is one closure shared by every caller, so an
     * upvalue would be overwritten by an on_event() handler's search while a
     * tick()'s is suspended. */
    lua_pushliteral(Ls, "^");
    lua_pushvalue(Ls, 2);
    lua_concat(Ls, 2);
    lua_replace(Ls, 2);

    return pattern_loop(Ls, init);
}

static void wrap_string_fn(lua_State *Ls, const char *name, lua_CFunction wrapper, int plain_arg, bool named) {
    lua_getglobal(Ls, "string");
    lua_getfield(Ls, -1, name);
    lua_CFunction orig = lua_tocfunction(Ls, -1);
    lua_pop(Ls, 1);
    if (!orig) {
        lua_pop(Ls, 1);
        return;
    }
    lua_pushlightuserdata(Ls, (void *)orig);
    lua_pushinteger(Ls, plain_arg);
    if (named) {
        lua_pushstring(Ls, name);
    } else {
        lua_pushnil(Ls);
    }
    lua_pushcclosure(Ls, wrapper, 3);
    lua_setfield(Ls, -2, name);
    lua_pop(Ls, 1);
}

/* Capped rather than made restartable: suspending gsub means carrying partial
 * output across yields, a replacement function can itself yield, and gmatch
 * keeps its state inside an iterator the matcher owns. A script scanning a
 * long string uses find, which is restartable. */
static int l_pattern_cap(lua_State *Ls) {
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(PU_ORIG));
    size_t len = 0;
    if (lua_isstring(Ls, 1)) {
        (void)lua_tolstring(Ls, 1, &len);
    }
    if (len > (size_t)PYRO_LUA_PATTERN_MAX) {
        return luaL_error(Ls,
                          "%s on %d bytes exceeds the %d byte limit "
                          "(not restartable; use string.find to scan)",
                          lua_tostring(Ls, lua_upvalueindex(PU_NAME)), (int)len, PYRO_LUA_PATTERN_MAX);
    }
    return orig(Ls);
}

static void guard_patterns(lua_State *Ls) {
    wrap_string_fn(Ls, "find", l_pattern_guard, 4, false); /* find(s, p, init, plain) */
    wrap_string_fn(Ls, "match", l_pattern_guard, 0, false);
    wrap_string_fn(Ls, "gsub", l_pattern_cap, 0, true);
    wrap_string_fn(Ls, "gmatch", l_pattern_cap, 0, true);
}

/* ── Number parsing without the C library ─────────────────────────
 *
 * newlib's strtof allocates from the system heap, which Lua may not use
 * (lua_arena.c). Decimal only: lobject.c parses hex itself, and l_str2d has
 * already refused "inf" and "nan". Accumulating in double keeps the float
 * result within half an ulp of strtof's except in double-rounding corner
 * cases; source literals and tonumber() go through the same function, so a
 * script always agrees with itself. */
#define STR2NUM_DIGITS 19   /* what a uint64_t holds exactly */
#define STR2NUM_EXP_MAX 400 /* well past float's range either way */

float pyro_lua_str2number(const char *s, char **endptr) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    bool negative = (*p == '-');
    if (*p == '-' || *p == '+')
        p++;

    uint64_t mant = 0;
    int digits = 0, exp10 = 0;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; p++, any = true) {
        if (digits < STR2NUM_DIGITS) {
            mant = mant * 10u + (uint64_t)(*p - '0');
            digits += (mant != 0);
        } else {
            exp10++;
        }
    }
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++, any = true) {
            if (digits < STR2NUM_DIGITS) {
                mant = mant * 10u + (uint64_t)(*p - '0');
                digits += (mant != 0);
                exp10--;
            }
        }
    }
    if (!any) {
        *endptr = (char *)s;
        return 0.0f;
    }
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        bool eneg = (*e == '-');
        if (*e == '-' || *e == '+')
            e++;
        if (*e >= '0' && *e <= '9') {
            int ev = 0;
            for (; *e >= '0' && *e <= '9'; e++) {
                if (ev < STR2NUM_EXP_MAX)
                    ev = ev * 10 + (*e - '0');
            }
            exp10 += eneg ? -ev : ev;
            p = e;
        }
    }
    *endptr = (char *)p;

    double v = (double)mant;
    double scale = 1.0, base = 10.0;
    for (int n = exp10 < 0 ? -exp10 : exp10; n > 0 && mant != 0; n >>= 1, base *= base) {
        if (n & 1)
            scale *= base;
    }
    v = (exp10 < 0) ? v / scale : v * scale;
    return (float)(negative ? -v : v);
}

/* ── Environment construction ─────────────────────────────────────── */

/* Not luaL_newlib(): it expands to sizeof(array)/sizeof(elem), which
 * computes a pointer size when the table arrives as a parameter. */
static void reg_table(lua_State *Ls, const char *name, const luaL_Reg *fns) {
    int n = 0;
    while (fns[n].name)
        n++;
    lua_createtable(Ls, 0, n);
    luaL_setfuncs(Ls, fns, 0);
    lua_setglobal(Ls, name);
}

/* [LUA-SAFE-11] flight.<NAME>, spelled from the enumerators so the numbers cannot drift
 * from flight_state_t. No flight.BOOT: boot is four states, and any one
 * number would miss three of them. */
#define FLIGHT_STATE(name)                                                                                             \
    { #name, name }
static const struct {
    const char *name;
    flight_state_t value;
} flight_state_names[] = {
    FLIGHT_STATE(BOOT_SETTLE),    FLIGHT_STATE(BOOT_CONTINUITY), FLIGHT_STATE(BOOT_CALIBRATE),
    FLIGHT_STATE(PAD_IDLE),       FLIGHT_STATE(ASCENT),          FLIGHT_STATE(FALLING),
    FLIGHT_STATE(DROGUE_DESCENT), FLIGHT_STATE(CHUTE_DESCENT),   FLIGHT_STATE(LANDED),
    FLIGHT_STATE(BOOT_SENSOR),    FLIGHT_STATE(FAULT),           FLIGHT_STATE(GROUND_TEST),
    {"DROGUE", DROGUE_DESCENT},   {"CHUTE", CHUTE_DESCENT},
};
_Static_assert(STATE_COUNT == 12, "flight_state_t changed: name the new state here and in docs/lua.html");

/* Invariant L4: a capability with no resources gets no table, so
 * `serial.write(...)` without serial fails as "attempt to index a nil value
 * (global 'serial')", naming what is missing. */
static void build_env(lua_State *Ls) {
    static const luaL_Reg output_fns[] = {
        {"set", l_output_set}, {"get", l_output_get}, {"list", l_output_list}, {NULL, NULL}};
    static const luaL_Reg input_fns[] = {{"get", l_input_get}, {"list", l_input_list}, {NULL, NULL}};
    static const luaL_Reg serial_fns[] = {{"write", l_serial_write}, {"read", l_serial_read}, {NULL, NULL}};
    static const luaL_Reg sensor_fns[] = {{"pressure_pa", l_sensor_pressure},
                                          {"altitude_cm", l_sensor_altitude},
                                          {"speed_cms", l_sensor_speed},
                                          {NULL, NULL}};
    static const luaL_Reg flight_fns[] = {{"state", l_flight_state},
                                          {"time_ms", l_flight_time},
                                          {"max_altitude_cm", l_flight_max_alt},
                                          {"under_thrust", l_flight_under_thrust},
                                          {"apogee", l_flight_apogee},
                                          {"seq", l_flight_seq},
                                          {NULL, NULL}};
    static const luaL_Reg log_fns[] = {{"write", l_log_write}, {"line", l_log_line}, {NULL, NULL}};
    static const luaL_Reg pyro_fns[] = {{"status", l_pyro_status}, {NULL, NULL}};
    static const luaL_Reg pixel_fns[] = {{"count", l_pixel_count}, {"set", l_pixel_set},   {"fill", l_pixel_fill},
                                         {"clear", l_pixel_clear}, {"show", l_pixel_show}, {NULL, NULL}};

    if (lua_iface_count_kind(LUA_IF_OUTPUT) > 0)
        reg_table(Ls, "output", output_fns);
    if (lua_iface_count_kind(LUA_IF_INPUT) > 0)
        reg_table(Ls, "input", input_fns);
    if (lua_iface_count_kind(LUA_IF_SERIAL) > 0)
        reg_table(Ls, "serial", serial_fns);
    if (lua_iface_count_kind(LUA_IF_PIXEL) > 0)
        reg_table(Ls, "pixel", pixel_fns);

    /* Always present: reading state grants nothing. */
    reg_table(Ls, "sensor", sensor_fns);
    reg_table(Ls, "flight", flight_fns);
    reg_table(Ls, "pyro", pyro_fns);
    reg_table(Ls, "log", log_fns);

    lua_getglobal(Ls, "flight");
    for (unsigned i = 0; i < sizeof(flight_state_names) / sizeof(flight_state_names[0]); i++) {
        lua_pushinteger(Ls, flight_state_names[i].value);
        lua_setfield(Ls, -2, flight_state_names[i].name);
    }
    lua_pop(Ls, 1);
}

/* dofile and loadfile reach a filesystem through stdio; load could take
 * bytecode; collectgarbage could stop the collector the arena relies on;
 * rawset and rawget would step around a metatable the host installs. */
static void strip_globals(lua_State *Ls) {
    static const char *banned[] = {"dofile", "loadfile", "load", "require", "collectgarbage", "rawset", "rawget"};
    for (unsigned i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        lua_pushnil(Ls);
        lua_setglobal(Ls, banned[i]);
    }
}

static void replace_base_fns(lua_State *Ls) {
    lua_pushcfunction(Ls, l_pcall);
    lua_setglobal(Ls, "pcall");
    lua_pushcfunction(Ls, l_xpcall);
    lua_setglobal(Ls, "xpcall");
    lua_pushcfunction(Ls, l_print);
    lua_setglobal(Ls, "print");

    lua_getglobal(Ls, "setmetatable");
    lua_pushlightuserdata(Ls, (void *)lua_tocfunction(Ls, -1));
    lua_pushcclosure(Ls, l_setmetatable, 1);
    lua_setglobal(Ls, "setmetatable");
    lua_pop(Ls, 1);
}

/* Runs under lua_pcall, so running out of arena while building the
 * environment is an error rather than a panic. */
static int setup_env(lua_State *Ls) {
    luaL_requiref(Ls, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(Ls, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(Ls, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(Ls, LUA_TABLIBNAME, luaopen_table, 1);
    lua_settop(Ls, 0);

    guard_patterns(Ls);
    strip_globals(Ls);
    replace_base_fns(Ls);
    build_env(Ls);
    return 0;
}

/* ── Host interface ───────────────────────────────────────────────── */

void pyro_lua_init(void) {
    lua_arena_init(&arena, arena_buf, sizeof(arena_buf));
    last_error[0] = '\0';
    co = NULL;
    slice_running = false;

    L = lua_newstate(lua_arena_alloc, &arena);
    if (!L) {
        snprintf(last_error, sizeof(last_error), "arena too small for a VM");
        return;
    }
    lua_atpanic(L, on_panic);
    lua_pushcfunction(L, setup_env);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        record_error(L);
        lua_close(L);
        L = NULL;
        return;
    }
    lua_sethook(L, count_hook, LUA_MASKCOUNT, PYRO_LUA_HOOK_COUNT);
}

void pyro_lua_shutdown(void) {
    if (L) {
        lua_close(L);
        L = NULL;
    }
    co = NULL;
    slice_running = false;
}

/* Runs what is on L's stack -- a function and nargs arguments -- under
 * lua_pcall with the raising limits. budget_us = 0: instruction budget only. */
static bool run_protected(int nargs, uint32_t budget_us) {
    limits_start(budget_us, false);
    int rc = lua_pcall(L, nargs, 0, 0);
    hard_boxed = false;
    if (rc != LUA_OK) {
        record_error(L);
        return false;
    }
    return true;
}

/* Calls the global named by argument 1, passing the string at argument 2 if
 * there is one; both arrive as light userdata, which pushing does not
 * allocate. The lookup honours _G's metatable, so a script's strict-globals
 * rule applies to the host's lookup like any other. */
static int call_global(lua_State *Ls) {
    const char *name = lua_touserdata(Ls, 1);
    const char *arg = lua_touserdata(Ls, 2);
    lua_settop(Ls, 0);
    if (lua_getglobal(Ls, name) != LUA_TFUNCTION) {
        return 0;
    }
    if (arg) {
        lua_pushstring(Ls, arg);
    }
    lua_call(Ls, arg ? 1 : 0, 0);
    return 0;
}

static bool call_global_protected(const char *name, const char *arg, uint32_t budget_us) {
    lua_pushcfunction(L, call_global);
    lua_pushlightuserdata(L, (void *)name);
    lua_pushlightuserdata(L, (void *)arg);
    return run_protected(2, budget_us);
}

static int new_thread(lua_State *Ls) {
    lua_newthread(Ls);
    return 1;
}

/* At load and after an error: a coroutine that raised is dead, and resuming
 * it returns an error forever. */
static bool make_coroutine(void) {
    slice_running = false;
    co = NULL;
    lua_settop(L, 0);
    lua_pushcfunction(L, new_thread);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        record_error(L);
        return false;
    }
    co = lua_tothread(L, 1);
    lua_sethook(co, count_hook, LUA_MASKCOUNT, PYRO_LUA_HOOK_COUNT);
    return true;
}

/* A program that failed to start gets no tick(); the console still works. */
static bool load(const char *chunkname, const char *src, size_t len) {
    slice_running = false;
    co = NULL;
    lua_settop(L, 0);
    if (luaL_loadbufferx(L, src, len, chunkname, "t") != LUA_OK) {
        record_error(L);
        return false;
    }
    if (!run_protected(0, 0) || !call_global_protected("init", NULL, 0)) {
        return false;
    }
    /* After init(), so a tick() that init() defined is found. */
    return make_coroutine();
}

static int tick_done(lua_State *Ls, int status, lua_KContext ctx) {
    (void)Ls;
    (void)status;
    (void)ctx;
    return 0;
}

/* The coroutine's body. Looked up on every tick rather than cached: a script
 * may define tick() from inside init(), or replace it. lua_callk, so the
 * hook's yield inside tick() can come back through here. */
static int tick_entry(lua_State *Ls) {
    if (lua_getglobal(Ls, "tick") != LUA_TFUNCTION) {
        return 0;
    }
    lua_callk(Ls, 0, 0, 0, tick_done);
    return 0;
}

static pyro_lua_status_t tick_slice(uint32_t budget_us) {
    if (!L || !co) {
        return PYRO_LUA_DONE;
    }
    limits_start(budget_us, true);
    if (!slice_running) {
        lua_settop(co, 0);
        lua_pushcfunction(co, tick_entry);
        slice_running = true;
    }

    int nres = 0;
    int rc = lua_resume(co, L, 0, &nres);
    if (rc == LUA_YIELD) {
        return PYRO_LUA_YIELD;
    }
    slice_running = false;
    if (rc != LUA_OK) {
        record_error(co);
        make_coroutine();
        return PYRO_LUA_ERROR;
    }
    lua_settop(co, 0);
    return PYRO_LUA_DONE;
}

static bool eval(const char *src) {
    if (luaL_loadbufferx(L, src, strlen(src), "=console", "t") != LUA_OK) {
        record_error(L);
        return false;
    }
    return run_protected(0, 0);
}

/* The public entries below set the panic handler's way back. */

bool pyro_lua_load(const char *chunkname, const char *src, size_t len) {
    if (!L)
        return false;
    jmp_buf here;
    if (setjmp(here) != 0) {
        abandon_vm();
        return false;
    }
    panic_exit = &here;
    bool ok = load(chunkname, src, len);
    panic_exit = NULL;
    return ok;
}

pyro_lua_status_t pyro_lua_tick_slice(uint32_t budget_us) {
    jmp_buf here;
    if (setjmp(here) != 0) {
        abandon_vm();
        return PYRO_LUA_ERROR;
    }
    panic_exit = &here;
    pyro_lua_status_t st = tick_slice(budget_us);
    panic_exit = NULL;
    return st;
}

bool pyro_lua_tick(void) {
    pyro_lua_status_t st;
    do {
        st = pyro_lua_tick_slice(0);
    } while (st == PYRO_LUA_YIELD);
    return st != PYRO_LUA_ERROR;
}

bool pyro_lua_event(const char *event_name, uint32_t budget_us) {
    if (!L)
        return true;
    jmp_buf here;
    if (setjmp(here) != 0) {
        abandon_vm();
        return false;
    }
    panic_exit = &here;
    bool ok = call_global_protected("on_event", event_name, budget_us);
    panic_exit = NULL;
    return ok;
}

bool pyro_lua_eval(const char *src) {
    if (!L)
        return false;
    jmp_buf here;
    if (setjmp(here) != 0) {
        abandon_vm();
        return false;
    }
    panic_exit = &here;
    bool ok = eval(src);
    panic_exit = NULL;
    if (!ok) {
        lua_plat_console_out(last_error, (int)strlen(last_error));
        lua_plat_console_out("\n", 1);
    }
    return ok;
}

const char *pyro_lua_last_error(void) {
    return last_error;
}

void pyro_lua_get_stats(lua_arena_stats_t *out) {
    *out = arena.stats;
}

bool pyro_lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out) {
    if (L) {
        return false;
    }
    lua_check_in(arena_buf, sizeof(arena_buf), src, len, env, out);
    return true;
}

/* ── Events ───────────────────────────────────────────────────────
 *
 * One poster (the flight task) and one consumer (the VM's task), on either
 * core: each index has one writer, and release/acquire orders the name
 * before the index that publishes it. */
#define EVENT_QUEUE_LEN 8u /* [LUA-SAFE-12] a power of two; apogee brings two or three at once */
#define EVENT_NAME_MAX 16

static char event_names[EVENT_QUEUE_LEN][EVENT_NAME_MAX];
static atomic_uint event_head;     /* the poster's */
static atomic_uint event_tail;     /* the consumer's */
static atomic_uint events_dropped; /* the poster's */

bool pyro_lua_post_event(const char *name) {
    unsigned head = atomic_load_explicit(&event_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&event_tail, memory_order_acquire);
    if (head - tail >= EVENT_QUEUE_LEN) {
        atomic_store_explicit(&events_dropped, atomic_load_explicit(&events_dropped, memory_order_relaxed) + 1u,
                              memory_order_relaxed);
        return false;
    }
    char *slot = event_names[head % EVENT_QUEUE_LEN];
    strncpy(slot, name, EVENT_NAME_MAX - 1);
    slot[EVENT_NAME_MAX - 1] = '\0';
    atomic_store_explicit(&event_head, head + 1u, memory_order_release);
    return true;
}

void pyro_lua_run_events(uint32_t budget_us) {
    uint32_t start_us = lua_plat_now_us();
    unsigned tail = atomic_load_explicit(&event_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&event_head, memory_order_acquire);
    for (; tail != head; tail++) {
        uint32_t left = 0u;
        if (budget_us != 0u) {
            uint32_t spent = lua_plat_now_us() - start_us;
            left = spent < budget_us ? budget_us - spent : 1u;
        }
        pyro_lua_event(event_names[tail % EVENT_QUEUE_LEN], left);
        atomic_store_explicit(&event_tail, tail + 1u, memory_order_release);
    }
}

uint32_t pyro_lua_events_dropped(void) {
    return atomic_load_explicit(&events_dropped, memory_order_relaxed);
}
