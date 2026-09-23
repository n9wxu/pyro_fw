/*
 * Lua on core1 — lifecycle, dispatch, and the flight snapshot.
 *
 * This module exists to make one sentence true:
 *
 *     core0 must never wait on anything core1 can hold.
 *
 * docs/core1_hazard.md shows why that is not free. Three mechanisms
 * discharge it:
 *
 *   1. Core1 acquires no shared resource. Core0 claims every PIO state
 *      machine, program offset and DMA channel at boot, and core1 writes
 *      only the registers core0 handed it. Core1 calls no hw_claim,
 *      allocates nothing from the system heap -- Lua has its own arena --
 *      and never touches flash.
 *
 *   2. Core0 dispatches work rather than asking permission. Core1 idles in
 *      a RAM-resident loop and executes only what core0 hands it, one
 *      time-boxed unit per grant. Core0 answers "is core1 in flash" from a
 *      flag it owns, so there is no request, ack or deadline to wait on.
 *
 *   3. The kill is unilateral and terminal. PSM frce_off needs no consent.
 *      Do not use the SDK's multicore_reset_core1(): it ends in
 *      multicore_fifo_pop_blocking(), so the recovery path would itself hang
 *      core0 when a resurrected core1 failed to answer. Core1 stays in reset
 *      until the next power cycle.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_CORE1_H
#define LUA_CORE1_H

#include <stdbool.h>
#include <stdint.h>

/* Core0 publishes and core1 reads, through a seqlock, so core0 never waits
 * and a core1 that keeps losing the race pays for it alone. */
typedef struct {
    int32_t pressure_pa;
    int32_t altitude_cm;
    int32_t speed_cms;
    int32_t max_alt_cm;
    int state;
    uint32_t time_ms;
    int pyro[2];
    /* The booleans in pyro[] round a degraded connector up to "good"; only
     * the raw count shows the difference. */
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

/* Call once, at boot, before the flight loop. The launch uses a FIFO
 * handshake with no timeout, which is acceptable only against a cold core1
 * in the bootrom. Do not add a relaunch path. */
bool lua_core1_start(const char *script, int len);

lua_c1_state_t lua_core1_state(void);
const char *lua_core1_error(void);
uint32_t lua_core1_heartbeat(void);

/* ── Dispatch ─────────────────────────────────────────────────────
 *
 * Core1 idles in a RAM-resident spin and executes only what core0 hands it,
 * one time-boxed unit per grant. That is what makes flash safe: core1
 * executes from flash only while working, and core0 started that work.
 *
 * One ordering rule holds it up. Core1 publishes "idle" from inside the
 * RAM-resident function, so by the time core0 observes it core1's program
 * counter is already in RAM. */

/* Never blocks, and does nothing while core1 is still working. Size the
 * grant to leave slack: an overrun costs a skipped dispatch, not a stall. */
void lua_core1_dispatch(uint32_t budget_us);

/* ── Startup ──────────────────────────────────────────────────────
 *
 * Core1's startup -- creating the VM, compiling the script, running init() --
 * runs unbounded, because init() is user code and any limit would be
 * arbitrary or leave a script half-run.
 *
 * That is safe only because core0 writes no flash for the same stretch, and
 * nothing needs flash there: logging starts at launch, an operator initiates
 * uploads and OTA, and the filesystem is already mounted. */

/* False while core1 is in startup or mid-unit: core0 must not touch flash. */
bool lua_core1_flash_ok(void);

/* The startup beep waits on this, so a board that beeps is running a script
 * rather than compiling one. */
bool lua_core1_ready(void);

/* Unilateral, terminal. Safe to call at any time from core0. */
void lua_core1_kill(void);

/* Never blocks; drops the event while core1 holds an unconsumed one. */
void lua_core1_event(const char *name);

/* Drain whatever core1 printed, for the web console. Returns bytes copied. */
int lua_core1_console_read(char *buf, int max);

/* Core1 must never touch flash, so it hands log bytes over and core0 owns
 * the write. Returns bytes copied. */
int lua_core1_log_read(char *buf, int max);

/* Core0 not draining fast enough. Core1 must never block, so dropping is
 * correct; /api/lua/console reports these so it is not silent. */
uint32_t lua_core1_console_dropped(void);
uint32_t lua_core1_log_dropped(void);

/* Watches the heartbeat and kills a wedged core1. Never blocks. */
void lua_core1_service(uint32_t now_ms);

/* Core1 runs on its own stack in bss, not the SDK's 2 kB default in
 * SCRATCH_X, which overflows downward into core0's stack. */
bool lua_core1_stack_ok(void);
uint32_t lua_core1_stack_free(void);
void lua_core1_check_stack(void);

/* For diagnosing a kill. */
void lua_core1_dispatch_stats(uint32_t *handed, uint32_t *taken, uint32_t *skipped, uint32_t *heartbeat);

/* Byte 0 = where core1 is, byte 1 = executing from flash, byte 2 = low bits
 * of the dispatch counter. */
uint32_t lua_core1_loc(void);

#endif
