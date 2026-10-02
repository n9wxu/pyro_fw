/*
 * HTTP work units, and their turn [WEB-HTTP-06, DD-061, DD-073].
 *
 * The transport only moves bytes between lwIP and each connection's rings.
 * Everything else a connection needs -- parsing, routing, rendering, reading
 * a file, writing flash -- is a unit: one bounded step on one connection,
 * run by the net task. Connections take turns, and a unit starts only with
 * its budget left in the period, except the first of each period, so HTTP
 * progresses under any load.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_WORK_H
#define HTTP_WORK_H

#include <stdbool.h>
#include <stdint.h>

#define HTTP_WORK_SLOTS 4
#define HTTP_UNIT_NONE 0xFFu

/* The /api/status render, the costliest unit that writes no flash, measured
 * 1.6 ms on MK1C. */
#define HTTP_UNIT_BUDGET_US 2000u

/* The units offered, indexed by unit number. Named *_vt because
 * support/prove_core0.py follows calls through *_vt tables. */
typedef void (*http_unit_fn)(int slot);
extern const http_unit_fn http_unit_vt[];

void http_work_reset(void);

/* A period's slack begins. */
void http_work_period(void);

/* The slot's next step is the unit `unit`. Ignored while one is pending. */
void http_work_offer(int slot, uint8_t unit);

/* Drops a pending unit. */
void http_work_cancel(int slot);

bool http_work_pending(int slot);

/* The slot to run next, in turn, or -1. runnable[i]: the connection has a
 * step of its own. *unit is the offered unit to run, or HTTP_UNIT_NONE for
 * the connection's own step. */
int http_work_next(const bool runnable[HTTP_WORK_SLOTS], int32_t remaining_us, uint8_t *unit);

/* What a unit cost, for the statistics. */
void http_work_note(uint32_t us);

typedef struct {
    uint32_t units;
    uint32_t max_us;
} http_work_stats_t;

void http_work_stats(http_work_stats_t *out);

#endif
