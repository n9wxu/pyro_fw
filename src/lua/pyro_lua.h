/*
 * Lua user programs — host interface.
 *
 * Everything a script can reach is described by src/lua/lua_platform.h; this
 * header is how the application drives the VM. See pyro_lua.c for the
 * invariants implemented.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_LUA_H
#define PYRO_LUA_H

#include <stdbool.h>
#include <stddef.h>
#include "lua_arena.h"

/* Create the VM and build the environment from what the platform offers.
 * Safe to call again after shutdown; each call starts clean. */
void pyro_lua_init(void);
void pyro_lua_shutdown(void);

/* Load and run a chunk, then call its init() if it defines one. A chunk that
 * defines tick() will have it called by pyro_lua_tick(). Returns false and
 * sets last_error on a syntax error, a runtime error, or budget exhaustion. */
bool pyro_lua_load(const char *chunkname, const char *src, size_t len);

/* Call tick() if the loaded chunk defined one. */
/* Outcome of one dispatched work unit. */
typedef enum {
    PYRO_LUA_DONE = 0, /* tick() ran to completion                       */
    PYRO_LUA_YIELD,    /* time box expired mid-call; resume next grant   */
    PYRO_LUA_ERROR,    /* script raised; pyro_lua_last_error() has it    */
} pyro_lua_status_t;

/* Run up to budget_us of the script's tick(), then yield.
 *
 * The script does NOT restart on the next call -- it resumes exactly where
 * the time box stopped it, because tick() runs inside a coroutine and the
 * instruction hook yields rather than erroring. So "while true do ... end" is
 * a legitimate program, not a runaway to be killed.
 *
 * A grant of 0 means run to completion, which is what the host tests want and
 * what a caller with no deadline should ask for. */
pyro_lua_status_t pyro_lua_tick_slice(uint32_t budget_us);

/* Run one whole tick(), no time box. Equivalent to pyro_lua_tick_slice(0). */
bool pyro_lua_tick(void);

/* Call on_event(name) if the loaded chunk defined one. Event names match the
 * flight EVT_* set: "LAUNCH", "ARMED", "APOGEE", "PYRO1_FIRE", "LANDING". */
bool pyro_lua_event(const char *event_name, uint32_t budget_us);

/* Evaluate one console line in the live VM. Errors go to the console rather
 * than killing the loaded program. */
bool pyro_lua_eval(const char *src);

const char *pyro_lua_last_error(void);
void pyro_lua_get_stats(lua_arena_stats_t *out);

#endif
