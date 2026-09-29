/*
 * The HAL's storage side, beyond hal.h: the filesystem's in-flight lock, the
 * storage task's service, and the flight log's text rows.
 *
 * Declared here rather than in hal.h and board_if.h: those are the contracts
 * every board implements, including the host test doubles, and only the
 * RP2040 tasks call what is below.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* ── The filesystem in flight [WEB-API-08, DD-058] ─────────────────
 *
 * The flight log holds the filesystem from launch until its tail is written,
 * and nothing else may use it meanwhile. hal_fs_enter() before any file use
 * but the log's returns HAL_FS_LOCKED meanwhile, else 0, and counts the use
 * until hal_fs_leave(). The HAL's own file calls do this; the web server does
 * it for the files it opens itself, and answers 423. */
bool hal_fs_locked(void);
int hal_fs_enter(void);
void hal_fs_leave(void);

/* The storage task's pass: the flight log's ring into its file, a drawn MAC
 * into /serial.txt, and whatever flash work the board queued. Never from the
 * flight task. */
void hal_storage_service(uint32_t now_ms);

/* Weak and empty: most boards queue no flash work of their own. */
void board_flash_service(uint32_t now_ms);

uint32_t hal_log_dropped(void);

/* One line of script output, appended to the flight log as an event row.
 *
 * Returns false when the line was not recorded. No flight being logged is the
 * ordinary case and is not counted: the flight log exists from launch to
 * landing, and ground output goes to the web console. Ring pressure is
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
