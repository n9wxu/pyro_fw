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
    float raw_altitude_m; /* the reading's altitude (atmosphere.h) */
    float raw_height_m;   /* and its height above the pad */

    /* The filtered state, of the estimator the flight obeys: above the pad,
     * in the pad's air, never clamped [SNS-ALT-01, DD-114]. */
    float pressure_pa; /* of the filtered altitude, for the log */
    float height_m;
    float speed; /* m/s, up is positive */
    float accel;
    float speed_sigma;
    float pad_ratio; /* sqrt(rho / rho_pad) here [FLT-AIR-01] */
    bool explains;   /* the estimator's model accounts for the readings [SNS-EST-08] */

    /* Every estimator's state at this reading, in estimator_at()'s order
     * [SNS-EST-07]. */
    estimate_t by_estimator[ESTIMATORS_MAX];

    bool suspect; /* a gap or a stuck sensor within the last second [SNS-PRES-10, SNS-PRES-11] */
    bool sensor_stuck;
    bool risen; /* this reading is more than 50 cm above the pad [FLT-LAUNCH-03] */
} pp_sample_t;

/* Samples between intake and the flight's steps. The collector holds four
 * cycles and the flight steps up to eight a period [FLT-RATE-06], so no more
 * than four ever wait. */
#define PP_RING_SIZE 16
#define PP_CAL_SAMPLES 10 /* [FLT-BOOT-08] */

void pp_init(void);
/* Which estimator the samples carry as the filtered state, by estimator_at()'s
 * index [SNS-EST-06]. pp_init() chooses the default. */
void pp_obey(uint8_t estimator);
uint8_t pp_obeyed(void);
/* The sensor's own temperature: on the pad, the pad's air [DD-114]. */
void pp_note_temperature(float temperature_c);
/* Heights in the standard atmosphere, not the pad's air [CFG-08]. */
void pp_use_standard_air(bool standard);
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
void pp_resume_flight(int32_t ground_pa, float pad_k);
float pp_pad_k(void); /* the pad's temperature the heights are in; 0: the standard's */
/* A pressure's height above the pad, in the pad's air, an altitude's, and
 * the pressure at a height. */
float pp_height_m(float pressure_pa);
float pp_altitude_height_m(float altitude_m);
float pp_pressure_at_m(float height_m);

/* ── The estimate before calibration, for the resume decision ─────── */

/* False until the estimators have run for min_ms on readings. */
bool pp_estimate_after(uint32_t min_ms, float *altitude_m, float *speed);

/* ── Status ───────────────────────────────────────────────────────── */

int32_t pp_last_raw_pa(void);
int32_t pp_last_read_raw_pa(void); /* of the sample pp_read() last returned [DAT-08] */
int32_t pp_last_filtered_pa(void);
float pp_noise_pa(void);
bool pp_newest(pp_sample_t *out);  /* the newest sample produced, read or not */
uint32_t pp_time_of_rise_ms(void); /* the first reading of the present run of risen ones */

#endif
