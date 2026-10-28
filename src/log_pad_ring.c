/*
 * See log_pad_ring.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "log_pad_ring.h"

#define GAP_MS 1000u

void log_pad_clear(log_pad_ring_t *r) {
    r->next = r->count = 0;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void log_pad_push(log_pad_ring_t *r, uint32_t t_ms, int32_t pressure_pa, int32_t raw_pa, int16_t temp_dc) {
    if (r->count > 0 && t_ms - r->newest_ms > GAP_MS)
        log_pad_clear(r);
    if (r->count == 0)
        r->first_temp_dc = temp_dc;
    r->newest_ms = t_ms;
    int32_t d = clamp(raw_pa - pressure_pa, INT16_MIN, INT16_MAX);
    r->e[r->next] = (log_pad_entry_t){pressure_pa, (int16_t)d, (uint16_t)t_ms};
    r->temp_less_first_dc[r->next] = (int8_t)clamp(temp_dc - r->first_temp_dc, INT8_MIN, INT8_MAX);
    r->next = (uint16_t)((r->next + 1u) % LOG_PAD_N);
    if (r->count < LOG_PAD_N)
        r->count++;
}

uint16_t log_pad_count(const log_pad_ring_t *r) {
    return r->count;
}

bool log_pad_row(const log_pad_ring_t *r, uint16_t i, uint32_t t0_ms, const air_t *air, uint8_t state,
                 flog_sample_t *out) {
    if (i >= r->count)
        return false;
    uint16_t at = (uint16_t)((r->next + LOG_PAD_N - r->count + i) % LOG_PAD_N);
    const log_pad_entry_t *e = &r->e[at];
    *out = (flog_sample_t){
        .time_ms = (int16_t)(uint16_t)(e->t_ms - (uint16_t)t0_ms),
        .pressure_pa = e->pressure_pa,
        .altitude_cm = (int32_t)(air_height_m(air, atmos_altitude_m((float)e->pressure_pa)) * 100.0f),
        .raw_pa = e->pressure_pa + e->raw_less_pressure_pa,
        .temp_dc = (int16_t)(r->first_temp_dc + r->temp_less_first_dc[at]),
        .state = state,
    };
    return true;
}
