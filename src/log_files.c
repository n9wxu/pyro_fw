/*
 * See log_files.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_files.h"
#include <stdio.h>
#include <string.h>

#define PREFIX "flight_"
#define SUFFIX ".bin"
#define DIGITS 4u

void log_files_name(uint32_t n, char out[LOG_FILES_NAME_MAX]) {
    (void)snprintf(out, LOG_FILES_NAME_MAX, PREFIX "%04u" SUFFIX, (unsigned)(n % (LOG_FILES_MAX + 1u)));
}

uint32_t log_files_number(const char *name) {
    size_t prefix = sizeof(PREFIX) - 1u;
    size_t suffix = sizeof(SUFFIX) - 1u;
    if ((strlen(name) != ((prefix + DIGITS) + suffix)) || (strncmp(name, PREFIX, prefix) != 0) ||
        (strcmp(&name[prefix + DIGITS], SUFFIX) != 0)) {
        return 0;
    }
    uint32_t n = 0;
    for (size_t i = prefix; i < (prefix + DIGITS); i++) {
        if ((name[i] < '0') || (name[i] > '9')) {
            return 0;
        }
        int offset = name[i] - '0';
        uint32_t digit = (uint32_t)offset;
        n = (n * 10u) + digit;
    }
    return n;
}

bool log_files_is_log(const char *name) {
    return (log_files_number(name) != 0u) || (strcmp(name, LOG_FILES_LEGACY) == 0);
}

void log_files_scan(log_files_t *f, log_files_exists_fn exists, void *ctx) {
    f->newest = 0;
    f->legacy = exists(ctx, LOG_FILES_LEGACY);
    uint32_t missing = 0;
    char name[LOG_FILES_NAME_MAX];
    for (uint32_t n = 1; (n <= LOG_FILES_MAX) && (missing < LOG_FILES_GAP); n++) {
        log_files_name(n, name);
        if (exists(ctx, name)) {
            f->newest = n;
            missing = 0;
        } else {
            missing++;
        }
    }
}

bool log_files_newest(const log_files_t *f, char out[LOG_FILES_NAME_MAX]) {
    if (f->newest > 0u) {
        log_files_name(f->newest, out);
        return true;
    }
    if (f->legacy) {
        (void)snprintf(out, LOG_FILES_NAME_MAX, "%s", LOG_FILES_LEGACY);
        return true;
    }
    return false;
}

bool log_files_next(log_files_t *f, char out[LOG_FILES_NAME_MAX]) {
    if (f->newest >= LOG_FILES_MAX) {
        return false;
    }
    f->newest++;
    log_files_name(f->newest, out);
    return true;
}
