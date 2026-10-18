/*
 * The flight log held for the host, in test mode with the log kept off the
 * flash [USB-09, WEB-API-16]: a ring of the log's bytes, read by offset from
 * the start of the log. The storage task writes; the net task reads, at its
 * own pace, whatever is still held.
 *
 * Neither waits for the other: the writer publishes its offset after the
 * bytes, and a reader that the writer overtook mid-copy drops the bytes it
 * cannot trust.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_LIVE_H
#define LOG_LIVE_H

#include <stdint.h>

/* Seconds of the log, at 1.2 to 1.6 kB a second, for a host that reads it
 * once a second. */
#define LOG_LIVE_BYTES 8192u

/* A new log, from offset 0. */
void log_live_reset(void);

/* After what is held; the oldest bytes give way. */
void log_live_write(const uint8_t *src, uint32_t n);

/* The bytes held from offset since, at most max, into dst; returns how many.
 * *first is dst[0]'s offset: since, or the oldest byte held when since is no
 * longer held or lies past the log's end. *end is the offset the log has
 * reached. */
uint32_t log_live_read(uint32_t since, uint8_t *dst, uint32_t max, uint32_t *first, uint32_t *end);

#endif
