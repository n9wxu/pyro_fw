/*
 * Lua build configuration, injected as LUA_USER_H.
 *
 * lua.h includes this at its end, after luaconf.h has fixed the number types,
 * so nothing here can select them: src/lua/lua_patch.cmake sets LUA_32BITS in
 * luaconf.h itself. What this header can set are the macros Lua reads where
 * they are used rather than where they are defined.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_LUACONF_H
#define PYRO_LUACONF_H

#if LUA_FLOAT_TYPE != LUA_FLOAT_FLOAT || LUA_INT_TYPE != LUA_INT_INT
#error "luaconf.h is not the 32-bit configuration: apply src/lua/lua_patch.cmake to the Lua sources"
#endif

/* Nested C calls and parser levels a script may reach. Each costs the Lua
 * task's stack, so this is sized from it (lua_core1.c C1_STACK_WORDS), and
 * Lua allows LUAI_MAXCCALLS / 10 * 11 while handling the overflow error.
 * Measured: a pcall level's frames sum to 520 B on Cortex-M0+ at -Os
 * (-fstack-usage), 816 B on x86-64, where stack painting gives 800 B. */
#define PYRO_LUA_C_LEVELS 20
#define LUAI_MAXCCALLS PYRO_LUA_C_LEVELS

/* lstrlib.c's matcher recursion depth -- one level per quantifier or capture
 * in a pattern, 40 B each on Cortex-M0+ -- also on the Lua task's stack. */
#define MAXCCALLS 32

/* luaconf.h parses decimals with strtof, which in newlib allocates from the
 * system heap; Lua must not (lua_arena.c). lobject.c's own hex parser takes
 * over once lua_strx2number is undefined. */
struct lua_State;
float pyro_lua_str2number(const char *s, char **endptr);
#undef lua_str2number
#define lua_str2number(s, p) pyro_lua_str2number((s), (p))
#undef lua_strx2number

/* Charges the pattern matcher's work to the same limits as VM instructions.
 * lua_patch.cmake inserts the call at the top of lstrlib.c's match(). */
void pyro_lua_match_step(struct lua_State *L);
#define luai_matchstep(L) pyro_lua_match_step(L)

#endif
