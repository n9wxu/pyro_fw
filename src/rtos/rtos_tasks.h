/*
 * The tasks, their cores and priorities [DD-073]. Plan 2's table, as built:
 *
 *   task            core  prio  does
 *   lockout helper  0, 1  T     parks its core for a flash operation (flash_op.h)
 *   flight          0     P     the 20 ms step, woken by a hardware-timer alarm
 *   net             1     P     TinyUSB, lwIP, HTTP
 *   lua             1     P     the VM (lua_core1.h)
 *   storage         1     P     the flight log, queued file work
 *   timer daemon    1     P     the SDK's interop
 *
 * Mode 0 (configRUN_MULTIPLE_PRIORITIES 0): while anything runs at T, nothing
 * at P runs on either core. The flight task is the only task at P on core0,
 * so nothing it shares a priority with can hold core0 when it wakes.
 *
 * The flight task never waits on anything another task can hold. Its only
 * blocking call is the wait for its next period; everything it hands over
 * goes through a ring or a notification, neither of which waits.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RTOS_TASKS_H
#define RTOS_TASKS_H

#include <stdbool.h>
#include <stdint.h>

#define PRIO_P 1u
#define PRIO_T 2u /* configMAX_PRIORITIES - 1, checked in rtos_tasks.c */

#define CORE0_ONLY (1u << 0)
#define CORE1_ONLY (1u << 1)

/* In words. /api/status reports what each task has never used, so these are
 * checked on the bench rather than trusted; Lua keeps core1's 12 kB. */
#define FLIGHT_STACK_WORDS 1024u
/* The net and storage tasks can both run pico_fota_bootloader's commit,
 * which copies a whole 4 kB flash sector onto the caller's stack. */
#define NET_STACK_WORDS 2048u
#define STORAGE_STACK_WORDS 2048u

struct flight_context_t;

/* Creates every task and starts the scheduler. Never returns. */
void rtos_start(struct flight_context_t *ctx);

/* True once the scheduler runs; before it, only core0 exists and anything
 * may touch flash directly. */
bool rtos_running(void);

/* True on the flight task, which may not block, touch a file or hold a lock. */
bool rtos_in_flight_task(void);

/* ── The flight task's mailbox ────────────────────────────────────
 *
 * State the flight task owns -- the running config, test mode, the buzzer --
 * is changed only by the flight task. Another task hands it a function to
 * run at the head of its next period and waits, at most timeout_ms, for it
 * to have run. False: it did not run and will not. */
typedef void (*flight_call_fn)(void *arg);
bool flight_call(flight_call_fn fn, void *arg, uint32_t timeout_ms);
void flight_call_service(void); /* the flight task only */

/* ── Hand-offs from the flight task: never block ──────────────────── */
void rtos_notify_storage(void);
void rtos_notify_lua(void);

/* The storage task checks in every pass. On the ground a storage task that
 * stops checking in lets the watchdog reset the board; in flight it is
 * counted and the flight goes on. */
void storage_checkin(uint32_t now_ms);
bool storage_alive(uint32_t now_ms);
uint32_t storage_stalls(void);

/* Stack high-water marks, bytes never used, for /api/status. */
typedef struct {
    const char *name;
    uint32_t free_bytes;
} rtos_task_stat_t;
int rtos_task_stats(rtos_task_stat_t *out, int max);

#endif
