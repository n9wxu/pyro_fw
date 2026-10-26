/*
 * See simple_apogee.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "simple_apogee.h"
#include "atmosphere.h"
#include <string.h>

#define RISE_M 2.0f
#define HOLD_MS 500u
#define HIGH_ENOUGH_M 30.0f
#define SPEED_STEP_MS 100u
#define LOCKOUT_SPEED_MS 50.0f
#define LOCKOUT_HOLD_MS 3000u

void simple_apogee_start(simple_apogee_t *f, float pad_pa) {
    memset(f, 0, sizeof(*f));
    f->pad_pa = pad_pa;
}

static int32_t median_of_three(const int32_t v[SIMPLE_MEDIAN_N]) {
    int32_t lo = v[0] < v[1] ? v[0] : v[1];
    int32_t hi = v[0] < v[1] ? v[1] : v[0];
    return v[2] < lo ? lo : v[2] > hi ? hi : v[2];
}

static void follow_the_top(simple_apogee_t *f, uint32_t t_ms) {
    if (atmos_height_above_m(f->median_pa, f->pad_pa) > HIGH_ENOUGH_M)
        f->high_enough = true;
    if (f->lowest_pa <= 0.0f || f->median_pa <= f->lowest_pa) {
        f->lowest_pa = f->median_pa;
        f->lowest_ms = t_ms;
        f->risen = false;
        return;
    }
    bool below_the_top = atmos_height_above_m(f->median_pa, f->lowest_pa) < -RISE_M;
    if (!f->high_enough || !below_the_top) {
        f->risen = false;
        return;
    }
    if (!f->risen)
        f->risen_since_ms = t_ms;
    f->risen = true;
}

static void follow_the_speed(simple_apogee_t *f, uint32_t t_ms) {
    if (f->heights > 0) {
        uint8_t newest = (uint8_t)((f->height_next + SIMPLE_SPEED_N - 1u) % SIMPLE_SPEED_N);
        if (t_ms - f->height_ms[newest] < SPEED_STEP_MS)
            return;
    }
    float h = simple_apogee_height_m(f);
    uint8_t oldest = f->heights < SIMPLE_SPEED_N ? 0u : f->height_next;
    if (f->heights == SIMPLE_SPEED_N && t_ms != f->height_ms[oldest]) {
        f->speed_ms = (h - f->height_m[oldest]) * 1000.0f / (float)(t_ms - f->height_ms[oldest]);
        f->have_speed = true;
    }
    f->height_m[f->height_next] = h;
    f->height_ms[f->height_next] = t_ms;
    f->height_next = (uint8_t)((f->height_next + 1u) % SIMPLE_SPEED_N);
    if (f->heights < SIMPLE_SPEED_N)
        f->heights++;

    if (!f->have_speed)
        return;
    bool slow = f->speed_ms < LOCKOUT_SPEED_MS && f->speed_ms > -LOCKOUT_SPEED_MS;
    if (slow && !f->slow)
        f->slow_since_ms = t_ms;
    f->slow = slow;
    f->released = slow && t_ms - f->slow_since_ms >= LOCKOUT_HOLD_MS;
}

void simple_apogee_reading(simple_apogee_t *f, int32_t raw_pa, uint32_t t_ms) {
    if (raw_pa <= 0)
        return;
    f->raw[f->raw_next] = raw_pa;
    f->raw_next = (uint8_t)((f->raw_next + 1u) % SIMPLE_MEDIAN_N);
    if (f->raw_count < SIMPLE_MEDIAN_N)
        f->raw_count++;
    if (f->raw_count < SIMPLE_MEDIAN_N)
        return;
    f->median_pa = (float)median_of_three(f->raw);
    follow_the_top(f, t_ms);
    follow_the_speed(f, t_ms);
}

bool simple_apogee_met(const simple_apogee_t *f, uint32_t now_ms) {
    return f->risen && now_ms - f->risen_since_ms >= HOLD_MS;
}

bool simple_apogee_released(const simple_apogee_t *f) {
    return f->released;
}

float simple_apogee_lowest_pa(const simple_apogee_t *f) {
    return f->lowest_pa;
}

uint32_t simple_apogee_lowest_ms(const simple_apogee_t *f) {
    return f->lowest_ms;
}

float simple_apogee_height_m(const simple_apogee_t *f) {
    return f->median_pa > 0.0f ? atmos_height_above_m(f->median_pa, f->pad_pa) : 0.0f;
}

float simple_apogee_speed_ms(const simple_apogee_t *f) {
    return f->speed_ms;
}
