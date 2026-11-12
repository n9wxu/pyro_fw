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

static uint8_t live_ring[LOG_LIVE_BYTES];
static volatile uint32_t head; /* bytes written since the log began */

void log_live_reset(void) {
    head = 0;
    PUBLISH();
}

void log_live_write(const uint8_t *src, uint32_t n) {
    uint32_t h = head;
    const uint8_t *s = src;
    uint32_t len = n;
    if (len > LOG_LIVE_BYTES) {
        s = &s[len - LOG_LIVE_BYTES];
        h += len - LOG_LIVE_BYTES;
        len = LOG_LIVE_BYTES;
    }
    uint32_t at = h % LOG_LIVE_BYTES;
    uint32_t first = (len < (LOG_LIVE_BYTES - at)) ? len : (LOG_LIVE_BYTES - at);
    (void)memcpy(&live_ring[at], s, first);
    (void)memcpy(live_ring, &s[first], len - first);
    PUBLISH();
    head = h + len;
}

static uint32_t oldest_of(uint32_t end) {
    return (end > LOG_LIVE_BYTES) ? (end - LOG_LIVE_BYTES) : 0u;
}

uint32_t log_live_read(uint32_t since, uint8_t *dst, uint32_t max, uint32_t *first, uint32_t *end) {
    uint32_t e = head;
    PUBLISH();
    uint32_t from = since;
    if ((from < oldest_of(e)) || (from > e)) {
        from = oldest_of(e);
    }
    uint32_t n = ((e - from) < max) ? (e - from) : max;
    uint32_t at = from % LOG_LIVE_BYTES;
    uint32_t part = (n < (LOG_LIVE_BYTES - at)) ? n : (LOG_LIVE_BYTES - at);
    (void)memcpy(dst, &live_ring[at], part);
    (void)memcpy(&dst[part], live_ring, n - part);
    PUBLISH();
    /* Bytes the writer overwrote while they were copied are not the log's. */
    uint32_t still = oldest_of(head);
    if (still > from) {
        uint32_t lost = ((still - from) < n) ? (still - from) : n;
        (void)memmove(dst, &dst[lost], n - lost);
        n -= lost;
        from += lost;
    }
    *first = from;
    *end = e;
    return n;
}
