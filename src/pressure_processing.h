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

/* ── The ground reference ─────────────────────────────────────────
 *
 * Ground pressure is a 5-second rolling mean of the filtered pressure, taken
 * while the board sits on the pad. Averaging PRESSURE rather than altitude
 * matters: pressure sits around 101 kPa and is nowhere near the zero that
 * altitude is clamped at, so the mean carries no clamp bias.
 *
 * A boxcar rather than the 60-second IIR it replaces, because a boxcar
 * forgets: the value frozen at launch is the mean of the last five seconds
 * before it, with nothing older leaking in.
 *
 * FROZEN AT LAUNCH, NOT SNAPPED
 *
 * It used to be snapped to the instantaneous reading at launch detection, so
 * T+0 altitude was zero by definition -- which threw away the 100 ft the
 * rocket had already climbed to trip the detector. Freezing keeps it, and
 * every altitude for the rest of the flight is that much truer. */
/* A sample this far from the current reference is not the pad.
 *
 * Without this the mean chases a climbing rocket: a launch takes about a
 * second to reach the 100 ft trigger, which is a fifth of the window, so the
 * reference would drift a fifth of the way up with it -- delaying detection
 * and biasing the frozen value toward flight pressure, which under-reports
 * every altitude afterwards.
 *
 * 50 Pa is roughly 14 ft. Sensor noise is a pascal or two and weather moves
 * far slower than the mean tracks, so nothing legitimate is rejected; a rocket
 * leaves this band almost immediately. */
#define PP_GROUND_MAX_DEV_PA 50

#define PP_GROUND_WINDOW_MS 5000
#define PP_GROUND_BLOCK_MS 250
#define PP_GROUND_BLOCKS (PP_GROUND_WINDOW_MS / PP_GROUND_BLOCK_MS)

/* Track while true, hold the last value while false. PAD_IDLE enables it;
 * launch freezes it. */
void pp_ground_track(bool enabled);
bool pp_ground_tracking(void);

/* How much of the window has filled, for /api/status. The mean is over
 * whatever is there, so an early launch is not penalised -- but a short
 * window is worth knowing about. */
uint32_t pp_ground_window_ms(void);

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
