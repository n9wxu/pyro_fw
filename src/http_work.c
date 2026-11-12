/*
 * SPDX-License-Identifier: MIT
 */
#include "http_work.h"
#include <string.h>

#if defined(__arm__)
#define WORK_FENCE() __asm volatile("dmb" ::: "memory")
#else
#define WORK_FENCE() __sync_synchronize()
#endif

/* core0 only. */
static uint8_t pending_unit[HTTP_WORK_SLOTS];
static bool ran_this_period;
static int turn;

/* core0 writes these before a grant and reads them back after it. */
static volatile bool slot_held[HTTP_WORK_SLOTS];
static volatile uint8_t held_unit[HTTP_WORK_SLOTS];
/* The worker's side of the handover. */
static volatile bool slot_started[HTTP_WORK_SLOTS];
static volatile bool done[HTTP_WORK_SLOTS];

static volatile uint32_t stat_units[2];
static volatile uint32_t stat_max_us[2];

void http_work_reset(void) {
    (void)memset(pending_unit, HTTP_UNIT_NONE, sizeof(pending_unit));
    ran_this_period = false;
    turn = 0;
    for (int i = 0; i < HTTP_WORK_SLOTS; i++) {
        done[i] = false;
        slot_started[i] = false;
        slot_held[i] = false;
        held_unit[i] = HTTP_UNIT_NONE;
    }
    stat_units[1] = 0;
    stat_units[0] = 0;
    stat_max_us[1] = 0;
    stat_max_us[0] = 0;
}

void http_work_period(void) {
    ran_this_period = false;
}

bool http_work_pending(int slot) {
    return pending_unit[slot] != HTTP_UNIT_NONE;
}

bool http_work_held(int slot) {
    return slot_held[slot];
}

void http_work_offer(int slot, uint8_t unit) {
    if (slot_held[slot] || http_work_pending(slot)) {
        return;
    }
    pending_unit[slot] = unit;
}

bool http_work_cancel(int slot) {
    if (slot_held[slot]) {
        return false;
    }
    pending_unit[slot] = HTTP_UNIT_NONE;
    return true;
}

int http_work_next(const bool runnable[HTTP_WORK_SLOTS], int32_t remaining_us, uint8_t *unit) {
    *unit = HTTP_UNIT_NONE;
    if (ran_this_period && (remaining_us < (int32_t)HTTP_UNIT_BUDGET_US)) {
        return -1;
    }
    for (int k = 0; k < HTTP_WORK_SLOTS; k++) {
        int i = (turn + k) % HTTP_WORK_SLOTS;
        if (slot_held[i]) {
            continue;
        }
        if (http_work_pending(i)) {
            *unit = pending_unit[i];
            pending_unit[i] = HTTP_UNIT_NONE;
        } else if (!runnable[i]) {
            continue;
        } else {
            /* runnable with nothing pending: run the slot's own step */
        }
        turn = (i + 1) % HTTP_WORK_SLOTS;
        ran_this_period = true;
        return i;
    }
    return -1;
}

int http_work_claim(uint32_t grant_us) {
    if (grant_us < HTTP_WORKER_LUA_MIN_US) {
        return 0;
    }
    uint32_t room = grant_us - HTTP_WORKER_LUA_MIN_US;
    int n = 0;
    for (int i = 0; (i < HTTP_WORK_SLOTS) && (room >= HTTP_WORKER_UNIT_US); i++) {
        if (!http_work_pending(i)) {
            continue;
        }
        held_unit[i] = pending_unit[i];
        pending_unit[i] = HTTP_UNIT_NONE;
        slot_started[i] = false;
        done[i] = false;
        slot_held[i] = true;
        room -= HTTP_WORKER_UNIT_US;
        n++;
    }
    WORK_FENCE();
    return n;
}

uint8_t http_work_reclaim(bool worker_idle, uint8_t *lost) {
    *lost = 0;
    if (!worker_idle) {
        return 0;
    }
    WORK_FENCE();
    uint8_t back = 0;
    for (int i = 0; i < HTTP_WORK_SLOTS; i++) {
        if (!slot_held[i]) {
            continue;
        }
        back |= (uint8_t)(1u << (uint32_t)i);
        if (!done[i] && slot_started[i]) {
            *lost |= (uint8_t)(1u << (uint32_t)i);
        } else if (!slot_started[i]) {
            pending_unit[i] = held_unit[i];
        } else {
            /* started and done: nothing is lost and nothing is queued again */
        }
        slot_started[i] = false;
        done[i] = false;
        slot_held[i] = false;
    }
    return back;
}

void http_work_note(int who, uint32_t us) {
    stat_units[who]++;
    if (us > stat_max_us[who]) {
        stat_max_us[who] = us;
    }
}

void http_work_run(void) {
    for (int i = 0; i < HTTP_WORK_SLOTS; i++) {
        if (!slot_held[i] || slot_started[i]) {
            continue;
        }
        slot_started[i] = true;
        WORK_FENCE();
        uint32_t t0 = http_work_clock_us();
        http_unit_vt[held_unit[i]](i);
        http_work_note(HTTP_ON_WORKER, http_work_clock_us() - t0);
        WORK_FENCE();
        done[i] = true;
    }
}

void http_work_stats(http_work_stats_t *out) {
    for (int w = 0; w < 2; w++) {
        out->units[w] = stat_units[w];
        out->max_us[w] = stat_max_us[w];
    }
}
