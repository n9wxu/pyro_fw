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
    int32_t altitude_cm; /* clamped to 0-8000 m [SNS-ALT-02, SNS-ALT-03]: what is reported */
    int32_t height_cm;   /* not clamped: what speed is taken from [SNS-ALT-04] */
    int32_t rise_cm;     /* the median reading's own height, unfiltered: T+0 [FLT-LAUNCH-03] */
    uint32_t timestamp_ms;
    uint32_t timestamp_us; /* the same instant, to the microsecond; wraps, so differences only */
    int32_t raw_pa;        /* the reading this sample is centred on, before the median [DAT-02] */
    /* The fit through the last second, at this sample [DD-048]: its pressure
     * and rates, and the height, speed and acceleration they give through the
     * altitude formula's slope at that pressure. None clamped. All zero while
     * fit_valid is false: too few samples in the window. */
    float fit_pa, fit_pdot, fit_pddot; /* Pa, Pa/s, Pa/s^2; pdot < 0 climbing */
    int32_t fit_height_cm;
    int32_t speed_cms; /* up is positive */
    int32_t accel_cms2;
    bool fit_valid;
    bool fit_clean; /* residuals no bigger than pp_sigma_pa() explains */
} altitude_sample_t;

/* ── Ring buffer sizing ──────────────────────────────────────────── */

#define PP_RING_SIZE 32 /* power of 2; ~640ms at 50Hz */
#define PP_RING_MASK (PP_RING_SIZE - 1)

#define PP_CAL_SAMPLES 10 /* number of raw samples for ground calibration */

/* The pressure filter's time constant. On a steady climb or descent the
 * filtered altitude trails the rocket by its rate times this. */
#define PP_FILTER_TAU_MS 500

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

/* Overwrite the ground pressure reference. Only valid while PP_RUNNING;
 * ignored otherwise. */
void pp_set_ground_pressure(int32_t pa);

/* ── The ground reference ─────────────────────────────────────────
 *
 * Ground pressure is a 5-second rolling mean of the filtered pressure, taken
 * while the board sits on the pad. Averaging PRESSURE rather than altitude
 * matters: pressure sits around 101 kPa and is nowhere near the zero that
 * altitude is clamped at, so the mean carries no clamp bias.
 *
 * A boxcar rather than an IIR, because a boxcar forgets: the value frozen at
 * launch is the mean of the last five seconds before it, with nothing older
 * leaking in.
 *
 * FROZEN AT LAUNCH, NOT SNAPPED
 *
 * Do not snap it to the reading at launch detection: that defines T+0 as zero
 * altitude and throws away the 100 ft the rocket climbed to trip the
 * detector. Frozen, every altitude for the rest of the flight keeps it. */
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

/* Freeze the reference to the mean of the blocks that ended before t_ms
 * [GND-CAL-04]. Launch passes the first sample of the rise: a reference frozen
 * at detection, a second or more later, has averaged in the start of the
 * climb, and reads every altitude low. With no such block the reference is
 * kept as it is. Returns false, and pp_ground_degraded() says so, when the
 * blocks span less than a second of the pad. */
bool pp_ground_freeze_before(uint32_t t_ms);
bool pp_ground_tracking(void);

/* How much of the window has filled, for /api/status. The mean is over
 * whatever is there, so an early launch is not penalised -- but a short
 * window is worth knowing about. */
uint32_t pp_ground_window_ms(void);

/* True when the reference frozen at launch rests on less than a second of the
 * pad [GND-CAL-07]. */
bool pp_ground_degraded(void);

/* [GND-CAL-06] A step bigger than the gate -- the board carried to a higher
 * or lower pad -- leaves every sample rejected, and the reference frozen at
 * the old ground for good. How long every sample has been rejected, and a
 * fresh start from the current filtered pressure. */
uint32_t pp_ground_rejecting_ms(uint32_t now_ms);
void pp_ground_reseed(void);
uint32_t pp_ground_reseeds(void);

/* ── The fit's noise [DD-048] ─────────────────────────────────────
 *
 * σ, which decides whether a fit is clean, is the fit's own residual RMS on
 * the pad, measured over about five seconds. Floored at the MS5607's datasheet
 * figure, so the median's quieter output does not make every flight fit look
 * dirty; capped, so a gusty pad cannot loosen the test. A recovered flight
 * takes it from the pad marker. */
#define PP_SIGMA_FLOOR_PA 1.2f
#define PP_SIGMA_CEIL_PA 5.0f
float pp_sigma_pa(void);
void pp_set_sigma(float sigma_pa);

/* ── Recent history [FLT-BROWN-02] ────────────────────────────────
 *
 * The median's output since power-on, in every state, calibration included:
 * brownout recovery decides before calibration starts, and must decide from
 * real readings. About 1.3 s at 50 Hz. */
#define PP_HIST_SIZE 64 /* power of 2 */

/* The oldest and newest times in the history; false while it is empty. */
bool pp_history_span(uint32_t *oldest_ms, uint32_t *newest_ms);

/* The median of the history's readings stamped from from_ms to to_ms, if at
 * least min_n of them are. */
bool pp_history_median(uint32_t from_ms, uint32_t to_ms, int min_n, int32_t *out_pa);

/* Produce altitude in flight without calibrating, against the pad marker's
 * ground pressure: calibrating in the air would call the current height zero.
 * The filter starts at start_pa, and the ground reference stays frozen. */
void pp_resume_flight(int32_t ground_pa, int32_t start_pa);

/* ── Producer: called by HAL or test code ────────────────────────── */

/* Feed a raw pressure sample.  During calibration this accumulates for
 * the ground average.  After calibration, it filters, converts to
 * altitude, and pushes to the ring buffer. */
void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms);

/* [SNS-PRES-08] The same, stamped to the microsecond by the hardware timer at
 * the reading's conversion. Milliseconds are this over 1000, the clock
 * hal_time_ms() reads. */
void pp_feed_us(int32_t raw_pressure_pa, uint64_t timestamp_us);

/* ── Consumer: called by flight software ─────────────────────────── */

/* Number of altitude samples available in the ring. */
int pp_available(void);

/* Read the oldest altitude sample from the ring.
 * Returns true if a sample was available, false if ring is empty. */
bool pp_read(altitude_sample_t *out);

/* ── Debug accessors (for telemetry / status display) ────────────── */

int32_t pp_last_raw_pa(void);

/* The raw reading of the sample pp_read() last returned: what the flight log
 * records beside it, so a log can be replayed through this layer. */
int32_t pp_last_read_raw_pa(void);
int32_t pp_last_filtered_pa(void);

/* ── Unit-testable internals (exposed for test_flight_states) ────── */

int32_t pp_filter_pressure(int32_t raw_pressure, uint32_t dt_ms);
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa);
int32_t pp_pressure_to_height_cm(int32_t pressure_pa, int32_t ground_pressure_pa);

#endif /* PRESSURE_PROCESSING_H */
