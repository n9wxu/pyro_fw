/*
 * The flash window. See flash_window.h for the argument.
 *
 * Plain flags, no synchronisation: every one of them is written and read by
 * core0 and by nothing else. Core1 never calls into this file -- it cannot,
 * because core1 never touches flash.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_window.h"
#include "hardware/structs/watchdog.h"

/* Weak: a board without Lua has no second core, so every moment is a moment
 * core1 is idle. src/lua/lua_core1.c provides the real one. */
__attribute__((weak)) bool lua_core1_flash_ok(void) {
    return true;
}

/* Weak and empty: most boards queue no flash work of their own. */
__attribute__((weak)) void board_flash_service(uint32_t now_ms) {
    (void)now_ms;
}

/* Open at reset, and stays open until core0's exec loop closes it at the end
 * of its first iteration.
 *
 * Everything before that loop runs on core0 alone -- hal_platform_init()
 * formats and mounts the filesystem, hal_config_load() writes a default
 * config.ini on a virgin board, lua_app_init() reads the stored script --
 * and core1 is not launched until two seconds into the loop. There is
 * nothing to collide with, and a window that started shut would turn a fresh
 * board's first boot into a board with no filesystem. */
static bool window_open = true;
static uint32_t opens, skips, refusals, deferrals;

/* How long one arming of the hold lasts.
 *
 * Long enough that a stalled TCP retransmit does not drop the window out
 * from under an upload mid-sector, short enough that a client that vanished
 * costs Lua a couple of seconds rather than the rest of the flight. Every
 * packet re-arms it, so a healthy transfer never sees the expiry. */
#define FLASH_HOLD_MS 2000u

static bool hold_armed;
static uint32_t hold_until_ms;

bool flash_window_is_open(void) {
    return window_open;
}

static uint32_t erases, programs;

void flash_window_open(void) {
    window_open = true;
    opens++;
}

void flash_window_close(void) {
    window_open = false;
}

void flash_window_erased(void) {
    erases++;
}

void flash_window_programmed(void) {
    programs++;
}

uint32_t flash_window_erases(void) {
    return erases;
}

uint32_t flash_window_programs(void) {
    return programs;
}

void flash_window_crumb(unsigned n) {
    watchdog_hw->scratch[0] = 0x53540000u | (uint32_t)n;
}

void flash_window_skipped(void) {
    skips++;
}

void flash_window_refused(void) {
    refusals++;
}

void flash_window_deferred(void) {
    deferrals++;
}

uint32_t flash_window_deferrals(void) {
    return deferrals;
}

uint32_t flash_window_opens(void) {
    return opens;
}

uint32_t flash_window_skips(void) {
    return skips;
}

uint32_t flash_window_refusals(void) {
    return refusals;
}

void flash_window_hold(uint32_t now_ms) {
    hold_until_ms = now_ms + FLASH_HOLD_MS;
    hold_armed = true;
}

void flash_window_release(void) {
    hold_armed = false;
}

bool flash_window_holding(uint32_t now_ms) {
    if (!hold_armed) {
        return false;
    }
    if ((int32_t)(now_ms - hold_until_ms) >= 0) {
        hold_armed = false; /* expired: give core1 its slack back */
        return false;
    }
    return true;
}
