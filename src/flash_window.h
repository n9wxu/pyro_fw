/*
 * The flash window — when core0 is allowed to write flash.
 *
 * The architecture is one sentence:
 *
 *     every flash write happens at a moment core0 chose, while core1 is
 *     idle in RAM.
 *
 * Not "core0 waits until core1 is idle". Waiting is what produced the
 * deadlock this module replaces: core0 spun on a flag only core0 could
 * clear, because clearing it meant reaching lua_app_service(), which core0
 * could not do while it was spinning. See docs/core1_hazard.md.
 *
 * So the window is scheduled, not negotiated:
 *
 *   - core0's exec loop runs the flight stages first. Those never touch
 *     flash; a log line goes into a RAM buffer and stays there.
 *   - Once per period, at a point where core1's last grant has provably
 *     expired, core0 opens the window and drains everything that was
 *     queued. Core1 is idle because core0 has not handed it work, not
 *     because it was asked to stop.
 *   - Core0 closes the window, then dispatches core1's next unit.
 *
 * Outside the window a flash write does not block and does not proceed: it
 * fails. littlefs_driver.c returns LFS_ERR_IO, hal_common.c refuses the
 * open, and the HTTP server pushes back on the TCP connection. Every one of
 * those is a caller that retries a period later, when the window is open.
 *
 * A streaming upload or an OTA image cannot wait for one 10 ms window per
 * sector, so it arms a HOLD: for as long as the hold is live core0 skips
 * core1's dispatch entirely, core1 stays parked in RAM, and the window
 * stays open across the whole slack loop. The hold is a deadline, not a
 * reference count, so a dropped connection cannot park Lua forever.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_WINDOW_H
#define FLASH_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/* Is core1 somewhere the window may be opened around?
 *
 * Defined in src/lua/lua_core1.c, where it means "core1's program counter is
 * in RAM and it is past its unbounded startup". Weak and true in
 * flash_window.c, which is what a board built without Lua links: there is no
 * second core to collide with.
 *
 * core0 READS this to decide whether to open the window. It never waits on
 * it. That distinction is the fix for the deadlock -- see the file comment. */
bool lua_core1_flash_ok(void);

/* ── The window itself. core0 only. ───────────────────────────────── */

/* True while core0 is inside the scheduled window. Every flash write in the
 * firmware is gated on this and nothing waits on it. */
bool flash_window_is_open(void);

void flash_window_open(void);
void flash_window_close(void);

/* Counted, not silent: a period whose window never opened is core1
 * overrunning its grant, and /api/status says so. */
void flash_window_skipped(void);

/* An lfs erase or program that arrived with the window shut, and was
 * therefore NOT performed. This one should always read zero: every caller is
 * supposed to have been admitted into an open window before it started, so a
 * non-zero count means one of them was not. Kept separate from the HTTP
 * pushback below precisely so the two cannot be confused -- one is a bug and
 * the other is the mechanism working. */
void flash_window_refused(void);

/* A request handed back to lwIP because the window was shut when it arrived.
 * Expected and lossless: lwIP redelivers it a period later. Counted because
 * a large number means core1 is monopolising the slack. */
void flash_window_deferred(void);
uint32_t flash_window_deferrals(void);

uint32_t flash_window_opens(void);

/* What the window actually costs, measured rather than assumed: how many
 * sector erases and page programs have run inside one. A flash erase is tens
 * of milliseconds and the datasheet worst case is hundreds, so "the window
 * is short" is a claim that needs a number behind it. How long the window
 * takes is already reported -- it is stage_max_us[7]. */
void flash_window_erased(void);
void flash_window_programmed(void);
uint32_t flash_window_erases(void);
uint32_t flash_window_programs(void);

/* A crumb any file in the flash path can drop, decoded by the safe-boot
 * latch exactly like a main-loop stage number. See CRUMB in main_hardware.c
 * for the number map. */
void flash_window_crumb(unsigned n);
uint32_t flash_window_skips(void);
uint32_t flash_window_refusals(void);

/* ── The hold ─────────────────────────────────────────────────────
 *
 * Armed by a caller that needs more than one window's worth of flash: an
 * HTTP upload, an OTA image, a config save. While it is live core0 does not
 * dispatch core1, so the window covers the loop's whole slack.
 *
 * Re-arm on every packet. It expires on its own, which is what stops a
 * connection that died mid-upload from being a permanent Lua outage. */
void flash_window_hold(uint32_t now_ms);
bool flash_window_holding(uint32_t now_ms);
void flash_window_release(void);

/* ── Queued flash work, drained inside the window ─────────────────
 *
 * Declared here rather than in hal.h on purpose: this is core0 scheduling,
 * not a contract every board implements. Widening hal.h would make every
 * board -- including the host test doubles -- carry a function that only
 * the RP2040 exec loop has any use for. */
void hal_flash_service(uint32_t now_ms);

/* A board's own queued flash work, run inside the window alongside the
 * logs. Weak and empty by default, so a board that writes no flash of its
 * own -- which is most of them -- defines nothing.
 *
 * Here rather than in board_if.h deliberately: board_if.h is the contract
 * every board must implement, and this is an option exactly one board
 * currently takes up. */
void board_flash_service(uint32_t now_ms);

/* Flight-log bytes the RAM buffer could not hold because the windows were
 * not draining it fast enough. Here for the same reason: it is a fact about
 * core0's scheduling, not part of the board contract. */
uint32_t hal_log_dropped(void);

#endif
