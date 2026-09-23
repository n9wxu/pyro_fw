/*
 * The flash window — when core0 may write flash.
 *
 * Core0 erases and programs flash only inside a window it opens itself, at a
 * point in the period where core1 is idling in RAM. Each period runs the
 * flight stages, then the window, then core1's next unit.
 *
 * Core0 must never wait for core1 to reach that state. Spinning on it
 * deadlocks: only lua_app_service() clears the flag, and core0 cannot reach
 * lua_app_service() while it spins. See docs/core1_hazard.md.
 *
 * A flash write that arrives outside the window fails instead of blocking.
 * littlefs_driver.c returns LFS_ERR_IO, hal_common.c refuses the open, and
 * http_server.c hands the segment back to lwIP. Each caller retries a period
 * later.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_WINDOW_H
#define FLASH_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/* Defined in src/lua/lua_core1.c; weak and always true in flash_window.c, for
 * a board built without Lua. */
bool lua_core1_flash_ok(void);

/* ── The window itself. core0 only. ───────────────────────────────── */

bool flash_window_is_open(void);
void flash_window_open(void);
void flash_window_close(void);
uint32_t flash_window_opens(void);

/* Below, past tense records an event and plural reads its count.
 *
 * Core1 overran its grant, so the period got no window. */
void flash_window_skipped(void);
uint32_t flash_window_skips(void);

/* An lfs erase or program that arrived with the window shut. Should stay at
 * zero: every caller is admitted into an open window before it starts, so any
 * count here is a caller that was not. */
void flash_window_refused(void);
uint32_t flash_window_refusals(void);

/* A request handed back to lwIP unparsed. Lossless, since lwIP redelivers it
 * a period later. A large count means core1 is taking most of the slack. */
void flash_window_deferred(void);
uint32_t flash_window_deferrals(void);

/* An erase costs tens of milliseconds and the datasheet worst case is
 * hundreds, so what a window costs is measured rather than assumed.
 *
 * Duration lands in one of two stages. A window opened at STAGE 7 and drained
 * there is stage_max_us[7]. A write driven from an HTTP callback under a hold
 * runs in the slack loop, so its cost is stage_max_us[8] -- measured at 52-64
 * ms for two erases across MK1A, MK1B and MK1C, against 22-41 us for a window
 * that only programs a page. */
void flash_window_erased(void);
void flash_window_programmed(void);
uint32_t flash_window_erases(void);
uint32_t flash_window_programs(void);

/* Decoded by the safe-boot latch like a main-loop stage number. CRUMB in
 * main_hardware.c holds the map. */
void flash_window_crumb(unsigned n);

/* ── The hold ─────────────────────────────────────────────────────
 *
 * Armed by a caller that needs more than one window's worth of flash: an HTTP
 * upload, an OTA image, a config save. While a hold is live core0 dispatches
 * core1 nothing, so the window covers the loop's whole slack.
 *
 * Re-arm on every packet. A hold expires on a deadline rather than on a
 * release, so a connection that dies mid-upload cannot park core1
 * permanently. */
void flash_window_hold(uint32_t now_ms);
bool flash_window_holding(uint32_t now_ms);
void flash_window_release(void);

/* ── Queued flash work, drained inside the window ─────────────────
 *
 * Declared here rather than in hal.h and board_if.h: those are the contracts
 * every board implements, including the host test doubles, and only the
 * RP2040 exec loop calls anything below. board_flash_service() is weak, so a
 * board that queues no flash of its own defines nothing. */
void hal_flash_service(uint32_t now_ms);
void board_flash_service(uint32_t now_ms);
uint32_t hal_log_dropped(void);

/* One line of script output, appended to the flight log as an event row.
 *
 * Returns false when the line was not recorded. No flight being logged is the
 * ordinary case and is not counted: the flight log exists from launch to
 * landing, and ground output goes to the web console. Buffer pressure is
 * counted, separately from the samples' own counter, because a lost sample
 * and a lost script line need different fixes. */
bool hal_log_text(uint32_t time_ms, const char *text, int len);
uint32_t hal_log_text_dropped(void);

/* A flight-log row for something the firmware was told to do and did NOT.
 *
 * Written next to the event that commanded it, so a log showing PYRO1 also
 * shows that nothing happened -- a fire event with no note beside it is a
 * record of an ignition that did not occur. See pyro_release.h. */
bool hal_log_mock(uint32_t time_ms, const char *what);

#endif
