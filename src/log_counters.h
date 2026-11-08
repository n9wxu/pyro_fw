/*
 * The counters in the flight log [FLT-LOG-15]: what /api/status counts of the
 * log, the flash, the loop and the sensor. All of them when a log opens and
 * when it closes; between, each in the flight loop's pass it changes, and
 * nothing that has not. Rows of "CNT name=value ..." that fit a text row.
 *
 * Pure: the HAL reads the values, this says which go in a row.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_COUNTERS_H
#define LOG_COUNTERS_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_COUNTERS(X)                                                                                                \
    X(LOG_CNT_DROPPED, "drop")         /* log_dropped */                                                               \
    X(LOG_CNT_TEXT_DROPPED, "tdrop")   /* text bytes the ring refused */                                               \
    X(LOG_CNT_PROGRAMS, "prog")        /* flash_programs */                                                            \
    X(LOG_CNT_ERASES, "erase")         /* flash_erases */                                                              \
    X(LOG_CNT_REFUSALS, "refuse")      /* flash_refusals */                                                            \
    X(LOG_CNT_SKIPS, "skip")           /* flash_skips */                                                               \
    X(LOG_CNT_FLIGHT_REFUSALS, "fref") /* flight_refusals */                                                           \
    X(LOG_CNT_SUSPENDS, "susp")        /* suspends */                                                                  \
    X(LOG_CNT_WAITS, "wait")           /* waits */                                                                     \
    X(LOG_CNT_OVERRUNS, "ovr")         /* loop_overruns */                                                             \
    X(LOG_CNT_OVERRUNS_GROUND, "ovrg") /* loop_overruns_ground */                                                      \
    X(LOG_CNT_WRITER_OVERRUNS, "wovr") /* writer_overruns */                                                           \
    X(LOG_CNT_LUA_SKIPPED, "lskip")    /* lua_skipped */                                                               \
    X(LOG_CNT_PRES_REJECTS, "prej")    /* pres_rejects */                                                              \
    X(LOG_CNT_PRES_RECOVERIES, "prec") /* pres_recoveries */                                                           \
    X(LOG_CNT_PRES_DROPPED, "pdrop")   /* pres_dropped */                                                              \
    X(LOG_CNT_PRES_FLASHED, "pflash")  /* pres_flashed */

#define LOG_CNT_ENUM(e, name) e,
typedef enum { LOG_COUNTERS(LOG_CNT_ENUM) LOG_COUNTER_COUNT } log_counter_t;
#undef LOG_CNT_ENUM

typedef struct {
    uint32_t last[LOG_COUNTER_COUNT]; /* as the log last said */
    uint32_t due;                     /* a bit a counter: to go in a row */
} log_counters_t;

/* Every counter due: at a log's open, and at its close. */
void log_counters_all(log_counters_t *c);

/* The counters changed since the log last said, due. */
void log_counters_note(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT]);

/* The next row of due counters into out, and those counters said; its
 * length, 0 when none is due. */
int log_counters_row(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT], char *out, int cap);

#endif
