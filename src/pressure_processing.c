/*
 * See pressure_processing.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_processing.h"
#include "atmosphere.h"
#include "ground_reference.h"
#include "pressure_estimator.h"
#include <math.h>
#include <string.h>

#define RING_MASK (PP_RING_SIZE - 1)
#define RISE_M 0.5f           /* [FLT-LAUNCH-03] */
#define GAP_US 250000u        /* [SNS-PRES-11] */
#define SUSPECT_US 1000000u   /* decisions wait this long after a gap or a stuck run */
#define STUCK_SPAN_US 950000u /* [SNS-PRES-10] a second of one reading, less the sampling's jitter */
#define STUCK_MIN_READINGS 8
/* A run of one reading this long is already not believed: a working sensor's
 * noise ends such a run within a few readings, a failed one never does. */
#define REPEATING_READINGS 8
#define SMOOTH_SIGMAS 4.0f /* [FLT-MACH-03] */
#define SMOOTH_US 1000000u
#define RECENT 3 /* readings kept for the short rate */

typedef enum { PP_IDLE, PP_CALIBRATING, PP_RUNNING } pp_state_t;

static struct {
    pp_state_t state;
    int32_t cal[PP_CAL_SAMPLES];
    int cal_count;

    pest_t estimator;
    pest_t estimator_at_run_start; /* as it stood when the present reading first appeared */
    uint32_t estimator_started_us;
    gref_t ground;

    struct {
        int32_t pa[RECENT];
        uint32_t us[RECENT];
        uint8_t n;
    } recent;

    bool have_reading;
    uint32_t last_us;
    int32_t stuck_value;
    uint32_t stuck_since_us;
    uint16_t stuck_readings;
    bool suspect;
    bool repeating;
    uint32_t suspect_until_us;
    bool rough;
    uint32_t rough_until_us;

    bool risen;
    uint32_t rise_ms;

    int32_t last_raw, last_filtered, last_read_raw;

    pp_sample_t ring[PP_RING_SIZE];
    uint8_t head, tail, count;
} pp;

void pp_init(void) {
    memset(&pp, 0, sizeof(pp));
}

/* ── Calibration ──────────────────────────────────────────────────── */

static void sort_ascending(int32_t *v, int n) {
    for (int i = 1; i < n; i++) {
        int32_t x = v[i];
        int j = i - 1;
        for (; j >= 0 && v[j] > x; j--)
            v[j + 1] = v[j];
        v[j + 1] = x;
    }
}

static int32_t calibration_median(void) {
    int32_t v[PP_CAL_SAMPLES];
    memcpy(v, pp.cal, sizeof(v));
    sort_ascending(v, PP_CAL_SAMPLES);
    return (v[PP_CAL_SAMPLES / 2 - 1] + v[PP_CAL_SAMPLES / 2]) / 2;
}

void pp_start_cal(void) {
    pp.cal_count = 0;
    pp.state = PP_CALIBRATING;
}

bool pp_cal_done(void) {
    return pp.state == PP_RUNNING;
}

static void calibrate_with(int32_t raw_pa) {
    pp.cal[pp.cal_count++] = raw_pa;
    if (pp.cal_count < PP_CAL_SAMPLES)
        return;
    gref_seed(&pp.ground, calibration_median());
    pp.state = PP_RUNNING;
}

void pp_resume_flight(int32_t ground_pa) {
    gref_hold(&pp.ground, ground_pa);
    pp.state = PP_RUNNING;
}

/* ── The sensor's own faults ──────────────────────────────────────── */

static void hold_suspect_from(uint32_t from_us) {
    pp.suspect = true;
    pp.suspect_until_us = from_us + SUSPECT_US;
}

static bool watch_for_gap_and_stuck(int32_t raw_pa, uint32_t us) {
    if (pp.have_reading && us - pp.last_us > GAP_US)
        hold_suspect_from(us);
    if (!pp.have_reading || raw_pa != pp.stuck_value) {
        pp.stuck_value = raw_pa;
        pp.stuck_since_us = us;
        pp.stuck_readings = 0;
    }
    pp.stuck_readings++;
    pp.have_reading = true;
    pp.last_us = us;

    bool stuck = pp.stuck_readings >= STUCK_MIN_READINGS && us - pp.stuck_since_us >= STUCK_SPAN_US;
    if (stuck)
        hold_suspect_from(us);
    if (pp.suspect && (int32_t)(us - pp.suspect_until_us) >= 0)
        pp.suspect = false;
    pp.repeating = pp.stuck_readings >= REPEATING_READINGS;
    return stuck;
}

static bool smooth_after(float innovation_sigmas, uint32_t us) {
    if (fabsf(innovation_sigmas) > SMOOTH_SIGMAS) {
        pp.rough = true;
        pp.rough_until_us = us + SMOOTH_US;
    } else if (pp.rough && (int32_t)(us - pp.rough_until_us) >= 0) {
        pp.rough = false;
    }
    return !pp.rough;
}

/* ── The raw readings' own rate ───────────────────────────────────── */

static void remember(int32_t raw_pa, uint32_t us) {
    for (int i = 0; i < RECENT - 1; i++) {
        pp.recent.pa[i] = pp.recent.pa[i + 1];
        pp.recent.us[i] = pp.recent.us[i + 1];
    }
    pp.recent.pa[RECENT - 1] = raw_pa;
    pp.recent.us[RECENT - 1] = us;
    if (pp.recent.n < RECENT)
        pp.recent.n++;
}

static float short_rate(void) {
    if (pp.recent.n < RECENT || pp.recent.pa[0] <= 0 || pp.recent.pa[RECENT - 1] <= 0)
        return 0.0f;
    int32_t dt_us = (int32_t)(pp.recent.us[RECENT - 1] - pp.recent.us[0]);
    if (dt_us <= 0)
        return 0.0f;
    return logf((float)pp.recent.pa[RECENT - 1] / (float)pp.recent.pa[0]) * 1e6f / (float)dt_us;
}

/* ── Samples ──────────────────────────────────────────────────────── */

static pp_sample_t *push_sample(void) {
    pp_sample_t *s = &pp.ring[pp.head];
    memset(s, 0, sizeof(*s));
    pp.head = (uint8_t)((pp.head + 1u) & RING_MASK);
    if (pp.count < PP_RING_SIZE)
        pp.count++;
    else
        pp.tail = (uint8_t)((pp.tail + 1u) & RING_MASK);
    return s;
}

static void note_rise(pp_sample_t *s, float ground_pa) {
    s->risen = (float)s->raw_pa < atmos_pressure_above_pa(ground_pa, RISE_M);
    if (s->risen && !pp.risen)
        pp.rise_ms = s->timestamp_ms;
    pp.risen = s->risen;
}

static void for_the_operator(pp_sample_t *s, float ground_pa) {
    float scale_height_m = atmos_scale_height_m(s->pressure_pa);
    s->altitude_cm = (int32_t)(atmos_height_above_m(s->pressure_pa, ground_pa) * 100.0f);
    s->speed_cms = (int32_t)(-s->rate * scale_height_m * 100.0f);
    s->accel_cms2 = (int32_t)(-s->curve * scale_height_m * 100.0f);
}

static void produce_sample(int32_t raw_pa, uint32_t ms, uint32_t us, bool stuck) {
    pest_estimate_t e = pest_estimate(&pp.estimator);
    pp.last_filtered = (int32_t)(e.pressure_pa + 0.5f);
    gref_feed(&pp.ground, pp.last_filtered, ms);
    float ground_pa = (float)gref_pressure(&pp.ground);

    pp_sample_t *s = push_sample();
    s->timestamp_ms = ms;
    s->timestamp_us = us;
    s->raw_pa = raw_pa;
    s->pressure_pa = e.pressure_pa;
    s->rate = e.rate;
    s->curve = e.curve;
    s->rate_sigma = e.rate_sigma;
    s->log_sigma = e.log_sigma;
    s->noise_pa = e.noise_pa;
    s->short_rate = short_rate();
    s->smooth = smooth_after(e.innovation_sigmas, us);
    s->suspect = pp.suspect || pp.repeating;
    s->sensor_stuck = stuck;
    note_rise(s, ground_pa);
    for_the_operator(s, ground_pa);
}

void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms) {
    pp_feed_us(raw_pressure_pa, (uint64_t)timestamp_ms * 1000u);
}

void pp_feed_us(int32_t raw_pressure_pa, uint64_t timestamp_us) {
    uint32_t ms = (uint32_t)(timestamp_us / 1000u);
    uint32_t us = (uint32_t)timestamp_us;
    pp.last_raw = raw_pressure_pa;
    remember(raw_pressure_pa, us);
    bool stuck = watch_for_gap_and_stuck(raw_pressure_pa, us);

    /* A sensor repeating itself gives no data [SNS-PRES-10]: the estimate
     * carries on from what it knew, and takes the readings up again when they
     * move. */
    if (!pp.estimator.started) {
        pest_start(&pp.estimator, raw_pressure_pa, us, PEST_NOISE_FLOOR_PA);
        pp.estimator_started_us = us;
    } else if (!pp.repeating) {
        pest_update(&pp.estimator, raw_pressure_pa, us);
        if (pp.stuck_readings == 1)
            pp.estimator_at_run_start = pp.estimator;
    } else if (pp.stuck_readings == REPEATING_READINGS) {
        pp.estimator = pp.estimator_at_run_start; /* the repeats it had taken as readings are taken back */
    }

    if (pp.state == PP_CALIBRATING)
        calibrate_with(raw_pressure_pa);
    else if (pp.state == PP_RUNNING)
        produce_sample(raw_pressure_pa, ms, us, stuck);
}

int pp_available(void) {
    return pp.count;
}

bool pp_read(pp_sample_t *out) {
    if (pp.count == 0)
        return false;
    *out = pp.ring[pp.tail];
    pp.last_read_raw = out->raw_pa;
    pp.tail = (uint8_t)((pp.tail + 1u) & RING_MASK);
    pp.count--;
    return true;
}

/* ── The ground reference ─────────────────────────────────────────── */

int32_t pp_ground_pressure(void) {
    return gref_pressure(&pp.ground);
}

bool pp_ground_tracking(void) {
    return pp.ground.tracking;
}

bool pp_ground_freeze_before(uint32_t t_ms) {
    return gref_freeze_before(&pp.ground, t_ms);
}

bool pp_ground_degraded(void) {
    return gref_degraded(&pp.ground);
}

uint32_t pp_ground_window_ms(void) {
    return gref_window_ms(&pp.ground);
}

uint32_t pp_ground_rejecting_ms(uint32_t now_ms) {
    return gref_rejecting_ms(&pp.ground, now_ms);
}

void pp_ground_reseed(void) {
    gref_reseed(&pp.ground, pp.last_filtered);
}

uint32_t pp_ground_reseeds(void) {
    return pp.ground.reseeds;
}

/* ── Status ───────────────────────────────────────────────────────── */

bool pp_estimate_after(uint32_t min_ms, float *pressure_pa, float *rate) {
    if (!pp.estimator.started || pp.estimator.t_us - pp.estimator_started_us < min_ms * 1000u)
        return false;
    pest_estimate_t e = pest_estimate(&pp.estimator);
    *pressure_pa = e.pressure_pa;
    *rate = e.rate;
    return true;
}

int32_t pp_last_raw_pa(void) {
    return pp.last_raw;
}

int32_t pp_last_read_raw_pa(void) {
    return pp.last_read_raw;
}

int32_t pp_last_filtered_pa(void) {
    return pp.last_filtered;
}

bool pp_newest(pp_sample_t *out) {
    if (pp.state != PP_RUNNING || !pp.have_reading)
        return false;
    *out = pp.ring[(pp.head + PP_RING_SIZE - 1u) & RING_MASK];
    return true;
}

float pp_noise_pa(void) {
    return pp.estimator.started ? pest_estimate(&pp.estimator).noise_pa : PEST_NOISE_FLOOR_PA;
}

uint32_t pp_time_of_rise_ms(void) {
    return pp.rise_ms;
}
