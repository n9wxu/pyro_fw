/*
 * See pressure_processing.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_processing.h"
#include "atmosphere.h"
#include "ground_reference.h"
#include "altitude_filter.h"
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
#define KELVIN 273.15f

typedef enum { PP_IDLE, PP_CALIBRATING, PP_RUNNING } pp_state_t;

static struct {
    pp_state_t state;
    int32_t cal[PP_CAL_SAMPLES];
    int cal_count;

    uint8_t obeyed;
    bool estimators_started;
    uint32_t estimators_started_us;
    /* Each estimator as it stood when the present reading first appeared. */
    uint8_t at_run_start[ESTIMATORS_MAX][ESTIMATOR_STATE_MAX];
    float sensor_k; /* 0: not known */
    float pad_k;    /* the sensor's temperature on the pad, until the ground is frozen */
    bool standard_air;
    gref_t ground;
    air_t air;
    float air_pad_k; /* the pad_k the air was made from */

    bool have_reading;
    uint32_t last_us;
    int32_t stuck_value;
    uint32_t stuck_since_us;
    uint16_t stuck_readings;
    bool suspect;
    bool repeating;
    uint32_t suspect_until_us;

    bool risen;
    uint32_t rise_ms;

    int32_t last_raw, last_filtered, last_read_raw;

    pp_sample_t ring[PP_RING_SIZE];
    uint8_t head, tail, count;
} pp;

void pp_init(void) {
    memset(&pp, 0, sizeof(pp));
}

void pp_obey(uint8_t estimator) {
    pp.obeyed = estimator < estimator_count() ? estimator : 0;
}

uint8_t pp_obeyed(void) {
    return pp.obeyed;
}

void pp_note_temperature(float temperature_c) {
    pp.sensor_k = temperature_c + KELVIN;
    if (pp.ground.tracking)
        pp.pad_k = pp.sensor_k;
}

void pp_use_standard_air(bool standard) {
    pp.standard_air = standard;
}

float pp_pad_k(void) {
    return pp.standard_air ? 0.0f : pp.pad_k;
}

/* The pad's pressure and temperature as they stand: both move until the
 * ground is frozen at launch. Not inlined: the flight-RAM proof cuts its
 * call to air_over_pad by name. */
static void __attribute__((noinline)) follow_the_pad(void) {
    float pad_pa = (float)gref_pressure(&pp.ground);
    float pad_k = pp_pad_k();
    if (pad_pa == pp.air.pad_pa && pad_k == pp.air_pad_k)
        return;
    pp.air = air_over_pad(pad_pa, pad_k);
    pp.air_pad_k = pad_k;
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

void pp_resume_flight(int32_t ground_pa, float pad_k) {
    gref_hold(&pp.ground, ground_pa);
    pp.pad_k = pad_k;
    follow_the_pad();
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

/* ── The estimators ───────────────────────────────────────────────── */

static void remember_estimators(void) {
    for (uint8_t i = 0; i < estimator_count(); i++)
        memcpy(pp.at_run_start[i], estimator_at(i)->state, estimator_at(i)->state_size);
}

/* The first reading may already be the start of a run. */
static void start_estimators(float altitude_m, float m_per_pa, uint32_t us) {
    for (uint8_t i = 0; i < estimator_count(); i++)
        estimator_at(i)->start(altitude_m, m_per_pa, us, pp.sensor_k);
    pp.estimators_started = true;
    pp.estimators_started_us = us;
    remember_estimators();
}

static void put_estimators_back(void) {
    for (uint8_t i = 0; i < estimator_count(); i++)
        memcpy(estimator_at(i)->state, pp.at_run_start[i], estimator_at(i)->state_size);
}

/* A sensor repeating itself gives no data [SNS-PRES-10]: the repeats the
 * estimators had taken as readings are taken back, and they carry on from
 * what they knew until the readings move. */
static void feed_estimators(float altitude_m, float m_per_pa, uint32_t us) {
    if (!pp.estimators_started) {
        start_estimators(altitude_m, m_per_pa, us);
        return;
    }
    if (pp.stuck_readings == REPEATING_READINGS)
        put_estimators_back();
    for (uint8_t i = 0; i < estimator_count(); i++) {
        if (pp.repeating)
            estimator_at(i)->no_reading(us);
        else
            estimator_at(i)->reading(altitude_m, m_per_pa, us);
    }
    if (pp.stuck_readings == 1)
        remember_estimators();
}

void pp_note_pulse(void) {
    if (!pp.estimators_started)
        return;
    for (uint8_t i = 0; i < estimator_count(); i++)
        estimator_at(i)->pulse(pp.last_us);
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

static void note_rise(pp_sample_t *s) {
    s->risen = s->raw_height_m > RISE_M;
    if (s->risen && !pp.risen)
        pp.rise_ms = s->timestamp_ms;
    pp.risen = s->risen;
}

/* [DD-114] The obeyed estimator's altitude, as height above the pad and
 * true speeds in the pad's air. */
static void in_the_pads_air(pp_sample_t *s, const estimate_t *e) {
    float scale = air_scale(&pp.air, e->altitude_m);
    s->height_m = air_height_m(&pp.air, e->altitude_m);
    s->speed = e->speed * scale;
    s->accel = e->accel * scale;
    s->speed_sigma = e->speed_sigma * scale;
    s->pad_ratio = air_pad_ratio(&pp.air, e->altitude_m);
    s->raw_height_m = air_height_m(&pp.air, s->raw_altitude_m);
}

static void produce_sample(int32_t raw_pa, float raw_altitude_m, uint32_t ms, uint32_t us, bool stuck) {
    pp_sample_t *s = push_sample();
    for (uint8_t i = 0; i < estimator_count(); i++)
        estimator_at(i)->estimate(&s->by_estimator[i]);
    const estimate_t *e = &s->by_estimator[pp.obeyed];
    s->pressure_pa = atmos_pressure_pa(e->altitude_m);
    pp.last_filtered = (int32_t)(s->pressure_pa + 0.5f);
    gref_feed(&pp.ground, pp.last_filtered, ms);
    follow_the_pad();

    s->timestamp_ms = ms;
    s->timestamp_us = us;
    s->raw_pa = raw_pa;
    s->raw_altitude_m = raw_altitude_m;
    s->explains = e->explains;
    s->suspect = pp.suspect || pp.repeating;
    s->sensor_stuck = stuck;
    in_the_pads_air(s, e);
    note_rise(s);
}

void pp_feed(int32_t raw_pressure_pa, uint32_t timestamp_ms) {
    pp_feed_us(raw_pressure_pa, (uint64_t)timestamp_ms * 1000u);
}

void pp_feed_us(int32_t raw_pressure_pa, uint64_t timestamp_us) {
    uint32_t ms = (uint32_t)(timestamp_us / 1000u);
    uint32_t us = (uint32_t)timestamp_us;
    /* No altitude belongs to it. */
    if (raw_pressure_pa <= 0)
        return;
    pp.last_raw = raw_pressure_pa;
    bool stuck = watch_for_gap_and_stuck(raw_pressure_pa, us);
    float altitude_m = atmos_altitude_m((float)raw_pressure_pa);
    feed_estimators(altitude_m, atmos_m_per_pa(altitude_m, (float)raw_pressure_pa), us);

    if (pp.state == PP_CALIBRATING)
        calibrate_with(raw_pressure_pa);
    else if (pp.state == PP_RUNNING)
        produce_sample(raw_pressure_pa, altitude_m, ms, us, stuck);
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

float pp_height_m(float pressure_pa) {
    return pp_altitude_height_m(atmos_altitude_m(pressure_pa));
}

float pp_altitude_height_m(float altitude_m) {
    return air_height_m(&pp.air, altitude_m);
}

float pp_pressure_at_m(float height_m) {
    return atmos_pressure_pa(air_altitude_m(&pp.air, height_m));
}

int32_t pp_ground_pressure(void) {
    return gref_pressure(&pp.ground);
}

bool pp_ground_tracking(void) {
    return pp.ground.tracking;
}

bool pp_ground_freeze_before(uint32_t t_ms) {
    bool ok = gref_freeze_before(&pp.ground, t_ms);
    follow_the_pad();
    return ok;
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

static estimate_t obeyed_estimate(void) {
    estimate_t e;
    estimator_at(pp.obeyed)->estimate(&e);
    return e;
}

bool pp_estimate_after(uint32_t min_ms, float *altitude_m, float *speed) {
    if (!pp.estimators_started || pp.last_us - pp.estimators_started_us < min_ms * 1000u)
        return false;
    estimate_t e = obeyed_estimate();
    *altitude_m = e.altitude_m;
    *speed = e.speed;
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
    if (!pp.estimators_started)
        return AF_NOISE_FLOOR_PA;
    estimate_t e = obeyed_estimate();
    return e.noise_m / atmos_m_per_pa(e.altitude_m, atmos_pressure_pa(e.altitude_m));
}

uint32_t pp_time_of_rise_ms(void) {
    return pp.rise_ms;
}
