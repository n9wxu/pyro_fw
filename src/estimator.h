/*
 * The estimator interface [SNS-EST-06..09, DD-092].
 *
 * An estimator turns the readings, as altitudes (atmosphere.h), into the
 * filtered state every decision is made on. The build carries one
 * (estimator_constacc.c), and the flight path calls it directly: there is
 * no choice of estimator to make between launch and landing [FLT-RT-07].
 *
 * Whatever the estimator's own model, it reports in the same terms: the
 * filtered altitude, its speed and acceleration, and whether its model
 * explains the readings. Its apogee is not decided while it does not
 * [DD-114].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ESTIMATOR_H
#define ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float altitude_m;
    float speed;       /* m/s of altitude, up is positive */
    float accel;       /* m/s^2 */
    float speed_sigma; /* one standard deviation of speed */
    float altitude_sigma;
    float noise_m; /* the sensor noise being tracked, as altitude here */
    /* The model accounts for the readings, and has had a second of them to
     * account for. */
    bool explains;
} estimate_t;

/* m_per_pa is the reading's metres of altitude per pascal: the sensor's
 * noise is in pascals. pad_temp_k is the sensor's own temperature at the
 * first reading, or 0 when it is not known. */
void estimator_start(float altitude_m, float m_per_pa, uint32_t t_us, float pad_temp_k);
void estimator_reading(float altitude_m, float m_per_pa, uint32_t t_us);
/* Time has passed and the sensor gave nothing usable. */
void estimator_no_reading(uint32_t t_us);
/* The board has just pulsed a pyro channel. */
void estimator_pulse(uint32_t t_us);
void estimator_estimate(estimate_t *out);

/* The whole of the estimator's state, so a caller can put it back as it
 * was [SNS-PRES-10]. */
#define ESTIMATOR_STATE_MAX 192
void estimator_save(uint8_t to[ESTIMATOR_STATE_MAX]);
void estimator_restore(const uint8_t from[ESTIMATOR_STATE_MAX]);

/* The estimators this build carries, as config.ini and the log name them:
 * at most 8 characters. The first is the default. */
#define ESTIMATORS_MAX 1
static inline uint8_t estimator_count(void) {
    return ESTIMATORS_MAX;
}
const char *estimator_name(uint8_t index);
/* The index of the estimator of this name, or of the default. */
uint8_t estimator_index(const char *name);

#endif
