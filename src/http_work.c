/*
 * SPDX-License-Identifier: MIT
 */
#include "http_work.h"
#include <string.h>

static uint8_t pending_unit[HTTP_WORK_SLOTS];
static bool ran_this_period;
static int turn;

static uint32_t stat_units;
static uint32_t stat_max_us;

void http_work_reset(void) {
    memset(pending_unit, HTTP_UNIT_NONE, sizeof(pending_unit));
    ran_this_period = false;
    turn = 0;
    stat_units = 0;
    stat_max_us = 0;
}

void http_work_period(void) {
    ran_this_period = false;
}

bool http_work_pending(int slot) {
    return pending_unit[slot] != HTTP_UNIT_NONE;
}

void http_work_offer(int slot, uint8_t unit) {
    if (http_work_pending(slot)) {
        return;
    }
    pending_unit[slot] = unit;
}

void http_work_cancel(int slot) {
    pending_unit[slot] = HTTP_UNIT_NONE;
}

int http_work_next(const bool runnable[HTTP_WORK_SLOTS], int32_t remaining_us, uint8_t *unit) {
    *unit = HTTP_UNIT_NONE;
    if (ran_this_period && remaining_us < (int32_t)HTTP_UNIT_BUDGET_US) {
        return -1;
    }
    for (int k = 0; k < HTTP_WORK_SLOTS; k++) {
        int i = (turn + k) % HTTP_WORK_SLOTS;
        if (http_work_pending(i)) {
            *unit = pending_unit[i];
            pending_unit[i] = HTTP_UNIT_NONE;
        } else if (!runnable[i]) {
            continue;
        }
        turn = (i + 1) % HTTP_WORK_SLOTS;
        ran_this_period = true;
        return i;
    }
    return -1;
}

void http_work_note(uint32_t us) {
    stat_units++;
    if (us > stat_max_us) {
        stat_max_us = us;
    }
}

void http_work_stats(http_work_stats_t *out) {
    out->units = stat_units;
    out->max_us = stat_max_us;
}
