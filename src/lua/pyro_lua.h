/*
 * Lua user programs: how the application drives the VM.
 *
 * Everything a script can reach is described by lua_platform.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_LUA_H
#define PYRO_LUA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lua_arena.h"
#include "lua_check.h"

/* Create the VM and build the environment from what the platform offers.
 * Safe to call again after shutdown; each call starts clean. */
void pyro_lua_init(void);
void pyro_lua_shutdown(void);

/* Load and run a chunk, then call its init() if it defines one. Returns false
 * and sets last_error on a syntax error, a runtime error or a limit; a
 * program that failed to start gets no tick() until a load succeeds. */
bool pyro_lua_load(const char *chunkname, const char *src, size_t len);

typedef enum {
    PYRO_LUA_DONE = 0, /* tick() ran to completion, or there is none */
    PYRO_LUA_YIELD,    /* time box expired mid-call; resume next grant */
    PYRO_LUA_ERROR,    /* script raised; pyro_lua_last_error() has it */
} pyro_lua_status_t;

/* Run up to budget_us of the script's tick(), then suspend it.
 *
 * The next call resumes where the box stopped it, so "while true do ... end"
 * is a legitimate program. Where tick() cannot be suspended -- inside a
 * comparator, a metamethod or a pattern match -- it may overrun its box by
 * PYRO_LUA_OVERRUN_US and is then stopped with an error.
 *
 * A grant of 0 runs to completion under the instruction budget instead. */
pyro_lua_status_t pyro_lua_tick_slice(uint32_t budget_us);

/* pyro_lua_tick_slice(0). */
bool pyro_lua_tick(void);

/* Call on_event(name) if the loaded chunk defined one. budget_us = 0: the
 * instruction budget only. */
bool pyro_lua_event(const char *event_name, uint32_t budget_us);

/* The flight task's side of on_event(): queue an event name for the VM's task.
 * Never blocks; drops and counts when the queue is full. Names are
 * flight_event_name()'s ("LAUNCH", "APOGEE", "PYRO1", "LANDING", ...). */
bool pyro_lua_post_event(const char *name);
uint32_t pyro_lua_events_dropped(void);

/* The VM's task: deliver every queued event, in order, within budget_us
 * altogether (0: the instruction budget per event). */
void pyro_lua_run_events(uint32_t budget_us);

/* Evaluate one console line in the live VM. Errors go to the console rather
 * than killing the loaded program. */
bool pyro_lua_eval(const char *src);

const char *pyro_lua_last_error(void);
void pyro_lua_get_stats(lua_arena_stats_t *out);

/* lua_check() in the VM's arena, for the Lua task's start-up check. Only
 * while no VM exists; returns false otherwise. */
bool pyro_lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out);

#endif
