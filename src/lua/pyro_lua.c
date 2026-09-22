/*
 * Lua VM host for user programs.
 *
 * Owns the VM lifecycle, the resource limits that keep a bad script from
 * mattering, and the sandbox. The bindings themselves are the security
 * boundary — RP2040 has no MPU, so nothing below this line is enforced by
 * hardware. Read them as you would read a syscall table.
 *
 * TEXT CHUNKS ONLY. Every load below passes mode "t" rather than using
 * luaL_loadbuffer(), whose NULL mode means "bt" -- text or precompiled
 * bytecode. Lua 5.4 does not verify bytecode: lundump.c checks a header and
 * trusts the rest, and the manual says so. A crafted blob POSTed to
 * /api/lua/script would therefore execute with arbitrary load/store over the
 * whole address space, which on this board includes the pyro GPIO registers
 * and core0's stack -- every invariant below is a property of the BINDINGS,
 * and bytecode never reaches them.
 *
 * Invariants implemented here (see the plan for the full list):
 *   L4  a capability that configuration did not enable has no table at all,
 *       so a script referencing it fails immediately and legibly
 *   L5  nothing in the API names hardware; resources are addressed by the
 *       names configuration gave them
 *   L7  the heap is a fixed arena; exhaustion kills the script, not the host
 *   L8  every invocation has an instruction budget
 *   L11 no binding can arm, fire, disarm or alter flight state
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro_lua.h"
#include "lua_arena.h"
#include "lua_platform.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdio.h>

#ifndef PYRO_LUA_ARENA_BYTES
#define PYRO_LUA_ARENA_BYTES (32 * 1024)
#endif

/* Instructions between hook calls. Also bounds how long the VM can go
 * without noticing a park request on the target. */
#ifndef PYRO_LUA_HOOK_COUNT
#define PYRO_LUA_HOOK_COUNT 1000
#endif

/* Hook calls allowed per invocation before the script is considered runaway. */
#ifndef PYRO_LUA_BUDGET
#define PYRO_LUA_BUDGET 2000
#endif

/* Aligned because lua_arena.c declares ALIGN 8 and aligns every block
 * relative to this base -- a 4-aligned base makes every "8-aligned" block a
 * lie. Observed at 0x2000A5A4 before this attribute. */
static uint8_t arena_buf[PYRO_LUA_ARENA_BYTES] __attribute__((aligned(8)));
static lua_State *L;
static char last_error[160];

/* The work-unit coroutine.
 *
 * tick() runs here rather than under lua_pcall() because a C-call boundary is
 * not yieldable -- lua_pcall gives "attempt to yield from outside a
 * coroutine" -- and yielding is the whole mechanism: the hook stops the
 * script when its time box expires and the next grant resumes it mid-loop.
 *
 * Anchored at index 1 of L's stack so the collector cannot take it. */
static lua_State *co;
static bool slice_running; /* a tick() is mid-flight, awaiting its next grant */
static uint32_t slice_deadline_us;
static bool slice_boxed; /* false for a grant of 0: run to completion */

/* Instruction budget, still used for the paths that CANNOT yield.
 *
 * Loading a chunk, running init() and the console eval all go through
 * lua_pcall, which is a non-yieldable C-call boundary. They cannot be time
 * boxed, so a runaway in one of them is still stopped the old way: the hook
 * raises once the budget is gone. tick() does not use this -- a long tick is
 * a legitimate program that simply spans several grants. */
static uint32_t budget_left;

/* ── Limits ───────────────────────────────────────────────────────── */

/* Provided by lua_core1.c on the target, where it parks the VM outside flash
 * so core0 can erase. Weak and empty everywhere else, so this file stays
 * board-independent and the host tests link without multicore. */
__attribute__((weak)) void lua_core1_park_check(void) {}

/* Fires every PYRO_LUA_HOOK_COUNT VM instructions. The hook must stay a COUNT
 * hook -- Lua permits yielding only from count and line hooks -- but the
 * decision is time, not instructions, because the thing being bounded is how
 * long core0 waits, and instructions are a poor proxy for that.
 *
 * PYRO_LUA_HOOK_COUNT therefore sets how finely the deadline is honoured, not
 * how much work is allowed. */
static void count_hook(lua_State *Ls, lua_Debug *ar) {
    (void)ar;
    /* First, so a script about to be stopped still answers a pending park:
     * core0's flash write must not wait for the VM to finish anything. */
    lua_core1_park_check();

    if (!slice_boxed) {
        /* Non-yieldable path (load, init, eval), or a grant of 0. Fall back to
         * the instruction budget: it is the only stop available when the call
         * cannot be suspended and resumed. */
        if (budget_left == 0) {
            luaL_error(Ls, "instruction budget exhausted");
        }
        budget_left--;
        return;
    }
    if ((int32_t)(lua_plat_now_us() - slice_deadline_us) >= 0) {
        /* Does not return -- longjmps out to lua_resume, which reports
         * LUA_YIELD. Nothing after this line runs. */
        lua_yield(Ls, 0);
    }
}

static int on_panic(lua_State *Ls) {
    const char *m = lua_tostring(Ls, -1);
    snprintf(last_error, sizeof(last_error), "panic: %s", m ? m : "?");
    return 0; /* returns to the host's setjmp rather than abort() */
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

/* Resolve a Lua-supplied name to a platform index. Returns -1 if the name is
 * not one configuration granted. This is where L5 is actually enforced: the
 * only thing a script can say is a name, and only names in this list resolve. */
static int find_output(const char *name) {
    for (int i = 0; i < lua_plat_output_count(); i++)
        if (strcmp(lua_plat_output_desc(i)->name, name) == 0)
            return i;
    return -1;
}
static int find_input(const char *name) {
    for (int i = 0; i < lua_plat_input_count(); i++)
        if (strcmp(lua_plat_input_desc(i)->name, name) == 0)
            return i;
    return -1;
}
static int find_serial(const char *name) {
    for (int i = 0; i < lua_plat_serial_count(); i++)
        if (strcmp(lua_plat_serial_desc(i)->name, name) == 0)
            return i;
    return -1;
}

/* ── output.* ─────────────────────────────────────────────────────── */

static int l_output_set(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    int idx = find_output(name);
    if (idx < 0)
        return luaL_error(Ls, "no output named '%s'", name);

    int v;
    if (lua_isboolean(Ls, 2)) {
        v = lua_toboolean(Ls, 2) ? 100 : 0;
    } else {
        v = (int)luaL_checkinteger(Ls, 2);
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        if (v != 0 && v != 100 && !lua_plat_output_desc(idx)->dimmable)
            return luaL_error(Ls, "output '%s' is not dimmable", name);
    }
    lua_plat_output_set(idx, v);
    return 0;
}

static int l_output_get(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    int idx = find_output(name);
    if (idx < 0)
        return luaL_error(Ls, "no output named '%s'", name);
    lua_pushinteger(Ls, lua_plat_output_get(idx));
    return 1;
}

static int l_output_list(lua_State *Ls) {
    int n = lua_plat_output_count();
    lua_createtable(Ls, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushstring(Ls, lua_plat_output_desc(i)->name);
        lua_rawseti(Ls, -2, i + 1);
    }
    return 1;
}

/* ── input.* ──────────────────────────────────────────────────────── */

static int l_input_get(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    int idx = find_input(name);
    if (idx < 0)
        return luaL_error(Ls, "no input named '%s'", name);
    lua_pushboolean(Ls, lua_plat_input_get(idx));
    return 1;
}

static int l_input_list(lua_State *Ls) {
    int n = lua_plat_input_count();
    lua_createtable(Ls, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushstring(Ls, lua_plat_input_desc(i)->name);
        lua_rawseti(Ls, -2, i + 1);
    }
    return 1;
}

/* ── serial.* ─────────────────────────────────────────────────────── */

static int l_serial_write(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    size_t len;
    const char *s = luaL_checklstring(Ls, 2, &len);
    int idx = find_serial(name);
    if (idx < 0)
        return luaL_error(Ls, "no serial named '%s'", name);
    lua_pushinteger(Ls, lua_plat_serial_write(idx, s, (int)len));
    return 1;
}

static int l_serial_read(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);
    int max = (int)luaL_optinteger(Ls, 2, 64);
    int idx = find_serial(name);
    if (idx < 0)
        return luaL_error(Ls, "no serial named '%s'", name);
    if (max < 1)
        max = 1;
    if (max > 256)
        max = 256;
    char buf[256];
    int n = lua_plat_serial_read(idx, buf, max);
    if (n <= 0) {
        lua_pushnil(Ls);
        return 1;
    }
    lua_pushlstring(Ls, buf, (size_t)n);
    return 1;
}

/* ── pixel.*  (addressable LED string) ────────────────────────────── */

static int l_pixel_count(lua_State *Ls) {
    lua_pushinteger(Ls, lua_plat_pixel_count());
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
    /* 1-based, matching Lua table convention rather than C. */
    lua_Integer i = luaL_checkinteger(Ls, 1);
    int n = lua_plat_pixel_count();
    if (i < 1 || i > n)
        return luaL_error(Ls, "pixel %d out of range (1..%d)", (int)i, n);
    lua_plat_pixel_set((int)i - 1, (uint8_t)check_channel(Ls, 2), (uint8_t)check_channel(Ls, 3),
                       (uint8_t)check_channel(Ls, 4));
    return 0;
}

static int l_pixel_fill(lua_State *Ls) {
    int r = check_channel(Ls, 1), g = check_channel(Ls, 2), b = check_channel(Ls, 3);
    for (int i = 0; i < lua_plat_pixel_count(); i++)
        lua_plat_pixel_set(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    return 0;
}

static int l_pixel_clear(lua_State *Ls) {
    (void)Ls;
    for (int i = 0; i < lua_plat_pixel_count(); i++)
        lua_plat_pixel_set(i, 0, 0, 0);
    return 0;
}

static int l_pixel_show(lua_State *Ls) {
    (void)Ls;
    lua_plat_pixel_show();
    return 0;
}

/* ── sensor.* / flight.* / pyro.*  (all read-only, L11) ───────────── */

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
static int l_pyro_status(lua_State *Ls) {
    int ch = (int)luaL_checkinteger(Ls, 1);
    if (ch != 1 && ch != 2)
        return luaL_error(Ls, "pyro channel must be 1 or 2");
    int st = lua_plat_pyro_status(ch);
    lua_createtable(Ls, 0, 4);
    lua_pushboolean(Ls, st & LUA_PYRO_CONTINUITY);
    lua_setfield(Ls, -2, "continuity");
    lua_pushboolean(Ls, st & LUA_PYRO_FIRED);
    lua_setfield(Ls, -2, "fired");
    lua_pushboolean(Ls, st & LUA_PYRO_FAULT);
    lua_setfield(Ls, -2, "fault");
    lua_pushboolean(Ls, st & LUA_PYRO_ARMED);
    lua_setfield(Ls, -2, "armed");
    /* The raw count belongs with the booleans, not in a separate call: a
     * script deciding whether a channel is trustworthy wants both, and
     * splitting them invites reading one without the other. */
    lua_pushinteger(Ls, lua_plat_pyro_adc(ch));
    lua_setfield(Ls, -2, "adc");
    return 1;
}

/* ── log.* ────────────────────────────────────────────────────────
 *
 * Hands bytes to the application, which owns the file. A script never writes
 * flash: on the target it runs on core1, and core1 touching flash is the
 * hazard the whole design exists to prevent. Never blocks -- a flooding
 * script loses output rather than stalling the core. */
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
 * Lua's matcher is the one C function a script can reach that is neither
 * preemptible nor bounded by the arena. No VM instructions run inside it, so
 * the count hook never fires and the time box cannot stop it; and matching
 * allocates nothing, so the arena never refuses. Backtracking is quadratic.
 * Measured with string.match(("a"):rep(N), "(a-)*$"), zero hook calls:
 *
 *      N=400   1.0 ms        N=1600  13.7 ms
 *      N=800   4.0 ms        N=3200  39.6 ms
 *
 * MAXCCALLS does not save this. It bounds recursion DEPTH, which catches
 * `(a*)*` but not the lazy `(a-)*`: that backtracks iteratively, never
 * recurses deeply, and simply grinds.
 *
 * THE FIX: that cost is N start positions times O(N) per attempt, not one
 * unbounded attempt. Anchoring the pattern makes each attempt independent, so
 * the search becomes a loop over positions that can be suspended between
 * them. Verified equivalent to string.find across 20 cases covering anchors,
 * captures, character classes, %b, %f and init offsets.
 *
 * The loop stays in C. lua_yieldk() suspends it -- unwinding with longjmp --
 * and Lua calls the continuation to resume at the saved position. Measured:
 * the longest un-interruptible span drops from the whole search to one
 * attempt, 37 ms to ~0.02 ms at N=3200.
 *
 * State lives in the activation, never in a static. gsub replacements can be
 * functions that call gsub again, and gmatch iterators interleave, so a
 * single shared state store would be clobbered by nesting.
 *
 * This does NOT make matching cheaper -- total work is still quadratic. It
 * makes it interruptible, which converts "core1 unstoppable and core0 dead"
 * into "the script starves itself and the skip counter says so".
 *
 * Not everything is yieldable: load, init() and the console eval run under
 * lua_pcall, a non-yieldable C-call boundary. Those keep a length cap, which
 * costs nothing real -- the firmware never calls a pattern function itself,
 * and core0 withholds flash for the whole of core1's startup anyway. */
#ifndef PYRO_LUA_PATTERN_MAX
#define PYRO_LUA_PATTERN_MAX 128
#endif

/* Below this, one call is already bounded and cheap; looping per position
 * would pay N call overheads to save nothing. */
#define PATTERN_DIRECT_MAX 64

/* Upvalues on every wrapper. */
#define PU_ORIG 1     /* the original C function, as light userdata */
#define PU_PLAIN 2    /* arg index that disables patterns, or 0     */
#define PU_ANCHORED 3 /* the "^"-prefixed pattern, built once       */

static int pattern_loop(lua_State *Ls, lua_Integer pos);

static int pattern_k(lua_State *Ls, int status, lua_KContext ctx) {
    (void)status;
    /* Resumed. The activation's stack -- subject, pattern, upvalues -- is
     * exactly as it was, so only the position needs carrying in ctx. */
    return pattern_loop(Ls, (lua_Integer)ctx);
}

static int pattern_loop(lua_State *Ls, lua_Integer pos) {
    size_t len = 0;
    (void)lua_tolstring(Ls, 1, &len);
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(PU_ORIG));

    for (; pos <= (lua_Integer)len + 1; pos++) {
        if (slice_boxed && (int32_t)(lua_plat_now_us() - slice_deadline_us) >= 0 && lua_isyieldable(Ls)) {
            /* Does not return. Lua unwinds and calls pattern_k on resume. */
            return lua_yieldk(Ls, 0, (lua_KContext)pos, pattern_k);
        }

        /* One anchored attempt at exactly this position.
         *
         * The subject stays at index 1 untouched; only the pattern and the
         * init offset are rewritten, so a resumed call sees the same stack
         * shape as a fresh one. Top is left at 3 so the original sees no
         * fourth argument and does not read a stale `plain` flag. */
        lua_settop(Ls, 3);
        lua_pushvalue(Ls, lua_upvalueindex(PU_ANCHORED));
        lua_replace(Ls, 2);
        lua_pushinteger(Ls, pos);
        lua_replace(Ls, 3);

        int n = orig(Ls);
        if (n > 0 && !lua_isnil(Ls, -n)) {
            return n; /* matched here */
        }
        lua_settop(Ls, 3);
    }
    lua_pushnil(Ls);
    return 1;
}

static int l_pattern_guard(lua_State *Ls) {
    lua_CFunction orig = (lua_CFunction)lua_touserdata(Ls, lua_upvalueindex(PU_ORIG));
    const int plain_arg = (int)lua_tointeger(Ls, lua_upvalueindex(PU_PLAIN));

    size_t len = 0;
    if (lua_isstring(Ls, 1)) {
        (void)lua_tolstring(Ls, 1, &len);
    }

    /* string.find(s, p, init, true) is a substring search, not pattern
     * interpretation: length is not a hazard, so leave it alone. */
    if (plain_arg && lua_toboolean(Ls, plain_arg)) {
        return orig(Ls);
    }
    if (len <= PATTERN_DIRECT_MAX) {
        return orig(Ls); /* already bounded; the loop would only cost more */
    }

    size_t plen = 0;
    const char *p = lua_tolstring(Ls, 2, &plen);
    if (!p) {
        return orig(Ls);
    }

    /* An already-anchored pattern tries one position by definition -- there
     * is nothing to loop over, and prefixing a second "^" would change it. */
    if (plen > 0 && p[0] == '^') {
        if (!lua_isyieldable(Ls) && len > (size_t)PYRO_LUA_PATTERN_MAX) {
            return luaL_error(Ls, "pattern on %d bytes exceeds the %d byte limit outside a work unit", (int)len,
                              PYRO_LUA_PATTERN_MAX);
        }
        return orig(Ls);
    }

    if (!lua_isyieldable(Ls) && len > (size_t)PYRO_LUA_PATTERN_MAX) {
        /* load / init() / eval: cannot be suspended, so cap instead. */
        return luaL_error(Ls, "pattern on %d bytes exceeds the %d byte limit outside a work unit", (int)len,
                          PYRO_LUA_PATTERN_MAX);
    }

    lua_Integer init = luaL_optinteger(Ls, 3, 1);
    if (init < 0) {
        init = (lua_Integer)len + init + 1;
    }
    if (init < 1) {
        init = 1;
    }

    /* Built once and kept as an upvalue: doing it per position would
     * allocate a string for every character of the subject. */
    lua_pushliteral(Ls, "^");
    lua_pushvalue(Ls, 2);
    lua_concat(Ls, 2);
    lua_replace(Ls, lua_upvalueindex(PU_ANCHORED));

    return pattern_loop(Ls, init);
}

/* Replace string.<name> with a restartable version of itself. Lua's own
 * matcher still does the work; only the loop over start positions moves. */
static void guard_pattern_fn(lua_State *Ls, const char *name, int plain_arg) {
    lua_getglobal(Ls, "string");
    lua_getfield(Ls, -1, name);
    lua_CFunction orig = lua_tocfunction(Ls, -1);
    lua_pop(Ls, 1);
    if (!orig) {
        lua_pop(Ls, 1);
        return; /* not a C function: leave it rather than break it */
    }
    lua_pushlightuserdata(Ls, (void *)orig);
    lua_pushinteger(Ls, plain_arg);
    lua_pushnil(Ls); /* PU_ANCHORED, filled per call */
    lua_pushcclosure(Ls, l_pattern_guard, 3);
    lua_setfield(Ls, -2, name);
    lua_pop(Ls, 1);
}

/* gsub and gmatch are capped rather than made restartable.
 *
 * gsub scans AND builds output, with string, table and function
 * replacements and %1 back-references, so suspending it means carrying
 * partial output across yields -- and a function replacement can itself
 * yield, which needs a second continuation nested inside the first.
 * gmatch's state lives inside an iterator closure the matcher owns.
 *
 * Both are reachable hazards, so neither is left unbounded: the cap applies
 * whether or not the caller could yield. A script that needs to scan a long
 * string can do it with find, which is restartable. */
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
                          lua_tostring(Ls, lua_upvalueindex(PU_ANCHORED)), (int)len, PYRO_LUA_PATTERN_MAX);
    }
    return orig(Ls);
}

static void cap_pattern_fn(lua_State *Ls, const char *name) {
    lua_getglobal(Ls, "string");
    lua_getfield(Ls, -1, name);
    lua_CFunction orig = lua_tocfunction(Ls, -1);
    lua_pop(Ls, 1);
    if (!orig) {
        lua_pop(Ls, 1);
        return;
    }
    lua_pushlightuserdata(Ls, (void *)orig);
    lua_pushinteger(Ls, 0);
    lua_pushstring(Ls, name); /* reused as the name in the error message */
    lua_pushcclosure(Ls, l_pattern_cap, 3);
    lua_setfield(Ls, -2, name);
    lua_pop(Ls, 1);
}

static void guard_patterns(lua_State *Ls) {
    guard_pattern_fn(Ls, "find", 4); /* find(s, p, init, plain) */
    guard_pattern_fn(Ls, "match", 0);
    cap_pattern_fn(Ls, "gsub");
    cap_pattern_fn(Ls, "gmatch");
}

/* ── Environment construction ─────────────────────────────────────── */

/* luaL_newlib() cannot be used here: it expands to sizeof(array)/sizeof(elem),
 * which silently computes a pointer size when the table arrives as a
 * parameter. Count the entries and size the table explicitly instead. */
static void reg_table(lua_State *Ls, const char *name, const luaL_Reg *fns) {
    int n = 0;
    while (fns[n].name)
        n++;
    lua_createtable(Ls, 0, n);
    luaL_setfuncs(Ls, fns, 0);
    lua_setglobal(Ls, name);
}

/* Build the environment from what the platform actually offers. A capability
 * with no resources gets no table, so `serial.write(...)` on a build without
 * serial fails as "attempt to index a nil value (global 'serial')" — which is
 * the point of L4: the failure names the missing capability. */
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

    if (lua_plat_output_count() > 0)
        reg_table(Ls, "output", output_fns);
    if (lua_plat_input_count() > 0)
        reg_table(Ls, "input", input_fns);
    if (lua_plat_serial_count() > 0)
        reg_table(Ls, "serial", serial_fns);
    if (lua_plat_pixel_count() > 0)
        reg_table(Ls, "pixel", pixel_fns);

    /* Always present: reading state grants nothing and costs nothing. */
    reg_table(Ls, "sensor", sensor_fns);
    reg_table(Ls, "flight", flight_fns);
    reg_table(Ls, "pyro", pyro_fns);
    reg_table(Ls, "log", log_fns);

    /* Flight state constants, so scripts compare against names. Values match
     * flight_state_t; see flight_states.h. */
    lua_getglobal(Ls, "flight");
    static const char *states[] = {"BOOT", "PAD_IDLE", "ASCENT", "FALLING", "DROGUE", "CHUTE", "LANDED"};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        lua_pushinteger(Ls, (lua_Integer)i);
        lua_setfield(Ls, -2, states[i]);
    }
    lua_pop(Ls, 1);
}

/* Remove what the standard libraries leave lying around. dofile and loadfile
 * reach a filesystem through stdio; load and dofile can pull in bytecode,
 * which is not something a user upload should be able to do. */
static void strip_globals(lua_State *Ls) {
    static const char *banned[] = {"dofile", "loadfile", "load", "require", "collectgarbage", "rawset", "rawget"};
    for (unsigned i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        lua_pushnil(Ls);
        lua_setglobal(Ls, banned[i]);
    }
}

/* ── Host interface ───────────────────────────────────────────────── */

void pyro_lua_init(void) {
    lua_arena_init(arena_buf, sizeof(arena_buf));
    last_error[0] = '\0';

    L = lua_newstate(lua_arena_alloc, NULL);
    if (!L) {
        snprintf(last_error, sizeof(last_error), "arena too small for a VM");
        return;
    }
    lua_atpanic(L, on_panic);

    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);

    guard_patterns(L);
    strip_globals(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    build_env(L);

    lua_sethook(L, count_hook, LUA_MASKCOUNT, PYRO_LUA_HOOK_COUNT);
}

void pyro_lua_shutdown(void) {
    if (L) {
        lua_close(L);
        L = NULL;
    }
}

/* Fresh coroutine, anchored on L's stack so the collector keeps it.
 *
 * Called at load and again after an error, because a coroutine that raised is
 * dead and cannot be resumed -- resuming one returns an error forever, which
 * would turn a single script fault into a permanently broken tick. */
static void make_coroutine(void) {
    slice_running = false;
    lua_settop(L, 0); /* drops the previous thread, if any */
    co = lua_newthread(L);
    if (co) {
        lua_sethook(co, count_hook, LUA_MASKCOUNT, PYRO_LUA_HOOK_COUNT);
    }
}

static bool run_protected(int nargs) {
    slice_boxed = false; /* cannot yield across lua_pcall; use the budget */
    budget_left = PYRO_LUA_BUDGET;
    int rc = lua_pcall(L, nargs, 0, 0);
    if (rc != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        snprintf(last_error, sizeof(last_error), "%s", m ? m : "error");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool pyro_lua_load(const char *chunkname, const char *src, size_t len) {
    if (!L)
        return false;
    budget_left = PYRO_LUA_BUDGET;
    if (luaL_loadbufferx(L, src, len, chunkname, "t") != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        snprintf(last_error, sizeof(last_error), "%s", m ? m : "syntax error");
        lua_pop(L, 1);
        return false;
    }
    if (!run_protected(0))
        return false;

    lua_getglobal(L, "init");
    if (lua_isfunction(L, -1)) {
        if (!run_protected(0)) {
            make_coroutine();
            return false;
        }
    } else {
        lua_pop(L, 1);
    }

    /* After init(), so a tick() that init() defined is visible. */
    make_coroutine();
    return co != NULL;
}

pyro_lua_status_t pyro_lua_tick_slice(uint32_t budget_us) {
    if (!L || !co) {
        return PYRO_LUA_DONE;
    }

    slice_boxed = (budget_us != 0u);
    slice_deadline_us = lua_plat_now_us() + budget_us;

    if (!slice_running) {
        /* Looked up per call rather than cached: a script may define tick()
         * from inside init(), and a stale "no tick" flag would silently never
         * run it. */
        lua_getglobal(co, "tick");
        if (!lua_isfunction(co, -1)) {
            lua_pop(co, 1);
            return PYRO_LUA_DONE;
        }
        slice_running = true;
    }

    int nres = 0;
    int rc = lua_resume(co, L, 0, &nres);

    if (rc == LUA_YIELD) {
        return PYRO_LUA_YIELD; /* time box expired; resume on the next grant */
    }

    slice_running = false;
    if (rc != LUA_OK) {
        const char *m = lua_tostring(co, -1);
        snprintf(last_error, sizeof(last_error), "%s", m ? m : "error");
        make_coroutine(); /* the old one is dead; a fault must not be permanent */
        return PYRO_LUA_ERROR;
    }
    lua_settop(co, 0); /* drop tick()'s results, if it returned any */
    return PYRO_LUA_DONE;
}

bool pyro_lua_tick(void) {
    /* No time box, so the instruction budget is what stops a runaway -- the
     * same bound an un-dispatched caller had before slicing existed. */
    budget_left = PYRO_LUA_BUDGET;
    pyro_lua_status_t st;
    do {
        st = pyro_lua_tick_slice(0);
    } while (st == PYRO_LUA_YIELD);
    return st != PYRO_LUA_ERROR;
}

bool pyro_lua_event(const char *event_name) {
    if (!L)
        return true;
    lua_getglobal(L, "on_event");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    lua_pushstring(L, event_name);
    return run_protected(1);
}

bool pyro_lua_eval(const char *src) {
    if (!L)
        return false;
    budget_left = PYRO_LUA_BUDGET;
    if (luaL_loadbufferx(L, src, strlen(src), "=console", "t") != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        snprintf(last_error, sizeof(last_error), "%s", m ? m : "syntax error");
        lua_plat_console_out(last_error, (int)strlen(last_error));
        lua_plat_console_out("\n", 1);
        lua_pop(L, 1);
        return false;
    }
    if (!run_protected(0)) {
        lua_plat_console_out(last_error, (int)strlen(last_error));
        lua_plat_console_out("\n", 1);
        return false;
    }
    return true;
}

const char *pyro_lua_last_error(void) {
    return last_error;
}

void pyro_lua_get_stats(lua_arena_stats_t *out) {
    lua_arena_get_stats(out);
}
