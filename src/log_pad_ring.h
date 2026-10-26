/*
 * The pad's last seconds, held for the flight log [DAT-11, DD-110].
 *
 * Every PAD_IDLE sample goes in; the oldest falls out. At launch the log
 * opens with all of them, timed from T+0, so it starts on the ground. 110
 * hold 1.5 s at the fastest sensor's 73 readings a second: the slowest rocket
 * designed for is declared within 1 s of T+0 [FLT-LAUNCH-08], which leaves
 * half a second of pad before it.
 *
 * Nine bytes a sample, for the RAM a board has left: the filtered pressure,
 * the raw reading as its difference from it, the low 16 bits of the sample's
 * time, which T+0 places since the ring spans far less than 32 s, and the
 * sensor's temperature as tenths of a degree from the ring's first [DAT-10].
 * A gap of a second in the pad's samples empties the ring, so nothing in it
 * is old enough for its time to be misplaced.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_PAD_RING_H
#define LOG_PAD_RING_H

#include "flight_log.h"
#include <stdbool.h>
#include <stdint.h>

#define LOG_PAD_N 110

typedef struct {
    int32_t pressure_pa;
    int16_t raw_less_pressure_pa;
    uint16_t t_ms; /* the low 16 bits */
} log_pad_entry_t;

typedef struct {
    log_pad_entry_t e[LOG_PAD_N];
    int8_t temp_less_first_dc[LOG_PAD_N];
    uint16_t next, count;
    uint32_t newest_ms;
    int16_t first_temp_dc;
} log_pad_ring_t;

void log_pad_clear(log_pad_ring_t *r);
void log_pad_push(log_pad_ring_t *r, uint32_t t_ms, int32_t pressure_pa, int32_t raw_pa, int16_t temp_dc);
uint16_t log_pad_count(const log_pad_ring_t *r);

/* The i-th oldest as a log row: timed from t0, its height above ground_pa
 * and the given state. False past the newest. */
bool log_pad_row(const log_pad_ring_t *r, uint16_t i, uint32_t t0_ms, int32_t ground_pa, uint8_t state,
                 flog_sample_t *out);

#endif
