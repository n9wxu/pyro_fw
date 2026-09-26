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
    int32_t cal[PP_CAL_SAMPLES];
    int cal_count;
    int32_t ground_pressure;

    /* The newest three readings, oldest first [SNS-PRES-07], and the time
     * of the last median out, so none goes out twice. */
    struct {
        int32_t pa[3];
        uint32_t ts[3];
        uint8_t n;
        bool emitted;
        uint32_t last_ts;
    } med;

    /* The median's output since power-on [FLT-BROWN-02]. */
    struct {
        int32_t pa[PP_HIST_SIZE];
        uint32_t ts[PP_HIST_SIZE];
        uint8_t head;
        uint8_t n;
    } hist;

    /* IIR filter state */
    int32_t filtered_q8; /* pascals x 256 [SNS-PRES-02] */
    bool filter_initialized;
    uint32_t last_timestamp;

    /* Debug copies */
    int32_t last_raw;
    int32_t last_filtered;

    /* Ground reference: a rolling mean of the filtered pressure, in blocks so
     * the window slides without keeping every sample. 20 blocks of 250 ms is
     * 5 s in 240 bytes rather than 1 kB. */
    struct {
        bool tracking;
        int64_t sum[PP_GROUND_BLOCKS]; /* filtered Pa summed within a block */
        uint16_t n[PP_GROUND_BLOCKS];
        uint32_t start[PP_GROUND_BLOCKS]; /* when each block began */
        uint8_t cur;                      /* block being filled */
        uint32_t block_start_ms;
        uint8_t filled; /* blocks that have ever been written */
        bool degraded;  /* frozen on less than a second of the pad */
    } gnd;

    /* Altitude ring buffer */
    altitude_sample_t ring[PP_RING_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} pp;

/* ── The ground reference ─────────────────────────────────────────
 *
 * See pressure_processing.h. Blocks, not individual samples: the window only
 * has to track slow atmospheric drift, and a 250 ms step is finer than
 * anything the weather does. */

static void gnd_reset(int32_t seed_pa) {
    memset(&pp.gnd, 0, sizeof(pp.gnd));
    pp.gnd.tracking = true;
    /* Seeded with the calibration result, so the mean is usable from the
     * first sample rather than climbing out of zero. */
    pp.gnd.sum[0] = seed_pa;
    pp.gnd.n[0] = 1;
    pp.gnd.filled = 1;
}

static void gnd_feed(int32_t filtered_pa, uint32_t now_ms) {
    if (!pp.gnd.tracking) {
        return;
    }
    /* Not the pad any more. Holding the reference here rather than at the
     * launch transition is what keeps the frozen value honest: by the time
     * launch is declared at 100 ft the rocket has been moving for a second,
     * and a mean that followed it up would under-report the whole flight. */
    int32_t dev = filtered_pa - pp.ground_pressure;
    if (dev > PP_GROUND_MAX_DEV_PA || dev < -PP_GROUND_MAX_DEV_PA) {
        return;
    }
    if (pp.gnd.block_start_ms == 0) {
        pp.gnd.block_start_ms = now_ms;
        pp.gnd.start[pp.gnd.cur] = now_ms;
    }
    if (now_ms - pp.gnd.block_start_ms >= PP_GROUND_BLOCK_MS) {
        pp.gnd.cur = (uint8_t)((pp.gnd.cur + 1) % PP_GROUND_BLOCKS);
        pp.gnd.sum[pp.gnd.cur] = 0;
        pp.gnd.n[pp.gnd.cur] = 0;
        pp.gnd.block_start_ms = now_ms;
        pp.gnd.start[pp.gnd.cur] = now_ms;
        if (pp.gnd.filled < PP_GROUND_BLOCKS) {
            pp.gnd.filled++;
        }
    }
    pp.gnd.sum[pp.gnd.cur] += filtered_pa;
    pp.gnd.n[pp.gnd.cur]++;

    int64_t total = 0;
    uint32_t count = 0;
    for (int i = 0; i < PP_GROUND_BLOCKS; i++) {
        total += pp.gnd.sum[i];
        count += pp.gnd.n[i];
    }
    if (count > 0) {
        pp.ground_pressure = (int32_t)(total / (int64_t)count);
    }
}

void pp_ground_track(bool enabled) {
    pp.gnd.tracking = enabled;
}

bool pp_ground_tracking(void) {
    return pp.gnd.tracking;
}

bool pp_ground_freeze_before(uint32_t t_ms) {
    pp.gnd.tracking = false;
    int64_t total = 0;
    uint32_t count = 0;
    uint32_t span_ms = 0;
    for (int i = 0; i < PP_GROUND_BLOCKS; i++) {
        /* The block being filled has not ended; one that ended after t_ms may
         * hold the climb. */
        if (i == pp.gnd.cur || pp.gnd.n[i] == 0 || (int32_t)(t_ms - (pp.gnd.start[i] + PP_GROUND_BLOCK_MS)) < 0)
            continue;
        total += pp.gnd.sum[i];
        count += pp.gnd.n[i];
        span_ms += PP_GROUND_BLOCK_MS;
    }
    if (count > 0)
        pp.ground_pressure = (int32_t)(total / (int64_t)count);
    pp.gnd.degraded = span_ms < 1000u;
    return !pp.gnd.degraded;
}

bool pp_ground_degraded(void) {
    return pp.gnd.degraded;
}

uint32_t pp_ground_window_ms(void) {
    return (uint32_t)pp.gnd.filled * PP_GROUND_BLOCK_MS;
}

/* ── Spike rejection [SNS-PRES-07] ──────────────────────────────────
 *
 * A median of three, between the range check and the filter. A single bad
 * reading -- a flipped bit, a bus glitch -- is a launch on the pad or an
 * apogee in coast if it reaches the filter.
 *
 * The median goes out with the MIDDLE reading's time. On a monotonic signal the
 * median is exactly the middle reading, so the stage costs one sample of
 * latency and distorts nothing: stamping it with the newest reading's time
 * would shift every altitude a sample early. */

static void med_push(int32_t pa, uint32_t ts) {
    pp.med.pa[0] = pp.med.pa[1];
    pp.med.ts[0] = pp.med.ts[1];
    pp.med.pa[1] = pp.med.pa[2];
    pp.med.ts[1] = pp.med.ts[2];
    pp.med.pa[2] = pa;
    pp.med.ts[2] = ts;
    if (pp.med.n < 3)
        pp.med.n++;
}

static int32_t median3(int32_t a, int32_t b, int32_t c) {
    int32_t lo = a < b ? a : b;
    int32_t hi = a < b ? b : a;
    return c < lo ? lo : (c > hi ? hi : c);
}

/* The median out of the window, if it has one not yet given. Short of three
 * readings -- at power-on, or after a test primes the layer -- the newest
 * goes straight through. */
static bool med_out(int32_t *pa, uint32_t *ts) {
    if (pp.med.n == 0)
        return false;
    int32_t p = pp.med.pa[2];
    uint32_t t = pp.med.ts[2];
    if (pp.med.n == 3) {
        p = median3(pp.med.pa[0], pp.med.pa[1], pp.med.pa[2]);
        t = pp.med.ts[1];
    }
    if (pp.med.emitted && (int32_t)(t - pp.med.last_ts) <= 0)
        return false;
    pp.med.emitted = true;
    pp.med.last_ts = t;
    *pa = p;
    *ts = t;
    return true;
}

static void hist_push(int32_t pa, uint32_t ts) {
    pp.hist.pa[pp.hist.head] = pa;
    pp.hist.ts[pp.hist.head] = ts;
    pp.hist.head = (uint8_t)((pp.hist.head + 1u) & (PP_HIST_SIZE - 1u));
    if (pp.hist.n < PP_HIST_SIZE)
        pp.hist.n++;
}

static void sort_i32(int32_t *v, int n) {
    for (int i = 1; i < n; i++) {
        int32_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

bool pp_history_span(uint32_t *oldest_ms, uint32_t *newest_ms) {
    if (pp.hist.n == 0)
        return false;
    unsigned newest = (pp.hist.head + PP_HIST_SIZE - 1u) & (PP_HIST_SIZE - 1u);
    unsigned oldest = (pp.hist.head + PP_HIST_SIZE - pp.hist.n) & (PP_HIST_SIZE - 1u);
    *newest_ms = pp.hist.ts[newest];
    *oldest_ms = pp.hist.ts[oldest];
    return true;
}

bool pp_history_median(uint32_t from_ms, uint32_t to_ms, int min_n, int32_t *out_pa) {
    int32_t v[PP_HIST_SIZE];
    int n = 0;
    for (unsigned i = 0; i < pp.hist.n; i++) {
        unsigned k = (pp.hist.head + PP_HIST_SIZE - 1u - i) & (PP_HIST_SIZE - 1u);
        uint32_t t = pp.hist.ts[k];
        if ((int32_t)(t - from_ms) >= 0 && (int32_t)(to_ms - t) >= 0)
            v[n++] = pp.hist.pa[k];
    }
    if (n < min_n || n == 0)
        return false;
    sort_i32(v, n);
    *out_pa = (n & 1) ? v[n / 2] : (int32_t)(((int64_t)v[n / 2 - 1] + v[n / 2]) / 2);
    return true;
}

/* The ground reference from calibration's readings: their median, so one bad
 * reading cannot bias it -- and a biased reference stays biased, because the
 * tracker rejects everything 50 Pa from it [GND-CAL-03]. */
static int32_t cal_median(void) {
    int32_t v[PP_CAL_SAMPLES];
    memcpy(v, pp.cal, sizeof(v));
    sort_i32(v, PP_CAL_SAMPLES);
    return (v[PP_CAL_SAMPLES / 2 - 1] + v[PP_CAL_SAMPLES / 2]) / 2;
}

/* ── IIR pressure filter ─────────────────────────────────────────── */

/* [SNS-PRES-02, SNS-PRES-03] First-order IIR, τ = 500 ms, its state in Q8.
 * See IMPLEMENTATION.md "Pressure Filter".
 *
 * Whole pascals would not do: at 20 ms the step is 3.8 % of the difference,
 * which rounds to nothing under 26 Pa, and forcing a 1 Pa step instead made
 * the filter a rate limiter that passed the noise straight through. In Q8 the
 * dead band is a tenth of a pascal. 101 325 Pa x 256 fits in an int32; the
 * product is taken in int64. */
static int32_t filter_q8(int32_t raw_pressure, uint32_t dt_ms) {
    if (!pp.filter_initialized) {
        pp.filtered_q8 = raw_pressure * 256;
        pp.filter_initialized = true;
        return pp.filtered_q8;
    }
    int64_t diff_q8 = (int64_t)raw_pressure * 256 - pp.filtered_q8;
    int64_t alpha_q16 = ((int64_t)dt_ms << 16) / (PP_FILTER_TAU_MS + dt_ms);
    pp.filtered_q8 += (int32_t)((diff_q8 * alpha_q16) / 65536);
    return pp.filtered_q8;
}

static int32_t q8_round(int32_t q8) {
    return (q8 >= 0 ? q8 + 128 : q8 - 128) / 256;
}

int32_t pp_filter_pressure(int32_t raw_pressure, uint32_t dt_ms) {
    return q8_round(filter_q8(raw_pressure, dt_ms));
}

/* ── Altitude conversion ──────────────────────────────────────────── */

#define MAX_ALTITUDE_CM 800000

/* [SNS-ALT-01..03] Hypsometric formula — exact inverse of the ISA
 * troposphere pressure model.
 *
 *   h = 44330 × (1 − (P/P₀)^(1/5.2561))
 */
/* [SNS-ALT-04] Unclamped, because a clamp stops the altitude and a stopped
 * altitude is a speed of zero: below the pad, a glitch's decay read as
 * apogee; above 8 km, the climb itself did (N26). */
int32_t pp_pressure_to_height_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    if (pressure_pa <= 0 || ground_pressure_pa <= 0)
        return 0;
    float ratio = (float)pressure_pa / (float)ground_pressure_pa;
    float alt_m = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
    return (int32_t)(alt_m * 100.0f);
}

static int32_t height_from_q8(int32_t pressure_q8, int32_t ground_pressure_pa) {
    if (pressure_q8 <= 0 || ground_pressure_pa <= 0)
        return 0;
    float ratio = (float)pressure_q8 / (256.0f * (float)ground_pressure_pa);
    float alt_m = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
    return (int32_t)(alt_m * 100.0f);
}

int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    int32_t alt_cm = pp_pressure_to_height_cm(pressure_pa, ground_pressure_pa);
    if (alt_cm > MAX_ALTITUDE_CM)
        alt_cm = MAX_ALTITUDE_CM;
    if (alt_cm < 0)
        alt_cm = 0;
    return alt_cm;
}

/* ── Ring buffer helpers ──────────────────────────────────────────── */

static void ring_push(int32_t altitude_cm, int32_t height_cm, int32_t rise_cm, uint32_t timestamp_ms) {
    pp.ring[pp.head].altitude_cm = altitude_cm;
    pp.ring[pp.head].height_cm = height_cm;
    pp.ring[pp.head].rise_cm = rise_cm;
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
    /* A previous test's readings, and their times, are not this one's. */
    memset(&pp.med, 0, sizeof(pp.med));
    pp.last_timestamp = 0;
    /* And the ground reference, which otherwise starts empty and would climb
     * out of zero on the first sample. */
    gnd_reset(ground_pressure_pa);
}

void pp_resume_flight(int32_t ground_pa, int32_t start_pa) {
    pp.state = PP_RUNNING;
    pp.ground_pressure = ground_pa;
    pp.filtered_q8 = start_pa * 256;
    pp.last_filtered = start_pa;
    pp.filter_initialized = true;
    pp.last_timestamp = pp.med.last_ts;
    pp.gnd.tracking = false;
}

void pp_start_cal(void) {
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
    med_push(raw_pressure_pa, timestamp_ms);
    int32_t pa;
    uint32_t ts;
    bool fresh = med_out(&pa, &ts);
    if (fresh)
        hist_push(pa, ts);

    switch (pp.state) {
    case PP_IDLE:
        /* Discard — not calibrating yet */
        return;

    case PP_CALIBRATING:
        pp.cal[pp.cal_count++] = raw_pressure_pa;
        if (pp.cal_count >= PP_CAL_SAMPLES) {
            pp.ground_pressure = cal_median();
            pp.filtered_q8 = pp.ground_pressure * 256;
            pp.filter_initialized = true;
            pp.last_timestamp = pp.med.last_ts;
            gnd_reset(pp.ground_pressure);
            pp.state = PP_RUNNING;
        }
        return;

    case PP_RUNNING: {
        if (!fresh)
            return;

        uint32_t dt = ts - pp.last_timestamp;
        pp.last_timestamp = ts;

        /* IIR filter */
        int32_t q8 = filter_q8(pa, dt);
        int32_t filtered = q8_round(q8);
        pp.last_filtered = filtered;

        /* Before the altitude, so the reference is this sample's own. */
        gnd_feed(filtered, ts);

        /* The altitude from the fractional pressure: from the rounded one a
         * whole pascal is 8 cm, and the speed would step in 4 m/s. */
        int32_t height_cm = height_from_q8(q8, pp.ground_pressure);
        int32_t alt_cm = height_cm < 0 ? 0 : (height_cm > MAX_ALTITUDE_CM ? MAX_ALTITUDE_CM : height_cm);

        /* T+0 is read from the reading itself, not the filter: the filter
         * lags the first half metre by its time constant, and the median's
         * noise is a tenth of it. */
        int32_t rise_cm = pp_pressure_to_height_cm(pa, pp.ground_pressure);

        /* Push to ring */
        ring_push(alt_cm, height_cm, rise_cm, ts);
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
