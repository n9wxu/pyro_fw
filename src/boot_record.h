/*
 * The start-up's record [FLT-LOG-14]: what a post-mortem needs to know of
 * how the board started, read hours later with the set-up gone. Rows of
 * FLOG_TEXT_MAX characters at most, each a "BOOT ..." line.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOOT_RECORD_H
#define BOOT_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#define BOOT_RECORD_ROWS 3
#define BOOT_RECORD_ROW_MAX 64 /* FLOG_TEXT_MAX */

typedef struct {
    const char *cause; /* the reset cause: power, run_pin, software, debug */
    bool watchdog;     /* the watchdog ended the last start-up */
    int32_t stage;     /* the stage it caught; -1: none recorded */
    uint32_t stage_ms; /* the uptime it caught it at */
    const char *fw, *flash;
    uint32_t flash_kb;
    bool usb;
    const char *start;    /* what the flight software decided: resumed, or why not */
    const char *last_log; /* the newest log the store held; "" for none */
} boot_facts_t;

typedef char boot_rows_t[BOOT_RECORD_ROWS][BOOT_RECORD_ROW_MAX + 1];

void boot_record_rows(const boot_facts_t *f, boot_rows_t rows);

/* The reset cause's name in a row (reset_cause_t). */
const char *boot_record_cause(int reset_cause);

/* The boot log is appended to, never rewritten, so it needs no buffer. Past
 * BOOT_LOG_ROTATE_BYTES it becomes BOOT_LOG_OLDER_PATH, the one before is
 * dropped, and a new one starts: 4 to 8 kB of start-ups are kept, some 25 to
 * 50 of them. */
#define BOOT_LOG_OLDER_PATH "boot_log.1.txt"
#define BOOT_LOG_ROTATE_BYTES 4096

/* The rows as the boot log's lines, into out; the bytes, 0 when they do not
 * fit. */
int boot_record_lines(boot_rows_t rows, char *out, int cap);

#endif
