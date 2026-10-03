/*
 * The estimator interface [SNS-EST-06..09, DD-092].
 *
 * An estimator turns the raw pressure readings into the filtered state every
 * decision is made on. Several are built in (estimator_table.c). All of them
 * are fed every reading; the flight obeys one, chosen by `estimator` in
 * config.ini at start-up, and logs what the others would have done.
 *
 * Whatever the estimator's own model, it reports in the same terms: the
 * filtered pressure, the rate and curvature of ln(pressure), and whether its
 * model explains the readings. Nothing is decided while it does not.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ESTIMATOR_H
#define ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float pressure_pa;
    float rate;       /* d(ln p)/dt, 1/s: negative while climbing */
    float curve;      /* d2(ln p)/dt2, 1/s^2 */
    float rate_sigma; /* one standard deviation of rate */
    float log_sigma;  /* one standard deviation of ln(pressure) */
    float noise_pa;   /* the sensor noise being tracked */
    /* The model accounts for the readings, and has had a second of them to
     * account for. */
    bool explains;
} estimate_t;

typedef struct estimator_vt {
    const char *name; /* as config.ini names it: at most 8 characters */
    /* The first reading. pad_temp_k is the sensor's own temperature then, or
     * 0 when it is not known. */
    void (*start)(int32_t reading_pa, uint32_t t_us, float pad_temp_k);
    void (*reading)(int32_t reading_pa, uint32_t t_us);
    /* Time has passed and the sensor gave nothing usable. */
    void (*no_reading)(uint32_t t_us);
    /* The board has just pulsed a pyro channel. */
    void (*pulse)(uint32_t t_us);
    void (*estimate)(estimate_t *out);
    /* The whole of the estimator's state, so a caller can put it back as it
     * was [SNS-PRES-10]. */
    void *state;
    uint16_t state_size;
} estimator_vt;

#define ESTIMATORS_MAX 3
#define ESTIMATOR_STATE_MAX 192

/* The estimators this build carries (estimator_table.c). The first is the
 * default. */
uint8_t estimator_count(void);
const estimator_vt *estimator_at(uint8_t index);
/* The index of the estimator of this name, or of the default. */
uint8_t estimator_index(const char *name);

#endif
