/*
 * The room a flight log has [FLT-LOG-13]: nothing is erased in flight, so a
 * log has what the store held free when it opened. Every write and every
 * sync is charged against it; a sync stores the part page it holds, so it
 * costs a whole page. LOG_ROOM_KEEP pages are kept back for the OUT OF ROOM
 * row and the file's close.
 *
 * Pure: both HALs' writers charge the same way.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_ROOM_H
#define LOG_ROOM_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_ROOM_KEEP 2u /* pages: the OUT OF ROOM row, and the close */
#define LOG_ROOM_TEXT "OUT OF ROOM"

typedef struct {
    uint32_t page;  /* bytes a page holds */
    uint32_t room;  /* bytes the log may spend */
    uint32_t spent; /* bytes spent: written, and the rest of each page a sync stored */
    uint32_t in_page;
    bool out;
} log_room_t;

void log_room_open(log_room_t *r, uint64_t free_bytes, uint32_t page_bytes);

/* True when bytes more fit; false, and out, when they do not. */
bool log_room_take(log_room_t *r, uint32_t bytes);

/* Bytes written. */
void log_room_wrote(log_room_t *r, uint32_t bytes);

/* A sync stored the part page. */
void log_room_synced(log_room_t *r);

#endif
