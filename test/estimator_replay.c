/*
 * Replays a file of readings through one estimator and prints what it
 * reports: the C side of sim/study/lumped_port_check.py.
 *
 *     estimator_replay <name> <pad kelvin, or 0>  < readings
 *
 * Each line of input: time in microseconds, the reading in pascals or -1 for
 * none, and 1 when the board pulsed a channel at that time. Each reading is
 * given as the board gives it, as an altitude; each line of output is the
 * estimate in the study's terms: pressure, and the rate and curvature of
 * ln(pressure).
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/atmosphere.h"
#include "../src/estimator.h"
#include <stdio.h>
#include <stdlib.h>

#define G0 9.80665f
#define R_AIR 287.05287f

static void give(const estimator_vt *e, long reading_pa, unsigned long t_us, bool first, float pad_k) {
    float pa = (float)reading_pa;
    float altitude_m = atmos_altitude_m(pa);
    float m_per_pa = atmos_m_per_pa(altitude_m, pa);
    if (first)
        e->start(altitude_m, m_per_pa, (uint32_t)t_us, pad_k);
    else
        e->reading(altitude_m, m_per_pa, (uint32_t)t_us);
}

int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    const estimator_vt *e = estimator_at(estimator_index(argv[1]));
    float pad_k = (float)atof(argv[2]);
    unsigned long t_us;
    long reading_pa;
    int pulsed;
    bool started = false;
    while (scanf("%lu %ld %d", &t_us, &reading_pa, &pulsed) == 3) {
        if (!started) {
            if (reading_pa < 0)
                continue;
            give(e, reading_pa, t_us, true, pad_k);
            started = true;
        } else {
            if (pulsed)
                e->pulse((uint32_t)t_us);
            if (reading_pa < 0)
                e->no_reading((uint32_t)t_us);
            else
                give(e, reading_pa, t_us, false, pad_k);
        }
        estimate_t out;
        e->estimate(&out);
        float per_m = G0 / (R_AIR * atmos_temperature_at_k(out.altitude_m)); /* d(ln p) per metre of altitude */
        printf("%lu %.3f %.9g %.9g %.9g %.9g %d\n", t_us, (double)atmos_pressure_pa(out.altitude_m),
               (double)(-out.speed * per_m), (double)(-out.accel * per_m), (double)(out.speed_sigma * per_m),
               (double)(out.altitude_sigma * per_m), out.explains);
    }
    return 0;
}
