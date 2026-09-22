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
    /* Everything the built-in telemetry formatter emits, so a script can
     * produce the same sentences rather than a subset of them. The raw ADC
     * counts are the point: the booleans in pyro[] round a degraded connector
     * to "good", and only the count shows it. */
    int pyro_adc[2];
    int under_thrust;
    int apogee_detected;
    uint32_t telem_seq;
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

/* ── Dispatch ─────────────────────────────────────────────────────
 *
 * Core1 is a worker, not a free-running loop. It idles in a RAM-resident spin
 * and executes only the work core0 hands it, one time-boxed unit per grant.
 *
 * That is what makes flash safe, and it replaces asking. Core1 executes from
 * flash only while it is working, core0 is the one that started that work, so
 * "is core1 in flash right now" is a question core0 can answer from a flag it
 * can see -- no request, no ack, no deadline, no kill on timeout.
 *
 * The one ordering rule that survives from the old park protocol: core1
 * publishes "idle" from INSIDE the RAM-resident function. Once core0 observes
 * it, core1's program counter is already in RAM, so there is no window where
 * core0 believes core1 is idle while it is still fetching from XIP. */

/* Hand core1 one work unit of at most budget_us. Never blocks. Does nothing
 * if core1 is still working, which is why the grant should leave slack: a
 * unit that overruns costs a skipped dispatch, not a stall. */
void lua_core1_dispatch(uint32_t budget_us);

/* Grants dropped because core1 was still on the previous unit. */
uint32_t lua_core1_dispatch_skipped(void);

/* True when core1 is idling in RAM and flash is safe to erase or program.
 * Also true when core1 is off or dead -- there is nothing to collide with. */
bool lua_core1_idle(void);

/* ── Startup ──────────────────────────────────────────────────────
 *
 * Core1's one-time startup -- creating the VM, compiling the script, running
 * init() -- runs UNBOUNDED. It has to: init() is user code, it may parse
 * strings or build tables, and the alternatives are a limit that is arbitrary
 * or a script that half-runs.
 *
 * Unbounded is safe here only because core0 writes no flash until core1 is
 * ready. That is the whole trade: the startup window is the one time core1
 * executes from flash for an unpredictable duration, so core0 gives up flash
 * for the same window rather than trying to interrupt it.
 *
 * Nothing needs flash in that window. Logging starts at launch, uploads and
 * OTA are operator-initiated, and the filesystem is already mounted. */

/* False while core1 is in startup or mid-unit: core0 must not touch flash. */
bool lua_core1_flash_ok(void);

/* True once core1 has finished startup and reached the dispatch loop. This is
 * what the heartbeat and the startup beep wait for, so a board that is
 * blinking and has beeped has a running script rather than a compiling one. */
bool lua_core1_ready(void);

/* Unilateral, terminal. Safe to call at any time from core0. */
void lua_core1_kill(void);

/* Deliver a flight event. Non-blocking: drops if core1 has not consumed the
 * previous one. */
void lua_core1_event(const char *name);

/* Drain whatever core1 printed, for the web console. Returns bytes copied. */
int lua_core1_console_read(char *buf, int max);

/* Drain whatever core1 asked to be logged, for core0 to put in a file.
 * Core1 must never touch flash, so it hands bytes over and core0 owns the
 * write; this is the same single-producer ring as the console, pointed at a
 * different consumer. Returns bytes copied. */
int lua_core1_log_read(char *buf, int max);

/* Bytes the two rings had to drop because core0 was not draining fast enough.
 * Dropping is correct -- core1 must never block -- but silent dropping is
 * not, so these are reported on /api/lua/console. */
uint32_t lua_core1_console_dropped(void);
uint32_t lua_core1_log_dropped(void);

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

/* Dispatch accounting, for diagnosing a kill: units handed out, units taken,
 * grants skipped because core1 had not finished, and the heartbeat. */
void lua_core1_park_stats(uint32_t *ok, uint32_t *req, uint32_t *ack, uint32_t *hb);

/* Packed: byte 0 = where core1 is now, byte 1 = whether it is executing from
 * flash, byte 2 = the low bits of the dispatch counter. */
uint32_t lua_core1_loc(void);

#endif
