/*
 * Replays a file of readings through one estimator and prints what it
 * reports: the C side of sim/study/lumped_port_check.py.
 *
 *     estimator_replay <name> <pad kelvin, or 0>  < readings
 *
 * Each line of input: time in microseconds, the reading in pascals or -1 for
 * none, and 1 when the board pulsed a channel at that time.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/estimator.h"
#include <stdio.h>
#include <stdlib.h>

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
            e->start((int32_t)reading_pa, (uint32_t)t_us, pad_k);
            started = true;
        } else {
            if (pulsed)
                e->pulse((uint32_t)t_us);
            if (reading_pa < 0)
                e->no_reading((uint32_t)t_us);
            else
                e->reading((int32_t)reading_pa, (uint32_t)t_us);
        }
        estimate_t out;
        e->estimate(&out);
        printf("%lu %.3f %.9g %.9g %.9g %.9g %d\n", t_us, (double)out.pressure_pa, (double)out.rate, (double)out.curve,
               (double)out.rate_sigma, (double)out.log_sigma, out.explains);
    }
    return 0;
}
