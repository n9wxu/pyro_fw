/*
 * HTTP work units, and who runs them.
 *
 * The transport only moves bytes between lwIP and each connection's rings.
 * Everything else a connection needs -- parsing, routing, rendering, reading
 * a file, writing flash -- is a unit: one bounded step on one connection.
 * Core0 runs units from the loop's slack, after the flight work, never from
 * the loop's head. A portable unit touches nothing but its own connection:
 * one core0 had no room for, core1 runs with its next grant, before its Lua
 * slice. See DD-061.
 *
 * Ownership is whole-connection and changes hands only on core0: core0 hands
 * units over before it grants core1 its slice, and takes them back once
 * core1 is idle again. While core1 holds a connection, core0 leaves it
 * alone, so the rings need no cross-core protocol.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_WORK_H
#define HTTP_WORK_H

#include <stdbool.h>
#include <stdint.h>

#define HTTP_WORK_SLOTS 4
#define HTTP_UNIT_NONE 0xFFu

/* What one unit may cost. Core0 starts one only with this much of the period
 * left, except the first of each period, so HTTP progresses under any load.
 * The /api/status render, the costliest unit that writes no flash, measured
 * 1.6 ms on MK1C. */
#define HTTP_UNIT_BUDGET_US 2000u

/* The same unit on core1, which fetches from flash while core0 does: 3.8 ms,
 * measured under G4. What the claim charges a grant per unit. */
#define HTTP_WORKER_UNIT_US 4000u

/* Of a core1 grant, what units may not take: the Lua slice's floor. */
#define HTTP_WORKER_LUA_MIN_US 1000u

enum { HTTP_ON_CORE0 = 0, HTTP_ON_WORKER = 1 };

/* The portable units, indexed by the unit numbers offered. Named *_vt so
 * support/prove_core0.py folds every entry into core1's call graph. */
typedef void (*http_unit_fn)(int slot);
extern const http_unit_fn http_unit_vt[];

/* The platform's microsecond clock, for the cost statistics. */
extern uint32_t http_work_clock_us(void);

void http_work_reset(void);

/* ── core0 ────────────────────────────────────────────────────────── */

/* A period's slack begins. */
void http_work_period(void);

/* The slot's next step is the portable unit `unit`. Ignored while the slot
 * already has one pending or held. */
void http_work_offer(int slot, uint8_t unit);

/* Drops a pending unit. False while the worker holds the slot. */
bool http_work_cancel(int slot);

bool http_work_pending(int slot);
bool http_work_held(int slot);

/* The slot core0 runs next, in turn, or -1. runnable[i]: the connection has
 * a step for core0. *unit is the portable unit to run, or HTTP_UNIT_NONE for
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

/* ── the worker (core1) ───────────────────────────────────────────── */

/* Runs every unit it was handed. */
void http_work_run(void);

/* ── statistics ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t units[2]; /* HTTP_ON_CORE0, HTTP_ON_WORKER */
    uint32_t max_us[2];
} http_work_stats_t;

void http_work_stats(http_work_stats_t *out);

#endif
