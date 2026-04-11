/*
 * Pressure processing layer — sits between HAL and flight software.
 *
 * Responsibilities:
 *   1. IIR low-pass filter on raw pressure
 *   2. Hypsometric pressure-to-altitude conversion
 *   3. Ground calibration (accumulates first N raw samples)
 *   4. Altitude ring buffer for flight software consumption
 *
 * Data flow:
 *   HAL pres_tick() → pp_feed(raw_pa, timestamp_ms)
 *                    → IIR filter → altitude conversion
 *                    → ring buffer → flight software reads via pp_read()
 *
 * The flight software sees only altitude_cm + timestamp.  All pressure
 * math is encapsulated here.  Tests and hardware use the same code path.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_PROCESSING_H
#define PRESSURE_PROCESSING_H

#include <stdint.h>
#include <stdbool.h>

/* ── Altitude sample — what flight software consumes ─────────────── */

typedef struct {
    int32_t altitude_cm;
    uint32_t timestamp_ms;
} altitude_sample_t;

/* ── Ring buffer sizing ──────────────────────────────────────────── */

#define PP_RING_SIZE 32 /* power of 2; ~640ms at 50Hz */
#define PP_RING_MASK (PP_RING_SIZE - 1)

#define PP_CAL_SAMPLES 10 /* number of raw samples for ground calibration */

/* ── Lifecycle ───────────────────────────────────────────────────── */

void pp_init(void);

/* Test helper: prime pp to PP_RUNNING with given ground pressure.
 * Allows unit tests to skip the boot/calibration sequence. */
void pp_test_prime(int32_t ground_pressure_pa);

/* Start ground calibration.  Until calibration completes, pp_feed()
 * accumulates raw pressure but produces no altitude output. */
void pp_start_cal(void);

/* Returns true once calibration has accumulated PP_CAL_SAMPLES. */
bool pp_cal_done(void);

/* Returns the ground pressure computed during calibration. */
int32_t pp_ground_pressure(void);

/* Overwrite the ground pressure reference (PAD_IDLE tracking and launch snap).
 * Only valid while PP_RUNNING; ignored otherwise. */
void pp_set_ground_pressure(int32_t pa);

/* ── Producer: called by HAL or test code ────────────────────────── */

/* Feed a raw pressure sample.  During calibration this accumulates for
 * the ground average.  After calibration, it filters, converts to
 * altitude, and pushes to the ring buffer. */
void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms);

/* ── Consumer: called by flight software ─────────────────────────── */

/* Number of altitude samples available in the ring. */
int pp_available(void);

/* Read the oldest altitude sample from the ring.
 * Returns true if a sample was available, false if ring is empty. */
bool pp_read(altitude_sample_t *out);

/* ── Debug accessors (for telemetry / status display) ────────────── */

int32_t pp_last_raw_pa(void);
int32_t pp_last_filtered_pa(void);

/* ── Unit-testable internals (exposed for test_flight_states) ────── */

int32_t pp_filter_pressure(int32_t raw_pressure, uint32_t dt_ms);
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa);

#endif /* PRESSURE_PROCESSING_H */
