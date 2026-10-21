/*
 * HTTP work units, and their turn.
 *
 * The transport only moves bytes between the stack and each connection's rings.
 * Everything else a connection needs -- parsing, routing, rendering, reading
 * a file, writing flash -- is a unit: one bounded step on one connection.
 * The net task runs them, a few between transport passes, each connection in
 * turn (net_task.c, http_server_work()). See DD-073.
 *
 * A portable unit touches nothing but its own connection, so a worker may run
 * it. Ownership is whole-connection and changes hands only in the task that
 * runs units: while the worker holds a connection that task leaves it alone,
 * so the rings need no cross-task protocol. The firmware claims nothing for a
 * worker; http_work_reclaim() keeps a connection from staying held.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_WORK_H
#define HTTP_WORK_H

#include <stdbool.h>
#include <stdint.h>

#define HTTP_WORK_SLOTS 4
#define HTTP_UNIT_NONE 0xFFu

/* What one unit may cost. After a period's first unit, one starts only with
 * this much of remaining_us left, so HTTP progresses under any load.
 * The /api/status render, the costliest unit that writes no flash, measured
 * 1.6 ms on MK1C. */
#define HTTP_UNIT_BUDGET_US 2000u

/* What http_work_claim() charges a grant per unit: the same render on core1
 * while core0 also fetches from flash, 3.8 ms, measured under G4. */
#define HTTP_WORKER_UNIT_US 4000u

/* Of a grant, what claimed units may not take: the worker's own floor. */
#define HTTP_WORKER_LUA_MIN_US 1000u

enum { HTTP_ON_CORE0 = 0, HTTP_ON_WORKER = 1 };

/* The portable units, indexed by the unit numbers offered. Named *_vt so
 * support/prove_core0.py folds every entry into core1's call graph. */
typedef void (*http_unit_fn)(int slot);
extern const http_unit_fn http_unit_vt[];

/* The platform's microsecond clock, for the cost statistics. */
extern uint32_t http_work_clock_us(void);

void http_work_reset(void);

/* ── the task that runs units ─────────────────────────────────────── */

/* A period begins: its first unit runs whatever remaining_us. */
void http_work_period(void);

/* The slot's next step is the portable unit `unit`. Ignored while the slot
 * already has one pending or held. */
void http_work_offer(int slot, uint8_t unit);

/* Drops a pending unit. False while the worker holds the slot. */
bool http_work_cancel(int slot);

bool http_work_pending(int slot);
bool http_work_held(int slot);

/* The slot to run next, in turn, or -1. runnable[i]: the connection has a
 * step of its own. *unit is the portable unit to run, or HTTP_UNIT_NONE for
 * the connection's own step. */
int http_work_next(const bool runnable[HTTP_WORK_SLOTS], int32_t remaining_us, uint8_t *unit);

/* Hands the worker the pending units that fit a grant of grant_us. Call
 * before the grant is given. Returns how many. */
int http_work_claim(uint32_t grant_us);

/* Takes the worker's slots back once worker_idle. Returns the slots
 * returned; *lost gets those whose unit was cut short, whose connection is
 * in an unknown state. A claim the worker never started is pending again. */
uint8_t http_work_reclaim(bool worker_idle, uint8_t *lost);

void http_work_note(int who, uint32_t us);

/* ── the worker ───────────────────────────────────────────────────── */

/* Runs every unit it was handed. */
void http_work_run(void);

/* ── statistics ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t units[2]; /* HTTP_ON_CORE0, HTTP_ON_WORKER */
    uint32_t max_us[2];
} http_work_stats_t;

void http_work_stats(http_work_stats_t *out);

#endif
