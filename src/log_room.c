/*
 * See log_room.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_room.h"

void log_room_open(log_room_t *r, uint64_t free_bytes, uint32_t page_bytes) {
    uint64_t keep = (uint64_t)LOG_ROOM_KEEP * page_bytes;
    *r = (log_room_t){.page = page_bytes};
    r->room = free_bytes > keep ? (uint32_t)(free_bytes - keep) : 0u;
}

bool log_room_take(log_room_t *r, uint32_t bytes) {
    if (!r->out && (uint64_t)r->spent + bytes > r->room)
        r->out = true;
    return !r->out;
}

void log_room_wrote(log_room_t *r, uint32_t bytes) {
    r->spent += bytes;
    r->in_page = r->page ? (r->in_page + bytes) % r->page : 0u;
}

void log_room_synced(log_room_t *r) {
    if (r->in_page == 0u)
        return;
    r->spent += r->page - r->in_page;
    r->in_page = 0u;
}
