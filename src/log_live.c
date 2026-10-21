/*
 * The flight log held for the host. See log_live.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_live.h"
#include <string.h>

#if defined(__arm__)
#define PUBLISH() __asm volatile("dmb" ::: "memory")
#else
#define PUBLISH() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static uint8_t ring[LOG_LIVE_BYTES];
static volatile uint32_t head; /* bytes written since the log began */

void log_live_reset(void) {
    head = 0;
    PUBLISH();
}

void log_live_write(const uint8_t *src, uint32_t n) {
    uint32_t h = head;
    if (n > LOG_LIVE_BYTES) {
        src += n - LOG_LIVE_BYTES;
        h += n - LOG_LIVE_BYTES;
        n = LOG_LIVE_BYTES;
    }
    uint32_t at = h % LOG_LIVE_BYTES;
    uint32_t first = n < LOG_LIVE_BYTES - at ? n : LOG_LIVE_BYTES - at;
    memcpy(&ring[at], src, first);
    memcpy(ring, src + first, n - first);
    PUBLISH();
    head = h + n;
}

static uint32_t oldest_of(uint32_t end) {
    return end > LOG_LIVE_BYTES ? end - LOG_LIVE_BYTES : 0u;
}

uint32_t log_live_read(uint32_t since, uint8_t *dst, uint32_t max, uint32_t *first, uint32_t *end) {
    uint32_t e = head;
    PUBLISH();
    uint32_t from = since;
    if (from < oldest_of(e) || from > e)
        from = oldest_of(e);
    uint32_t n = e - from < max ? e - from : max;
    uint32_t at = from % LOG_LIVE_BYTES;
    uint32_t part = n < LOG_LIVE_BYTES - at ? n : LOG_LIVE_BYTES - at;
    memcpy(dst, &ring[at], part);
    memcpy(dst + part, ring, n - part);
    PUBLISH();
    /* Bytes the writer overwrote while they were copied are not the log's. */
    uint32_t still = oldest_of(head);
    if (still > from) {
        uint32_t lost = still - from < n ? still - from : n;
        memmove(dst, dst + lost, n - lost);
        n -= lost;
        from += lost;
    }
    *first = from;
    *end = e;
    return n;
}
