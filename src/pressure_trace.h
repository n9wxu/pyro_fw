/*
 * Every conversion the pressure sensor delivers, kept for the bench: its
 * stamp, when the loop read it, its raw code and what it compensated to.
 * support/pressure_trace.py reads it through /api/pressure/trace and looks
 * for what a sample rate hides -- a stale read, a missed slot, a gap.
 *
 * Written and read on core0 only.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_TRACE_H
#define PRESSURE_TRACE_H

#include <stdint.h>

#define PTRACE_N 256
#define PTRACE_MAGIC "PTR1"

typedef enum {
    PTRACE_PRESSURE = 'P',
    PTRACE_TEMPERATURE = 'T',
    PTRACE_ZERO = 'Z',  /* read before the conversion finished */
    PTRACE_BUS = 'B',   /* the sensor did not answer */
    PTRACE_RANGE = 'R', /* compensated outside 1-120 kPa */
    PTRACE_MISSED = 'W', /* the loop found the last conversion still running */
    /* [DD-068] a flash operation ran during the conversion: not fed on */
    PTRACE_FLASHED = 'F',  /* the pressure's */
    PTRACE_FLASHED_T = 'G' /* the temperature's */
} ptrace_kind_t;

/* 24 bytes, little-endian, as sent. */
typedef struct {
    uint32_t at_us;   /* the driver's stamp, low 32 bits */
    uint32_t read_us; /* when the loop took it */
    uint32_t raw;     /* D1/D2, or the BMP280's adc_P */
    uint32_t raw_t;   /* the BMP280's adc_T; 0 for the MS5607 */
    int32_t pa_c;     /* compensated, hundredths of a pascal; 0 when none */
    uint8_t kind;
    uint8_t pad[3];
} ptrace_rec_t;

void ptrace_note(uint32_t at_us, uint32_t read_us, uint32_t raw, uint32_t raw_t, int32_t pa_c, ptrace_kind_t kind);

/* The records numbered since onward, oldest first, into dst: a 12-byte head
 * (the magic, the number of the first record sent, the number to ask for
 * next), then the records. At most what fits in cap. Records older than the
 * ring holds are gone: the first sent is then later than since. Returns the
 * bytes written. */
int ptrace_read(uint32_t since, uint8_t *dst, int cap);

void ptrace_reset(void);

#endif
