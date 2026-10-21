/*
 * snprintf for the text the flight path writes [FLT-RT-02, DD-104]: the
 * telemetry sentences, the flight log's estimator rows, the fire and fault
 * diagnostics and the script's status line.
 *
 * The conversions are %s, %c, %d, %i, %u, %x and %X, with the l length, a
 * 0 or - flag and a width, and %%: what those formats use. Their output is
 * the C library's, character for character; a conversion outside them is
 * copied as written. It is a few hundred bytes of integer code, small enough to
 * run from RAM, where the library's printf, with its floating point, is not.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SMALL_FMT_H
#define SMALL_FMT_H

#include <stdarg.h>
#include <stddef.h>

/* As snprintf: the length the whole text would have, NUL-terminated within
 * cap when cap is above zero. */
int small_snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int small_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif
