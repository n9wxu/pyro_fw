/*
 * Pressure processing layer — filter, altitude conversion, ring buffer.
 *
 * See pressure_processing.h for architecture overview.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_processing.h"
#include <string.h>
#include <math.h>

/* ── Internal state ───────────────────────────────────────────────── */

typedef enum {
    PP_IDLE,        /* discarding samples (before calibration starts) */
    PP_CALIBRATING, /* accumulating ground pressure */
    PP_RUNNING,     /* filtering and producing altitude */
} pp_state_t;

static struct {
    pp_state_t state;

    /* Calibration */
    int64_t cal_sum;
    int cal_count;
    int32_t ground_pressure;

    /* IIR filter state */
    int32_t filtered_pressure;
    bool filter_initialized;
    uint32_t last_timestamp;

    /* Debug copies */
    int32_t last_raw;
    int32_t last_filtered;

    /* Altitude ring buffer */
    altitude_sample_t ring[PP_RING_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} pp;

/* ── IIR pressure filter ─────────────────────────────────────────── */

/* [SNS-PRES-02..04] First-order IIR with minimum step to prevent stall.
 * Time constant τ = 500ms. See IMPLEMENTATION.md "Pressure Filter".
 *
 * Correct operation requires monotonically increasing timestamps.
 * The data flow (HAL pres_tick → pp_feed → ring) guarantees this
 * because the HAL produces samples in chronological order and
 * pp_feed() is called once per raw sample. */
int32_t pp_filter_pressure(int32_t raw_pressure, uint32_t dt_ms) {
    if (!pp.filter_initialized) {
        pp.filtered_pressure = raw_pressure;
        pp.filter_initialized = true;
        return raw_pressure;
    }
    int32_t diff = raw_pressure - pp.filtered_pressure;
    int32_t alpha = (dt_ms * 1000) / (500 + dt_ms);
    int32_t step = (diff * alpha) / 1000;
    if (step == 0 && diff != 0)
        step = (diff > 0) ? 1 : -1;
    pp.filtered_pressure += step;
    return pp.filtered_pressure;
}

/* ── Altitude conversion ──────────────────────────────────────────── */

#define MAX_ALTITUDE_CM 800000

/* [SNS-ALT-01..03] Hypsometric formula — exact inverse of the ISA
 * troposphere pressure model.
 *
 *   h = 44330 × (1 − (P/P₀)^(1/5.2561))
 */
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    if (pressure_pa <= 0 || ground_pressure_pa <= 0)
        return 0;
    float ratio = (float)pressure_pa / (float)ground_pressure_pa;
    float alt_m = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
    int32_t alt_cm = (int32_t)(alt_m * 100.0f);
    if (alt_cm > MAX_ALTITUDE_CM)
        alt_cm = MAX_ALTITUDE_CM;
    if (alt_cm < 0)
        alt_cm = 0;
    return alt_cm;
}

/* ── Ring buffer helpers ──────────────────────────────────────────── */

static void ring_push(int32_t altitude_cm, uint32_t timestamp_ms) {
    pp.ring[pp.head].altitude_cm = altitude_cm;
    pp.ring[pp.head].timestamp_ms = timestamp_ms;
    pp.head = (pp.head + 1) & PP_RING_MASK;
    if (pp.count < PP_RING_SIZE) {
        pp.count++;
    } else {
        /* Overrun: drop oldest */
        pp.tail = (pp.tail + 1) & PP_RING_MASK;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void pp_init(void) {
    memset(&pp, 0, sizeof(pp));
    pp.state = PP_IDLE;
}

void pp_test_prime(int32_t ground_pressure_pa) {
    pp.state = PP_RUNNING;
    pp.ground_pressure = ground_pressure_pa;
    pp.filter_initialized = false; /* first pp_feed will prime the IIR */
}

void pp_start_cal(void) {
    pp.cal_sum = 0;
    pp.cal_count = 0;
    pp.state = PP_CALIBRATING;
}

bool pp_cal_done(void) {
    return pp.state == PP_RUNNING;
}

int32_t pp_ground_pressure(void) {
    return pp.ground_pressure;
}

void pp_set_ground_pressure(int32_t pa) {
    if (pp.state == PP_RUNNING)
        pp.ground_pressure = pa;
}

void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms) {
    pp.last_raw = raw_pressure_pa;

    switch (pp.state) {
    case PP_IDLE:
        /* Discard — not calibrating yet */
        return;

    case PP_CALIBRATING:
        pp.cal_sum += raw_pressure_pa;
        pp.cal_count++;
        if (pp.cal_count >= PP_CAL_SAMPLES) {
            pp.ground_pressure = (int32_t)(pp.cal_sum / PP_CAL_SAMPLES);
            pp.filtered_pressure = pp.ground_pressure;
            pp.filter_initialized = true;
            pp.last_timestamp = timestamp_ms;
            pp.state = PP_RUNNING;
        }
        return;

    case PP_RUNNING: {
        /* Compute dt from previous sample */
        uint32_t dt = timestamp_ms - pp.last_timestamp;
        pp.last_timestamp = timestamp_ms;

        /* IIR filter */
        int32_t filtered = pp_filter_pressure(raw_pressure_pa, dt);
        pp.last_filtered = filtered;

        /* Convert to altitude */
        int32_t alt_cm = pp_pressure_to_altitude_cm(filtered, pp.ground_pressure);

        /* Push to ring */
        ring_push(alt_cm, timestamp_ms);
        return;
    }
    }
}

int pp_available(void) {
    return pp.count;
}

bool pp_read(altitude_sample_t *out) {
    if (pp.count == 0)
        return false;
    *out = pp.ring[pp.tail];
    pp.tail = (pp.tail + 1) & PP_RING_MASK;
    pp.count--;
    return true;
}

int32_t pp_last_raw_pa(void) {
    return pp.last_raw;
}

int32_t pp_last_filtered_pa(void) {
    return pp.last_filtered;
}
