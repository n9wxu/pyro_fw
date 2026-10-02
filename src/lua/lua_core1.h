/*
 * Lua on core1 -- lifecycle, ticks, events and the flight snapshot.
 *
 * The Lua task runs pinned to core1 at P, beside the net and storage tasks
 * [DD-073]. This module keeps one sentence true:
 *
 *     the flight task never waits on anything the Lua task can hold.
 *
 * Three things discharge it:
 *
 *   1. The Lua task acquires no shared resource. The flight task claims every
 *      pad, PIO state machine, program offset and DMA channel at boot, and
 *      the task writes only the registers it was handed. It allocates nothing
 *      from the system heap -- Lua has its own arena -- and touches no file.
 *
 *   2. The flight task asks, never waits. A tick is requested with a
 *      notification that does not block; a tick still running when the next
 *      is due skips it. Events and the flight snapshot go one way through
 *      structures the flight task writes without waiting, and console and log
 *      text come back through rings it drains without waiting.
 *
 *   3. The stop is unilateral: vTaskSuspend() needs no consent, and what the
 *      VM drove is put down after it. Flash does not depend on where the Lua
 *      task is: every flash operation parks the other core itself [DD-074].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_CORE1_H
#define LUA_CORE1_H

#include <stdbool.h>
#include <stdint.h>

/* Published by the flight task, read by the Lua task, through a seqlock: the
 * writer never waits, and a reader that keeps losing the race pays for it
 * alone. */
typedef struct {
    int32_t pressure_pa;
    int32_t altitude_cm;
    int32_t speed_cms;
    int32_t max_alt_cm;
    int state; /* flight_state_t */
    uint32_t time_ms;
    int pyro[2];
    /* The booleans in pyro[] round a degraded connector up to "good"; only
     * the raw count shows the difference. */
    int pyro_adc[2];
    int under_thrust;
    int apogee_detected;
    uint32_t telem_seq;
} lua_flight_t;

/* The Lua task's side: the last consistent snapshot. */
const lua_flight_t *lua_flight_snapshot(void);

/* The flight task's side. Never blocks. */
void lua_core1_publish(const lua_flight_t *f);

/* ── Lifecycle ────────────────────────────────────────────────────── */

/* Written only by the flight task. The values reach /api/lua/console. */
typedef enum {
    LUA_C1_OFF = 0,     /* never started, or disabled by configuration */
    LUA_C1_RUNNING = 1, /* started                                     */
    LUA_C1_DEAD = 3,    /* stopped; stays stopped until reboot         */
} lua_c1_state_t;

/* Before the scheduler starts: the task, waiting for lua_core1_start(). */
void lua_task_create(void);

/* Once. Starts the script in the waiting task. Do not add a relaunch path:
 * a VM stopped mid-tick is not resumable. */
bool lua_core1_start(const char *script, int len);

lua_c1_state_t lua_core1_state(void);
const char *lua_core1_error(void);
uint32_t lua_core1_heartbeat(void);

/* Why the task refused to run the script -- the start-up check's first
 * finding -- or NULL. The Lua task checks the script against the bound
 * resources before loading it, on its own stack, which is sized for the
 * parser. */
const char *lua_core1_refusal(void);

/* ── Ticks ────────────────────────────────────────────────────────
 *
 * The flight task asks for one tick() a period. Never blocks, and does
 * nothing while the last tick is still running: a skipped tick is counted,
 * not queued. budget_us is the tick's time box, events included. */
void lua_core1_dispatch(uint32_t budget_us);

/* The startup beep waits on this, so a board that beeps is running a script
 * rather than compiling one. */
bool lua_core1_ready(void);

/* Unilateral, terminal. Safe to call at any time from the flight task. */
void lua_core1_kill(void);

/* A flight event for on_event(), delivered before the next tick. Never
 * blocks; dropped and counted when the queue is full or Lua is not running. */
void lua_core1_event(const char *name);

/* Drain whatever the script printed, for the web console. Returns bytes copied. */
int lua_core1_console_read(char *buf, int max);

/* The script's log() text, which the flight task writes into the flight log.
 * Returns bytes copied. */
int lua_core1_log_read(char *buf, int max);

/* Not drained fast enough. The script must never block, so dropping is
 * correct; /api/lua/console reports these so it is not silent. */
uint32_t lua_core1_console_dropped(void);
uint32_t lua_core1_log_dropped(void);

/* Watches the heartbeat and stops a wedged task. Never blocks. */
void lua_core1_service(uint32_t now_ms);

/* The task's stack, from the kernel's high-water mark: bytes never used, and
 * a stop when C1_STACK_MARGIN_WORDS or fewer are left. */
uint32_t lua_core1_stack_free(void);
void lua_core1_check_stack(void);

/* Ticks asked, ticks done, ticks skipped, heartbeat: for diagnosing a stop. */
void lua_core1_dispatch_stats(uint32_t *handed, uint32_t *taken, uint32_t *skipped, uint32_t *heartbeat);

/* Byte 0 = where the task is, byte 1 = mid-tick, byte 2 = low bits of the
 * ticks asked. */
uint32_t lua_core1_loc(void);

#endif
