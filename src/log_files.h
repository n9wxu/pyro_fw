/*
 * The flight logs' names [FLT-LOG-12]: each log its own file,
 * flight_NNNN.bin, numbered one past the highest the store holds. Nothing in
 * flight removes one; only an erase on the bench does. A board's
 * flight_log.bin from before is number 0, the oldest.
 *
 * Pure: the store is asked through exists(), so the HALs and the host tests
 * number logs the same way.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_FILES_H
#define LOG_FILES_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_FILES_NAME_MAX 16 /* "flight_9999.bin" and its terminator */
#define LOG_FILES_MAX 9999u
#define LOG_FILES_LEGACY "flight_log.bin"

/* A run of this many missing numbers ends the search: a log deleted on its
 * own leaves a gap, and the numbering goes on past it. */
#define LOG_FILES_GAP 8u

typedef bool (*log_files_exists_fn)(void *ctx, const char *name);

typedef struct {
    uint32_t newest; /* 0: none numbered */
    bool legacy;     /* a flight_log.bin is held */
} log_files_t;

void log_files_name(uint32_t n, char out[LOG_FILES_NAME_MAX]);

/* The number a log's name carries, or 0 when it is no numbered log. */
uint32_t log_files_number(const char *name);

/* True for every name an erase of the logs removes. */
bool log_files_is_log(const char *name);

/* What the store holds. On the ground: one exists() per number. */
void log_files_scan(log_files_t *f, log_files_exists_fn exists, void *ctx);

/* The newest log's name; false when there is none. */
bool log_files_newest(const log_files_t *f, char out[LOG_FILES_NAME_MAX]);

/* The name for a new log, which becomes the newest; false when every number
 * is taken. */
bool log_files_next(log_files_t *f, char out[LOG_FILES_NAME_MAX]);

#endif
