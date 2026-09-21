/*
 * Lua on core1 — lifecycle, the park protocol, and the flight snapshot.
 *
 * The whole of this module exists to make one sentence true:
 *
 *     core0 must never wait on anything core1 can hold.
 *
 * docs/core1_hazard.md proves why that is not free. Three mechanisms here
 * discharge it:
 *
 *   1. core1 never acquires a shared resource. Every PIO state machine,
 *      program offset and DMA channel is claimed on core0 at boot; core1 only
 *      writes registers it was handed. It never calls hw_claim, never
 *      allocates from the system heap (Lua has its own arena), and never
 *      touches the flash.
 *
 *   2. Flash writes park core1 first, with a deadline. Core0 asks; core1
 *      answers from a RAM-resident spin loop. If the answer does not come in
 *      time core0 kills core1 and proceeds. Core0's wait is bounded by the
 *      deadline, not by core1's cooperation.
 *
 *   3. The kill is unilateral and terminal. PSM frce_off needs no consent.
 *      The SDK's own multicore_reset_core1() is NOT used: it ends in
 *      multicore_fifo_pop_blocking(), so the SDK's recovery path would itself
 *      hang core0 if the resurrected core1 failed to answer. Core1 is left in
 *      reset until the next power cycle.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_CORE1_H
#define LUA_CORE1_H

#include <stdbool.h>
#include <stdint.h>

/* ── Flight state handed to core1 ─────────────────────────────────
 *
 * Core0 publishes; core1 reads. Written with a seqlock so core1 never sees a
 * half-updated snapshot, and core0 never waits: the writer increments a
 * counter before and after, and the reader retries. A reader that keeps
 * losing the race is core1's problem, not core0's. */
typedef struct {
    int32_t pressure_pa;
    int32_t altitude_cm;
    int32_t speed_cms;
    int32_t max_alt_cm;
    int state;
    uint32_t time_ms;
    int pyro[2];
} lua_flight_t;

/* core1 side: a stable snapshot. */
const lua_flight_t *lua_flight_snapshot(void);

/* core0 side: publish the current flight state. Never blocks. */
void lua_core1_publish(const lua_flight_t *f);

/* ── Lifecycle ────────────────────────────────────────────────────── */

typedef enum {
    LUA_C1_OFF = 0, /* never started, or disabled by configuration */
    LUA_C1_RUNNING, /* VM live                                     */
    LUA_C1_PARKED,  /* spinning in RAM, flash safe                 */
    LUA_C1_DEAD,    /* killed; stays dead until reboot             */
} lua_c1_state_t;

/* Launch core1 with the VM. Call once, at boot, before the flight loop.
 * Launching uses a FIFO handshake that has no timeout, which is acceptable
 * exactly once from a cold core1 in the bootrom and never again -- there is
 * deliberately no relaunch. */
bool lua_core1_start(const char *script, int len);

lua_c1_state_t lua_core1_state(void);
const char *lua_core1_error(void);
uint32_t lua_core1_heartbeat(void);

/* Park core1 outside flash. Returns true if core1 is parked (or was never
 * running); false if it had to be killed to meet the deadline. Either way
 * the caller may proceed to touch flash. */
bool lua_core1_park(uint32_t timeout_us);
void lua_core1_unpark(void);

/* Unilateral, terminal. Safe to call at any time from core0. */
void lua_core1_kill(void);

/* Deliver a flight event. Non-blocking: drops if core1 has not consumed the
 * previous one. */
void lua_core1_event(const char *name);

/* Drain whatever core1 printed, for the web console. Returns bytes copied. */
int lua_core1_console_read(char *buf, int max);

/* core0 housekeeping: watches the heartbeat and kills a wedged core1.
 * Called from the main loop; never blocks. */
void lua_core1_service(uint32_t now_ms);

/* Stack guard. core1 runs on its own stack in bss rather than the SDK's
 * 2 kB default in SCRATCH_X, because that one overflows downward into
 * core0's stack -- a core1 fault that takes out core0 is precisely what this
 * module exists to prevent. */
bool lua_core1_stack_ok(void);
uint32_t lua_core1_stack_free(void);
void lua_core1_check_stack(void);

/* Park accounting, for diagnosing a kill: how many parks succeeded, and the
 * request/ack/heartbeat values at the one that did not. */
void lua_core1_park_stats(uint32_t *ok, uint32_t *req, uint32_t *ack, uint32_t *hb);

/* Packed: byte 0 = where core1 is now, byte 1 = where it was when a park
 * went unanswered, byte 2 = the live park request counter. */
uint32_t lua_core1_loc(void);

#endif
