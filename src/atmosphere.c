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

float atmos_temperature_at_k(float altitude_m) {
    const layer_t *l = layer_of_altitude(altitude_m);
    return l->base_k + l->lapse_k_per_m * (altitude_m - l->base_m);
}

float atmos_m_per_pa(float altitude_m, float pressure_pa) {
    return R_AIR * atmos_temperature_at_k(altitude_m) / (G0 * pressure_pa);
}

air_t air_over_pad(float pad_pa, float pad_k) {
    air_t a = {.pad_pa = pad_pa, .pad_altitude_m = atmos_altitude_m(pad_pa)};
    float standard_k = atmos_temperature_at_k(a.pad_altitude_m);
    a.offset_k = pad_k > 0.0f && pad_pa > 0.0f ? pad_k - standard_k : 0.0f;
    a.pad_k = standard_k + a.offset_k;
    return a;
}

/* dz = (R T / g) d(ln p), T the standard's plus the offset: the standard's
 * altitude, and the offset's share. */
float air_height_m(const air_t *a, float altitude_m) {
    if (a->offset_k == 0.0f)
        return altitude_m - a->pad_altitude_m;
    float offset_m = R_AIR * a->offset_k / G0 * logf(a->pad_pa / atmos_pressure_pa(altitude_m));
    return altitude_m - a->pad_altitude_m + offset_m;
}

float air_scale(const air_t *a, float altitude_m) {
    return 1.0f + a->offset_k / atmos_temperature_at_k(altitude_m);
}

float air_pad_ratio(const air_t *a, float altitude_m) {
    if (a->pad_pa <= 0.0f)
        return 1.0f;
    float density = atmos_pressure_pa(altitude_m) / (atmos_temperature_at_k(altitude_m) + a->offset_k);
    return sqrtf(density / (a->pad_pa / a->pad_k));
}

#define INVERSE_STEPS 4

float air_altitude_m(const air_t *a, float height_m) {
    float altitude_m = a->pad_altitude_m + height_m;
    if (a->offset_k == 0.0f)
        return altitude_m;
    for (int i = 0; i < INVERSE_STEPS; i++)
        altitude_m -= (air_height_m(a, altitude_m) - height_m) / air_scale(a, altitude_m);
    return altitude_m;
}
