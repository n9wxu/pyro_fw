/*
 * See flight_run.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_run.h"
#include "board_harness.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAD_S 10.0f

const mp_site_t COLD = {10.0f, 0.0f};
const mp_site_t HOT = {45.0f, 2000.0f};
const mp_site_t ISA = {15.0f, 0.0f};

const rocket_t ROCKETS[N_ROCKETS] = {
    [HOP] = {"hop", {0.3f, 0.02f, 18.0f, 0.8f, 0.0012f, 2.0f, 6.0f}},              /* about 60 m */
    [SUBSONIC] = {"subsonic", {1.0f, 0.10f, 141.0f, 1.5f, 0.0020f, 2.0f, 20.0f}},  /* Mach 0.50 */
    [MID_MACH] = {"mid-Mach", {1.0f, 0.15f, 220.0f, 1.6f, 0.0020f, 2.0f, 20.0f}},  /* Mach 0.76 */
    [DRAGGY] = {"draggy", {2.0f, 0.60f, 1709.0f, 1.5f, 0.0045f, 3.0f, 20.0f}},     /* Mach 1.50, 66 g */
    [LOW_DRAG] = {"low-drag", {8.0f, 4.00f, 1216.0f, 5.0f, 0.0008f, 1.5f, 20.0f}}, /* Mach 1.60, 9.5 km */
    [BOOST_30G] = {"30 g", {1.0f, 0.30f, 395.0f, 1.1f, 0.0015f, 2.0f, 20.0f}},     /* Mach 0.93 at 0.96 s */
    [TO_20_KM] = {"20 km", {8.0f, 3.00f, 1700.0f, 5.0f, 0.0008f, 1.5f, 20.0f}},    /* Mach 2.4 */
    [TO_30_KM] = {"30 km", {8.0f, 6.00f, 2000.0f, 6.0f, 0.0008f, 1.5f, 20.0f}},    /* Mach 3.0 */
    [TO_45_KM] = {"45 km", {8.0f, 8.00f, 2100.0f, 7.5f, 0.0008f, 1.5f, 20.0f}},    /* past any sensor's range */
};

static void plant_to_apogee(const mp_rocket_t *rocket, const mp_site_t *site, mp_state_t *st) {
    mp_launch(st);
    while (!st->apogee && st->t < 600.0f)
        mp_step(st, site, rocket, 0.001f);
}

float plant_apogee_m(const mp_rocket_t *rocket, const mp_site_t *site) {
    mp_state_t st;
    plant_to_apogee(rocket, site, &st);
    return st.apogee_h;
}

float plant_apogee_s(const mp_rocket_t *rocket, const mp_site_t *site) {
    mp_state_t st;
    plant_to_apogee(rocket, site, &st);
    return st.apogee_t;
}

static void store_config(const flight_conditions_t *c) {
    char ini[512];
    int n = snprintf(ini, sizeof(ini), "[pyro]\nunits=m\n");
    if (c->to_landed)
        n += snprintf(ini + n, sizeof(ini) - (size_t)n, "landing_timeout=0\n");
    if (c->main_m)
        n += snprintf(ini + n, sizeof(ini) - (size_t)n, "pyro2_mode=agl\npyro2_value=%u\n", (unsigned)c->main_m);
    if (c->estimator)
        n += snprintf(ini + n, sizeof(ini) - (size_t)n, "estimator=%s\n", c->estimator);
    if (c->config)
        snprintf(ini + n, sizeof(ini) - (size_t)n, "%s", c->config);
    harness_config(ini);
}

static void set_board(const flight_conditions_t *c) {
    if (c->pyro_faulted[0]) {
        mock_pyro.p1_good = false;
        mock_pyro.p1_open = true;
    }
    if (c->pyro_faulted[1]) {
        mock_pyro.p2_good = false;
        mock_pyro.p2_open = true;
    }
    mock_pyro.energises_nothing = c->energises_nothing;
    mock_pyro.verdict_after_ms = c->verdict_after_ms;
    mock_stall_model = c->stalls;
    if (c->limits)
        mock_pyro_limits = *c->limits;
}

/* What a channel's charge puts out, once the pulse that lights it comes. */
typedef struct {
    float rate_ms[2];
    int lights_on[2];
    int pulses[2];
    bool lit[2];
} charges_t;

static charges_t charges_of(const flight_conditions_t *c, const mp_rocket_t *rocket) {
    charges_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.rate_ms[0] = c->rate_ms[0] > 0.0f ? c->rate_ms[0] : rocket->canopy_ms;
    ch.rate_ms[1] = c->rate_ms[1] > 0.0f ? c->rate_ms[1] : c->main_ms;
    for (int i = 0; i < 2; i++)
        ch.lights_on[i] = c->lights_on_pulse[i] == 0 ? 1 : c->lights_on_pulse[i];
    return ch;
}

/* True when this pulse lit the channel's charge. */
static bool pulse_lights(charges_t *ch, const mock_pulse_t *pulse) {
    int i = pulse->channel - 1;
    ch->pulses[i]++;
    if (ch->lit[i] || !pulse->energised || ch->pulses[i] != ch->lights_on[i])
        return false;
    ch->lit[i] = true;
    return true;
}

static void set_sensor(const flight_conditions_t *c, const mp_rocket_t *rocket, const mp_state_t *st, float tf,
                       bool *glitched) {
    bool out = c->dropout_s > 0.0f && tf >= c->dropout_at_s && tf < c->dropout_at_s + c->dropout_s;
    mock_pressure.sensor_type = out ? 0 : 2;
    mock_sensor_stuck =
        c->stuck_at_s > 0.0f && tf >= c->stuck_at_s && (c->stuck_s <= 0.0f || tf < c->stuck_at_s + c->stuck_s);
    if (c->glitch_n > 0 && tf >= c->glitch_at_s && !*glitched) {
        mock_glitch_pa = c->glitch_pa;
        mock_glitch_samples = c->glitch_n;
        *glitched = true;
    }
    float quiet = c->sensor_rms_pa > 0.0f ? c->sensor_rms_pa : SENSOR_RMS_PA;
    mock_noise_rms_pa = quiet;
    if (c->swing_rms_pa > 0.0f && st->canopy && !st->landed)
        mock_noise_rms_pa = c->swing_rms_pa;
    if (c->coast_noise_pa > 0.0f && tf > rocket->burn_s && !st->apogee &&
        (c->coast_noise_to_s <= 0.0f || tf < c->coast_noise_to_s))
        mock_noise_rms_pa = c->coast_noise_pa;
}

static float pad_air_descent_ms(const mp_site_t *site, const mp_state_t *st) {
    if (st->v >= 0.0f)
        return 0.0f;
    float rho = mp_pressure_pa(site, st->h) / mp_temp_k(site, st->h);
    float rho_pad = mp_pad_pa(site) / (site->temp_c + 273.15f);
    return -st->v * sqrtf(rho / rho_pad);
}

flown_t fly(const mp_rocket_t *rocket, const mp_site_t *site, const flight_conditions_t *c, uint32_t seed,
            float until_s) {
    flown_t res;
    memset(&res, 0, sizeof(res));
    boot_like_hardware(seed);
    if (c->interval_ms)
        mock_sample_interval_ms = c->interval_ms;
    if (c->sensor_rms_pa > 0.0f)
        mock_noise_rms_pa = c->sensor_rms_pa;
    store_config(c);
    set_board(c);
    mock_pressure.pressure_pa = mp_pad_pa(site);
    mock_pressure.temperature_c = site->temp_c;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    uint32_t ign = pad + (uint32_t)((c->pad_s > 0.0f ? c->pad_s : PAD_S) * 1000.0f);

    mp_rocket_t rk = *rocket;
    charges_t charges = charges_of(c, rocket);
    mp_state_t st;
    mp_launch(&st);
    st.thin_air = c->thin_air;
    int seen = 0;
    float charge_at = -1.0f;
    bool glitched = false, lost = false, restarted = false;
    loop_lag_ms = c->loop_lag;
    for (; t < ign + (uint32_t)(until_s * 1000.0f); t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        if (tf >= 0.0f)
            mp_step(&st, site, &rk, 0.001f);
        float static_pa = mp_pressure_pa(site, st.h);
        float sensed = static_pa + mp_port_error_pa(&c->port, st.mach, static_pa);
        if (charge_at >= 0.0f)
            sensed += mp_charge_pa(&c->charge, tf - charge_at);
        mock_pressure.pressure_pa = sensed;
        set_sensor(c, rocket, &st, tf, &glitched);
        if (c->canopy_lost_below_m > 0.0f && st.apogee && st.canopy && !lost && st.h < c->canopy_lost_below_m) {
            st.canopy = false;
            lost = true;
        }
        if (c->restart_at_s > 0.0f && !restarted && tf >= c->restart_at_s) {
            restarted = true;
            harness_restart((reset_cause_t)c->restart_cause);
            seen = 0;
        }
        tick(t);

        if (ctx.pyros_armed && !res.armed) {
            res.armed = true;
            res.armed_t = tf;
            res.armed_h = st.h;
        }
        if (res.launched && !ctx.under_thrust && res.thrust_end_t == 0.0f)
            res.thrust_end_t = tf;
        if (ctx.current_state == ASCENT && !res.launched) {
            res.launched = true;
            res.launch_t = tf;
            res.launch_h = st.h;
        }
        if (ctx.apogee_declared && !res.apogee_declared) {
            res.apogee_declared = true;
            res.declared_t = tf;
        }
        for (; seen < mock_pulse_count; seen++) {
            const mock_pulse_t *pulse = &mock_pulses[seen];
            if (res.pulses < FLOWN_PULSES_MAX)
                res.pulse[res.pulses++] = (flown_pulse_t){tf, st.h, st.v, pulse->channel, pulse->energised};
            if (pulse->channel == 1 && !res.drogue) {
                res.drogue = true;
                res.drogue_t = tf;
            } else if (pulse->channel == 2 && !res.main) {
                res.main = true;
                res.main_t = tf;
                res.main_h = st.h;
            }
            if (!pulse_lights(&charges, pulse))
                continue;
            if (pulse->channel == 1) { /* a lit charge leaves its channel open */
                mock_pyro.p1_good = false;
                mock_pyro.p1_open = true;
            } else {
                mock_pyro.p2_good = false;
                mock_pyro.p2_open = true;
            }
            if (c->charge.peak_pa > 0.0f)
                charge_at = tf;
            float rate = charges.rate_ms[pulse->channel - 1];
            if (rate > 0.0f && !lost && (!st.canopy || rate < rk.canopy_ms)) {
                st.canopy = true;
                rk.canopy_ms = rate;
            }
        }
        for (uint8_t i = 0; i < estimator_count(); i++) {
            if (ctx.estimator_said_apogee[i] && !res.said_apogee[i]) {
                res.said_apogee[i] = true;
                res.said_t[i] = tf;
            }
        }
        float descent = pad_air_descent_ms(site, &st);
        if (descent > res.fastest_descent_ms)
            res.fastest_descent_ms = descent;
        if (c->to_landed && ctx.current_state == LANDED) {
            res.landed_t = tf;
            break;
        }
        if (c->stop_after_apogee && st.apogee && tf > st.apogee_t + 2.0f)
            break;
        if (st.landed && res.touchdown_t == 0.0f)
            res.touchdown_t = tf;
        if (st.landed && (!c->to_landed || tf - res.touchdown_t > 10.0f))
            break;
    }
    res.apogee_t = st.apogee_t;
    res.apogee_h = st.apogee_h;
    res.max_mach = st.max_mach;
    res.peak_cm = ctx.max_altitude_cm;
    res.peak_lower_bound = ctx.peak_lower_bound;
    res.final_state = (int)ctx.current_state;
    res.final_altitude_cm = ctx.altitude_cm;
    return res;
}
