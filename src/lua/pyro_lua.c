/*
 * Lua VM host for user programs.
 *
 * Owns the VM lifecycle, the resource limits that keep a bad script from
 * mattering, and the sandbox. The bindings themselves are the security
 * boundary — RP2040 has no MPU, so nothing below this line is enforced by
 * hardware. Read them as you would read a syscall table.
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

static uint8_t arena_buf[PYRO_LUA_ARENA_BYTES];
static lua_State *L;
static char last_error[160];
static uint32_t budget_left;
static bool have_tick;

/* ── Limits ───────────────────────────────────────────────────────── */

static void count_hook(lua_State *Ls, lua_Debug *ar) {
    (void)ar;
    if (budget_left == 0) {
        /* Not an error the script can catch: pcall will surface it, but the
         * budget stays at zero so a pcall-wrapped infinite loop cannot simply
         * carry on. Reset happens only when the host starts a new call. */
        luaL_error(Ls, "instruction budget exhausted");
    }
    budget_left--;
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
    return 1;
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
    static const luaL_Reg flight_fns[] = {
        {"state", l_flight_state}, {"time_ms", l_flight_time}, {"max_altitude_cm", l_flight_max_alt}, {NULL, NULL}};
    static const luaL_Reg pyro_fns[] = {{"status", l_pyro_status}, {NULL, NULL}};

    if (lua_plat_output_count() > 0)
        reg_table(Ls, "output", output_fns);
    if (lua_plat_input_count() > 0)
        reg_table(Ls, "input", input_fns);
    if (lua_plat_serial_count() > 0)
        reg_table(Ls, "serial", serial_fns);

    /* Always present: reading state grants nothing and costs nothing. */
    reg_table(Ls, "sensor", sensor_fns);
    reg_table(Ls, "flight", flight_fns);
    reg_table(Ls, "pyro", pyro_fns);

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
    have_tick = false;

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

static bool run_protected(int nargs) {
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
    if (luaL_loadbuffer(L, src, len, chunkname) != LUA_OK) {
        const char *m = lua_tostring(L, -1);
        snprintf(last_error, sizeof(last_error), "%s", m ? m : "syntax error");
        lua_pop(L, 1);
        return false;
    }
    if (!run_protected(0))
        return false;

    lua_getglobal(L, "tick");
    have_tick = lua_isfunction(L, -1);
    lua_pop(L, 1);

    lua_getglobal(L, "init");
    if (lua_isfunction(L, -1))
        return run_protected(0);
    lua_pop(L, 1);
    return true;
}

bool pyro_lua_tick(void) {
    if (!L || !have_tick)
        return true;
    lua_getglobal(L, "tick");
    return run_protected(0);
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
    if (luaL_loadbuffer(L, src, strlen(src), "=console") != LUA_OK) {
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
