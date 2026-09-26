/*
 * MK1C bus captures from the plant model, written as boards/mk1c/pyro_board.c
 * writes them, for support/pyro_check.py to grade offline (pyro_check_tests).
 *
 *     synth_mk1c_capture <dir> [bleed_open]
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant.h"
#include "../boards/mk1c/pyro_sense.h"
#include <stdio.h>
#include <string.h>

#define STR_(x) #x
#define STR(x) STR_(x)

enum { BIAS_BUS = 23, BIAS_A = 16, BIAS_B = 25, ADC_VBAT = 0, ADC_BUS = 1, ADC_A = 2, ADC_B = 3 };

static unsigned levels[5];

static void start(int bleed_open) {
    plant_init(PLANT_MK1C);
    plant_set_pack_mv(8400);
    plant_match(1)->state = MATCH_ABSENT;
    plant_match(2)->state = MATCH_ABSENT;
    if (bleed_open)
        plant_set_fault(PF_BLEED_OPEN, true);
    plant_step(0.05);
}

/* The routine tests' readings, as the capture header records them. */
static void measure_levels(int bleed_open) {
    start(bleed_open);
    levels[0] = plant_adc_counts(ADC_BUS);
    plant_set_gpio(BIAS_BUS, 1); plant_step(0.008);
    levels[1] = plant_adc_counts(ADC_BUS);
    plant_set_gpio(BIAS_BUS, 0); plant_step(0.05);
    plant_set_gpio(BIAS_A, 1); plant_step(0.008);
    levels[2] = plant_adc_counts(ADC_A);
    plant_set_gpio(BIAS_A, 0); plant_step(0.05);
    plant_set_gpio(BIAS_B, 1); plant_step(0.008);
    levels[3] = plant_adc_counts(ADC_B);
    plant_set_gpio(BIAS_B, 0);
    levels[4] = plant_adc_counts(ADC_VBAT);
}

/* The bench capture's timing: 30 ms to settle, 200 us of baseline, the
 * BIAS_BUS edge, 2048 samples at 4 us (charge) or 12 us (decay). */
static int capture(const char *dir, int charge, int bleed_open) {
    unsigned dt = charge ? 4 : 12, pre = 200 / dt, n = 2048;
    start(bleed_open);
    plant_set_gpio(BIAS_BUS, charge ? 0 : 1);
    plant_step(0.030);
    char path[512];
    snprintf(path, sizeof path, "%s/wave_%s.csv", dir, charge ? "c" : "d");
    FILE *f = fopen(path, "w");
    if (!f)
        return 1;
    fprintf(f, "# capture,pyro bus %s\n# board,Pyro MK1C (plant model)\n# node,FIRING_BUS\n",
            charge ? "charge" : "decay");
    fprintf(f, "# dt_us,%u\n# n_samples,%u\n# pre_n,%u\n# edge_us,200\n# uv_per_count,2421\n", dt, n, pre);
    fprintf(f, "# design_r_bias_ohm,330\n# design_r_bleed_ohm,2200\n# design_r_div_ohm,14990\n"
               "# design_rpd_ohm,1918\n# design_c_bus_nf,1000\n# design_bus_biased_counts,1058\n"
               "# design_ch_biased_counts,1214\n");
    fprintf(f, "# bench_bus_biased_counts," STR(MK1C_BENCH_BUS_BIASED_COUNTS) "\n"
               "# bench_ch_biased_counts," STR(MK1C_BENCH_CH_BIASED_COUNTS) "\n"
               "# bench_c_bus_nf," STR(MK1C_BENCH_C_BUS_NF) "\n"
               "# u9_rev_is_a," STR(MK1C_U9_REV_IS_A) "\n"
               "# u9_rev_nvt_v," STR(MK1C_U9_REV_NVT_V) "\n"
               "# u9_rev_ohm," STR(MK1C_U9_REV_OHM) "\n"
               "# bias_gpio_v," STR(MK1C_BIAS_GPIO_V) "\n"
               "# bias_diode_is_a," STR(MK1C_BIAS_DIODE_IS_A) "\n"
               "# bias_diode_nvt_v," STR(MK1C_BIAS_DIODE_NVT_V) "\n");
    fprintf(f, "# meas_bus_quiescent_counts,%u\n# meas_bus_biased_counts,%u\n# meas_ch_a_biased_counts,%u\n"
               "# meas_ch_b_biased_counts,%u\n# meas_vbat_counts,%u\ni,counts\n",
            levels[0], levels[1], levels[2], levels[3], levels[4]);
    for (unsigned i = 0; i < n; i++) {
        if (i == pre)
            plant_set_gpio(BIAS_BUS, charge ? 1 : 0);
        fprintf(f, "%u,%u\n", i, plant_adc_counts(ADC_BUS));
        plant_step(dt * 1e-6);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    int bleed_open = argc > 2 && !strcmp(argv[2], "bleed_open");
    measure_levels(bleed_open);
    return capture(argv[1], 1, bleed_open) || capture(argv[1], 0, bleed_open);
}
