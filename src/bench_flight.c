/*
 * A flight on the bench. See bench_flight.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "bench_flight.h"
#include "atmosphere.h"
#include <string.h>

static struct {
    fsim_t sim;
    bool flying, mocked;
    uint64_t t0_us;
    float t_s, alt_m, pa, ground_pa, peak_m;
    uint32_t flights;
    uint32_t fires[2];
    bool fired[2];
    bool pulsing;
    uint32_t pulse_ms;
} bf;

bf_start_t bench_flight_start(const fsim_params_t *p, float ground_pa, bool on_pad, bool test_mode, uint64_t now_us) {
    if (bf.flying)
        return BF_RUNNING;
    if (!on_pad)
        return BF_NOT_ON_PAD;
    if (!test_mode)
        return BF_NOT_TESTING;
    if (!fsim_start(&bf.sim, p, ground_pa))
        return BF_BAD_PROFILE;
    bf.flying = bf.mocked = true;
    bf.t0_us = now_us;
    bf.t_s = bf.alt_m = bf.peak_m = 0.0f;
    bf.pa = bf.ground_pa = ground_pa;
    bf.fired[0] = bf.fired[1] = false;
    bf.flights++;
    return BF_STARTED;
}

void bench_flight_stop(void) {
    bf.flying = false;
}

bool bench_flight_pressure(uint64_t stamp_us, bool landed, float *pa) {
    if (!bf.flying)
        return false;
    /* A reading stamped before the start is the pad's. */
    bf.t_s = stamp_us > bf.t0_us ? (float)(stamp_us - bf.t0_us) / 1e6f : 0.0f;
    bf.alt_m = fsim_altitude(&bf.sim, bf.t_s);
    if (bf.alt_m > bf.peak_m)
        bf.peak_m = bf.alt_m;
    bf.pa = atmos_pressure_pa(bf.sim.pad_msl + bf.alt_m);
    *pa = bf.pa;
    if (landed && fsim_phase(&bf.sim) == FSIM_LANDED)
        bf.flying = false;
    return true;
}

bool bench_flight_mocked(void) {
    return bf.mocked;
}

void bench_flight_fire(uint8_t channel, uint32_t now_ms) {
    if (channel < 1 || channel > 2)
        return;
    bf.fires[channel - 1]++;
    bf.fired[channel - 1] = true;
    bf.pulsing = true;
    bf.pulse_ms = now_ms;
}

bool bench_flight_firing(uint32_t now_ms) {
    if (bf.pulsing && now_ms - bf.pulse_ms >= BENCH_PULSE_MS)
        bf.pulsing = false;
    return bf.pulsing;
}

bool bench_flight_fired(uint8_t channel) {
    return channel >= 1 && channel <= 2 && bf.fired[channel - 1];
}

void bench_flight_status(bench_flight_status_t *out) {
    out->flying = bf.flying;
    out->mocked = bf.mocked;
    out->p = bf.sim.p;
    out->phase = fsim_phase(&bf.sim);
    out->t_s = bf.t_s;
    out->alt_m = bf.alt_m;
    out->pa = bf.pa;
    out->ground_pa = bf.ground_pa;
    out->peak_m = bf.peak_m;
    out->t_apogee_s = bf.sim.p.pad_s + bf.sim.t_apogee;
    out->flights = bf.flights;
    out->fires[0] = bf.fires[0];
    out->fires[1] = bf.fires[1];
}

void bench_flight_init(void) {
    memset(&bf, 0, sizeof(bf));
}
