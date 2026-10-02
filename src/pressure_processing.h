/*
 * Pressure processing: between the sensor HAL and the flight software.
 *
 * pp_feed_us(): median of three [SNS-PRES-07] -> history [FLT-BROWN-02] ->
 * IIR filter [SNS-PRES-02] -> altitude [SNS-ALT-01] -> ring -> pp_read(),
 * with each sample's fit through the last second [DD-048].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_PROCESSING_H
#define PRESSURE_PROCESSING_H

#include <stdint.h>
#include "pressure_fit.h"
#include <stdbool.h>

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
    bool fit_clean; /* residuals no bigger than pp_sigma_pa() explains, and not suspect */
    /* The window holds a gap or a stuck run [SNS-PRES-10, SNS-PRES-11]: no
     * fit is clean until a whole window of new samples exists. */
    bool fit_suspect;
    bool sensor_stuck; /* the whole window is one reading, to the pascal */
    /* The rate over the newest two intervals, Pa/s: far noisier than the
     * fit's, but through a hard boost's first second the fit still holds the
     * pad and under-reads the climb. For the Mach flag only [FLT-MACH-02]. */
    float short_pdot;
} altitude_sample_t;

/* ── Ring buffer sizing ──────────────────────────────────────────── */

#define PP_RING_SIZE 64 /* power of 2; 1.3 s at 50 Hz */
#define PP_RING_MASK (PP_RING_SIZE - 1)

#define PP_CAL_SAMPLES 10 /* [FLT-BOOT-08] */

/* The pressure filter's time constant. On a steady climb or descent the
 * filtered altitude trails the rocket by its rate times this. */
#define PP_FILTER_TAU_MS 500

void pp_init(void);

/* For tests: running against this ground, with no calibration. */
void pp_test_prime(int32_t ground_pressure_pa);

/* Until PP_CAL_SAMPLES readings have arrived, no altitude comes out. */
void pp_start_cal(void);
bool pp_cal_done(void);

int32_t pp_ground_pressure(void);

/* ── The ground reference [GND-CAL-01..07] ────────────────────────
 *
 * A 5 s boxcar mean of the filtered pressure on the pad, frozen (never
 * snapped) at launch. A boxcar, because it forgets: the frozen value holds
 * nothing older than the window. Pressure, not altitude, because altitude is
 * clamped at zero and its mean would carry the clamp's bias. */

/* A sample this far from the reference is not the pad, and is left out:
 * otherwise the mean follows the first second of a climb up. 50 Pa is about
 * 4 m, far above sensor noise and far faster than weather [GND-CAL-03]. */
#define PP_GROUND_MAX_DEV_PA 50

#define PP_GROUND_WINDOW_MS 5000
#define PP_GROUND_BLOCK_MS 250
#define PP_GROUND_BLOCKS (PP_GROUND_WINDOW_MS / PP_GROUND_BLOCK_MS)

/* Calibration starts tracking and pp_ground_freeze_before() stops it; this
 * is for tests that want it stopped. */
void pp_ground_track(bool enabled);

/* Freeze the reference to the mean of the blocks that ended before t_ms
 * [GND-CAL-04]. Launch passes the first sample of the rise: a reference frozen
 * at detection, a second or more later, has averaged in the start of the
 * climb, and reads every altitude low. With no such block the reference is
 * kept as it is. Returns false, and pp_ground_degraded() says so, when the
 * blocks span less than a second of the pad. */
bool pp_ground_freeze_before(uint32_t t_ms);
bool pp_ground_tracking(void);

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

/* [SNS-PRES-11] An interval this long is a gap: longer than a flash stall
 * (73 ms) and a sample together, so a stall is never one. A design constant. */
#define PP_GAP_US 250000u

/* ── Recent history [FLT-BROWN-02] ────────────────────────────────
 *
 * The median's output since power-on, in every state, calibration included:
 * brownout recovery decides before calibration starts, and must decide from
 * real readings. The fit's window and more at 50 Hz [FLT-RATE-01]:
 * a window short of samples is a noisier fit. */
#define PP_HIST_SIZE 128 /* power of 2 */

/* The oldest and newest times in the history; false while it is empty. */
bool pp_history_span(uint32_t *oldest_ms, uint32_t *newest_ms);

/* The median of the history's readings stamped from from_ms to to_ms, if at
 * least min_n of them are. */
bool pp_history_median(uint32_t from_ms, uint32_t to_ms, int min_n, int32_t *out_pa);

/* Produce altitude in flight without calibrating, against the pad marker's
 * ground pressure: calibrating in the air would call the current height zero.
 * The filter starts at start_pa, and the ground reference stays frozen. */
void pp_resume_flight(int32_t ground_pa, int32_t start_pa);

/* ── Producer: the HAL, the simulator and the tests ──────────────── */

void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms);

/* [SNS-PRES-08] The same, stamped to the microsecond by the hardware timer at
 * the reading's conversion. Milliseconds are this over 1000, the clock
 * hal_time_ms() reads. */
void pp_feed_us(int32_t raw_pressure_pa, uint64_t timestamp_us);

/* ── Consumer: the flight software ───────────────────────────────── */

/* The oldest sample in the ring; false when it is empty. */
bool pp_read(altitude_sample_t *out);

/* ── For telemetry, the log and the bench ────────────────────────── */

int32_t pp_last_raw_pa(void);

/* The raw reading of the sample pp_read() last returned: what the flight log
 * records beside it, so a log can be replayed through this layer. */
int32_t pp_last_read_raw_pa(void);
int32_t pp_last_filtered_pa(void);

/* The newest sample's fit, as the detectors saw it, for the bench. */
pfit_t pp_last_fit(bool *suspect, bool *stuck);

/* ── The conversions ─────────────────────────────────────────────── */

int32_t pp_filter_pressure(int32_t raw_pressure, uint32_t dt_ms);
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa);
int32_t pp_pressure_to_height_cm(int32_t pressure_pa, int32_t ground_pressure_pa);

/* [FLT-AIR-01, DD-079] What turns a speed from the altitude formula into the
 * speed the pad's air would give the same canopy: the atmosphere's slope over
 * the formula's, times sqrt(rho / rho_pad). 1 at the pad; 0.22 at 30 km over
 * a sea-level pad. Temperatures are the 1976 US Standard Atmosphere's at each
 * pressure. */
float pp_air_scale(int32_t pressure_pa, int32_t ground_pressure_pa);

#endif /* PRESSURE_PROCESSING_H */
