/*
 * See atmosphere.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "atmosphere.h"
#include <math.h>
#include <stdbool.h>

#define G0 9.80665f
#define R_AIR 287.05287f /* R* / M, J/(kg K) */

typedef struct {
    float base_m, base_k, lapse_k_per_m, base_pa;
} layer_t;

static const layer_t LAYERS[] = {
    {0.0f, 288.15f, -0.0065f, 101325.0f},
    {11000.0f, 216.65f, 0.0f, 22632.06f},
    {20000.0f, 216.65f, 0.001f, 5474.889f},
    {32000.0f, 228.65f, 0.0028f, 868.0187f},
};
#define N_LAYERS (int)(sizeof(LAYERS) / sizeof(LAYERS[0]))

static const layer_t *layer_of_pressure(float pressure_pa) {
    int i = N_LAYERS - 1;
    while (i > 0 && pressure_pa > LAYERS[i].base_pa)
        i--;
    return &LAYERS[i];
}

static const layer_t *layer_of_altitude(float altitude_m) {
    int i = N_LAYERS - 1;
    while (i > 0 && altitude_m < LAYERS[i].base_m)
        i--;
    return &LAYERS[i];
}

static bool isothermal(const layer_t *l) {
    return l->lapse_k_per_m == 0.0f;
}

float atmos_altitude_m(float pressure_pa) {
    if (pressure_pa <= 0.0f)
        return 0.0f;
    const layer_t *l = layer_of_pressure(pressure_pa);
    float ratio = pressure_pa / l->base_pa;
    if (isothermal(l))
        return l->base_m - logf(ratio) * R_AIR * l->base_k / G0;
    float temperature_k = l->base_k * powf(ratio, -R_AIR * l->lapse_k_per_m / G0);
    return l->base_m + (temperature_k - l->base_k) / l->lapse_k_per_m;
}

float atmos_pressure_pa(float altitude_m) {
    const layer_t *l = layer_of_altitude(altitude_m);
    float above_base_m = altitude_m - l->base_m;
    if (isothermal(l))
        return l->base_pa * expf(-G0 * above_base_m / (R_AIR * l->base_k));
    float temperature_k = l->base_k + l->lapse_k_per_m * above_base_m;
    return l->base_pa * powf(l->base_k / temperature_k, G0 / (R_AIR * l->lapse_k_per_m));
}

float atmos_temperature_k(float pressure_pa) {
    if (pressure_pa <= 0.0f)
        return LAYERS[0].base_k;
    const layer_t *l = layer_of_pressure(pressure_pa);
    if (isothermal(l))
        return l->base_k;
    return l->base_k * powf(pressure_pa / l->base_pa, -R_AIR * l->lapse_k_per_m / G0);
}

float atmos_height_above_m(float pressure_pa, float pad_pa) {
    return atmos_altitude_m(pressure_pa) - atmos_altitude_m(pad_pa);
}

float atmos_pressure_above_pa(float pad_pa, float height_m) {
    return atmos_pressure_pa(atmos_altitude_m(pad_pa) + height_m);
}

float atmos_scale_height_m(float pressure_pa) {
    return R_AIR * atmos_temperature_k(pressure_pa) / G0;
}

float atmos_pad_air_ratio(float pressure_pa, float pad_pa) {
    if (pressure_pa <= 0.0f || pad_pa <= 0.0f)
        return 1.0f;
    float density_ratio = (pressure_pa / atmos_temperature_k(pressure_pa)) / (pad_pa / atmos_temperature_k(pad_pa));
    return sqrtf(density_ratio);
}
