/*
 * From raw pressure readings to the samples the flight software decides on
 * [SNS-EST-01, DD-085].
 *
 * The HAL feeds each reading with the time it was measured; the flight
 * software reads one sample per reading. A sample carries the filtered state
 * in pressure terms, which every decision uses, and a height and speed for
 * the operator.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_PROCESSING_H
#define PRESSURE_PROCESSING_H

#include "estimator.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t timestamp_ms;
    uint32_t timestamp_us; /* the same instant; wraps, so differences only */
    int32_t raw_pa;

    /* The filtered state, of the estimator the flight obeys. rate and curve
     * are of ln(pressure): a rate of -1/H per metre per second of climb, H
     * the air's scale height. */
    float pressure_pa;
    float rate;
    float curve;
    float rate_sigma;
    float log_sigma; /* of ln(pressure) */
    float noise_pa;
    bool explains; /* the estimator's model accounts for the readings [SNS-EST-08] */

    /* Every estimator's state at this reading, in estimator_at()'s order
     * [SNS-EST-07]. */
    estimate_t by_estimator[ESTIMATORS_MAX];

    bool suspect; /* a gap or a stuck sensor within the last second [SNS-PRES-10, SNS-PRES-11] */
    bool sensor_stuck;
    bool risen; /* this reading is more than 50 cm above the pad [FLT-LAUNCH-03] */

    /* For the operator: relative to the pad, never clamped [SNS-ALT-01]. */
    int32_t altitude_cm;
    int32_t speed_cms; /* up is positive */
    int32_t accel_cms2;
} pp_sample_t;

#define PP_RING_SIZE 64
#define PP_CAL_SAMPLES 10 /* [FLT-BOOT-08] */

void pp_init(void);
/* Which estimator the samples carry as the filtered state, by estimator_at()'s
 * index [SNS-EST-06]. pp_init() chooses the default. */
void pp_obey(uint8_t estimator);
uint8_t pp_obeyed(void);
/* The sensor's own temperature, for the estimators' first reading. */
void pp_note_temperature(float temperature_c);
/* The board has pulsed a pyro channel [SNS-EST-09]. */
void pp_note_pulse(void);
void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms);
void pp_feed_us(int32_t raw_pressure_pa, uint64_t timestamp_us); /* [SNS-PRES-08] */
int pp_available(void);
bool pp_read(pp_sample_t *out);

/* ── Calibration and the ground reference ─────────────────────────── */

void pp_start_cal(void);
bool pp_cal_done(void);
int32_t pp_ground_pressure(void);
bool pp_ground_tracking(void);
bool pp_ground_freeze_before(uint32_t t_ms);
bool pp_ground_degraded(void);
uint32_t pp_ground_window_ms(void);
uint32_t pp_ground_rejecting_ms(uint32_t now_ms);
void pp_ground_reseed(void);
uint32_t pp_ground_reseeds(void);

/* Samples against a ground known beforehand, without calibrating
 * [FLT-BROWN-06]. */
void pp_resume_flight(int32_t ground_pa);

/* ── The estimate before calibration, for the resume decision ─────── */

/* False until the estimators have run for min_ms on readings. */
bool pp_estimate_after(uint32_t min_ms, float *pressure_pa, float *rate);

/* ── Status ───────────────────────────────────────────────────────── */

int32_t pp_last_raw_pa(void);
int32_t pp_last_read_raw_pa(void); /* of the sample pp_read() last returned [DAT-08] */
int32_t pp_last_filtered_pa(void);
float pp_noise_pa(void);
bool pp_newest(pp_sample_t *out);  /* the newest sample produced, read or not */
uint32_t pp_time_of_rise_ms(void); /* the first reading of the present run of risen ones */

#endif
