/*
 * The counters in the flight log [FLT-LOG-15]: what /api/status counts of the
 * log, the flash, the loop and the sensor. All of them when a log opens and
 * when it closes; between, each fault counter in the flight loop's pass it
 * changes, and nothing that has not. Rows of "CNT name=value ..." that fit a
 * text row.
 *
 * Pure: the HAL reads the values, this says which go in a row.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_COUNTERS_H
#define LOG_COUNTERS_H

#include <stdbool.h>
#include <stdint.h>

/* X(enum, name, said): LOG_CNT_ON_CHANGE in the pass it changes, a fault's
 * evidence; LOG_CNT_AT_ENDS at the open and the close only, the flash's own
 * activity, which moves with every page the log writes -- said each pass, it
 * filled the ring and cost the samples it was there to explain. */
#define LOG_CNT_ON_CHANGE 1
#define LOG_CNT_AT_ENDS 0
#define LOG_COUNTERS(X)                                                                                                \
    X(LOG_CNT_DROPPED, "drop", LOG_CNT_ON_CHANGE)         /* log_dropped */                                            \
    X(LOG_CNT_TEXT_DROPPED, "tdrop", LOG_CNT_ON_CHANGE)   /* text bytes the ring refused */                            \
    X(LOG_CNT_PROGRAMS, "prog", LOG_CNT_AT_ENDS)          /* flash_programs */                                         \
    X(LOG_CNT_ERASES, "erase", LOG_CNT_ON_CHANGE)         /* flash_erases: none in flight */                           \
    X(LOG_CNT_REFUSALS, "refuse", LOG_CNT_ON_CHANGE)      /* flash_refusals */                                         \
    X(LOG_CNT_SKIPS, "skip", LOG_CNT_ON_CHANGE)           /* flash_skips */                                            \
    X(LOG_CNT_FLIGHT_REFUSALS, "fref", LOG_CNT_ON_CHANGE) /* flight_refusals */                                        \
    X(LOG_CNT_SUSPENDS, "susp", LOG_CNT_AT_ENDS)          /* suspends */                                               \
    X(LOG_CNT_WAITS, "wait", LOG_CNT_AT_ENDS)             /* waits */                                                  \
    X(LOG_CNT_OVERRUNS, "ovr", LOG_CNT_ON_CHANGE)         /* loop_overruns */                                          \
    X(LOG_CNT_OVERRUNS_GROUND, "ovrg", LOG_CNT_ON_CHANGE) /* loop_overruns_ground */                                   \
    X(LOG_CNT_WRITER_OVERRUNS, "wovr", LOG_CNT_ON_CHANGE) /* writer_overruns */                                        \
    X(LOG_CNT_LUA_SKIPPED, "lskip", LOG_CNT_ON_CHANGE)    /* lua_skipped */                                            \
    X(LOG_CNT_PRES_REJECTS, "prej", LOG_CNT_ON_CHANGE)    /* pres_rejects */                                           \
    X(LOG_CNT_PRES_RECOVERIES, "prec", LOG_CNT_ON_CHANGE) /* pres_recoveries */                                        \
    X(LOG_CNT_PRES_DROPPED, "pdrop", LOG_CNT_ON_CHANGE)   /* pres_dropped */                                           \
    X(LOG_CNT_PRES_FLASHED, "pflash", LOG_CNT_AT_ENDS)    /* pres_flashed: every reading beside a page's program */

#define LOG_CNT_ENUM(e, name, said) e,
typedef enum { LOG_COUNTERS(LOG_CNT_ENUM) LOG_COUNTER_COUNT } log_counter_t;
#undef LOG_CNT_ENUM

typedef struct {
    uint32_t last[LOG_COUNTER_COUNT]; /* as the log last said */
    uint32_t due;                     /* a bit a counter: to go in a row */
} log_counters_t;

/* Every counter due: at a log's open, and at its close. */
void log_counters_all(log_counters_t *c);

/* The counters said on change that changed since the log last said, due. */
void log_counters_note(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT]);

/* The next row of due counters into out, and those counters said; its
 * length, 0 when none is due. */
int log_counters_row(log_counters_t *c, const uint32_t now[LOG_COUNTER_COUNT], char *out, int cap);

#endif
