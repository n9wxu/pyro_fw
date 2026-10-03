/*
 * MS5607-02BA03 pressure sensor driver interface.
 *
 * Detection, at boot: ms5607_detect_step(), once a loop, resets each I2C
 * address in turn and reads its PROM once the reset has reloaded it.
 *
 * In flight the pressure collector owns the part (pressure_collector.h):
 * MS5607_PART is what it needs to know of it. Each cycle is a pressure code
 * and the temperature code converted after it. The arithmetic on them is
 * here, and runs in the sensor task.
 *
 * Figures are from docs/datasheets/MS5607-02BA03_2017-06.pdf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MS5607_DRIVER_H
#define MS5607_DRIVER_H

#include "pressure_collector.h"
#include "pressure_sensor.h"
#include <stdbool.h>
#include <stdint.h>

/* The fastest SCLK its I2C allows (page 5). A board runs its bus at this or,
 * where its PCB cannot, slower: BOARD_MS5607_I2C_HZ [DD-052]. */
#define MS5607_I2C_MAX_HZ 400000u

/* OSR 4096's worst case is 9.04 ms (page 3). A read issued earlier answers 0
 * and spoils the conversion (page 11). */
#define MS5607_CONV_DONE_US 9100u

/* [SNS-PRES-08] A reading describes the middle of its conversion. */
#define MS5607_HALF_CONV_US 4500u

/* The PROM reloads for 2.8 ms after a reset (pages 10-11). */
#define MS5607_RESET_MS 3u

#define MS5607_CMD_RESET 0x1Eu
#define MS5607_CMD_CONVERT_D1 0x48u /* pressure, OSR 4096 */
#define MS5607_CMD_CONVERT_D2 0x58u /* temperature, OSR 4096 */
#define MS5607_CMD_ADC_READ 0x00u

enum { MS5607_PRESSURE, MS5607_TEMPERATURE };

#define MS5607_CONVERSION(cmd)                                                                                         \
    { {(cmd)}, 1, MS5607_CMD_ADC_READ, 3, MS5607_CONV_DONE_US, MS5607_HALF_CONV_US }

static const collector_part_t MS5607_PART = {
    .steps = 2,
    .step = {[MS5607_PRESSURE] = MS5607_CONVERSION(MS5607_CMD_CONVERT_D1),
             [MS5607_TEMPERATURE] = MS5607_CONVERSION(MS5607_CMD_CONVERT_D2)},
    .reset = {MS5607_CMD_RESET},
    .reset_len = 1,
    .reset_us = MS5607_RESET_MS * 1000u,
};

/* A 24-bit ADC code, most significant byte first. */
static inline uint32_t ms5607_code(const uint8_t bytes[3]) {
    return ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
}

/* The PROM's 4-bit CRC, in the low bits of its last word (page 13; the
 * algorithm is TE's application note AN520). */
static inline bool ms5607_prom_crc_ok(const uint16_t prom[8]) {
    uint16_t remainder = 0;
    for (int i = 0; i < 16; i++) {
        uint16_t word = i >> 1 == 7 ? (uint16_t)(prom[7] & 0xFF00u) : prom[i >> 1];
        remainder ^= (i & 1) ? (uint16_t)(word & 0x00FFu) : (uint16_t)(word >> 8);
        for (int bit = 0; bit < 8; bit++)
            remainder = (remainder & 0x8000u) ? (uint16_t)((remainder << 1) ^ 0x3000u) : (uint16_t)(remainder << 1);
    }
    return (remainder >> 12) == (prom[7] & 0x000Fu);
}

/* The datasheet's compensation from the PROM's C1-C6: first order (page 8),
 * and below 20 C the second order it recommends (page 9). */
static inline void ms5607_compensate_prom(const uint16_t prom[8], uint32_t d1, uint32_t d2, pressure_reading_t *out) {
    int32_t dT = (int32_t)d2 - ((int32_t)prom[5] << 8);
    int32_t temp = 2000 + (int32_t)(((int64_t)dT * prom[6]) >> 23);
    int64_t off = ((int64_t)prom[2] << 17) + (((int64_t)prom[4] * dT) >> 6);
    int64_t sens = ((int64_t)prom[1] << 16) + (((int64_t)prom[3] * dT) >> 7);
    if (temp < 2000) {
        int64_t below_20 = temp - 2000;
        int64_t off2 = (61 * below_20 * below_20) >> 4;
        int64_t sens2 = 2 * below_20 * below_20;
        if (temp < -1500) {
            int64_t below_minus_15 = temp + 1500;
            off2 += 15 * below_minus_15 * below_minus_15;
            sens2 += 8 * below_minus_15 * below_minus_15;
        }
        temp -= (int32_t)(((int64_t)dT * dT) >> 31);
        off -= off2;
        sens -= sens2;
    }
    int32_t p = (int32_t)((((int64_t)d1 * sens >> 21) - off) >> 15);
    out->temperature_c = temp / 100.0f;
    out->pressure_pa = (float)p;
}

/* ── Temperature, every loop [DD-066] ─────────────────────────────────
 *
 * Each pressure is compensated with the temperature at its own time, on the
 * line through the last MS5607_TEMPS readings: between the last loop's and
 * its own, taken 9 ms after it. Never reused as read: with the datasheet's
 * example coefficients a temperature 1 °C stale moves the pressure about
 * 240 Pa -- the bridge's own temperature coefficient, which the compensation
 * exists to remove. A line through several readings rather than two, because
 * a two-point slope carries both readings' noise into every pressure. */
#define MS5607_TEMPS 4u
/* Carried no further past the newest reading: after a gap, the line's end. */
#define MS5607_TEMPS_AHEAD_US 200000u

typedef struct {
    uint32_t d2[MS5607_TEMPS];
    uint64_t us[MS5607_TEMPS]; /* the middle of each conversion */
    uint8_t head, n;
} ms5607_temps_t;

static inline void ms5607_temps_note(ms5607_temps_t *t, uint32_t d2, uint64_t at_us) {
    t->d2[t->head] = d2;
    t->us[t->head] = at_us;
    t->head = (uint8_t)((t->head + 1u) % MS5607_TEMPS);
    if (t->n < MS5607_TEMPS)
        t->n++;
}

/* D2 at at_us. Times and counts are taken from the newest reading, so the
 * least-squares sums stay small enough for float. */
static inline uint32_t ms5607_temps_at(const ms5607_temps_t *t, uint64_t at_us) {
    unsigned newest = (t->head + MS5607_TEMPS - 1u) % MS5607_TEMPS;
    if (t->n < 2)
        return t->d2[newest];
    float x[MS5607_TEMPS], y[MS5607_TEMPS], mx = 0.0f, my = 0.0f;
    for (unsigned i = 0; i < t->n; i++) {
        unsigned k = (newest + MS5607_TEMPS - i) % MS5607_TEMPS;
        x[i] = (float)(int64_t)(t->us[k] - t->us[newest]) * 1e-6f;
        y[i] = (float)((int32_t)t->d2[k] - (int32_t)t->d2[newest]);
        mx += x[i];
        my += y[i];
    }
    mx /= (float)t->n;
    my /= (float)t->n;
    float sxx = 0.0f, sxy = 0.0f;
    for (unsigned i = 0; i < t->n; i++) {
        sxx += (x[i] - mx) * (x[i] - mx);
        sxy += (x[i] - mx) * (y[i] - my);
    }
    if (sxx <= 0.0f)
        return t->d2[newest];
    /* Anywhere along the readings, and a little past the newest; never
     * before the oldest. */
    unsigned oldest = (newest + MS5607_TEMPS - (t->n - 1u)) % MS5607_TEMPS;
    int64_t ahead = (int64_t)(at_us - t->us[newest]);
    int64_t back = (int64_t)(t->us[oldest] - t->us[newest]);
    if (ahead < back)
        ahead = back;
    if (ahead > (int64_t)MS5607_TEMPS_AHEAD_US)
        ahead = MS5607_TEMPS_AHEAD_US;
    float d = my + sxy / sxx * ((float)ahead * 1e-6f - mx);
    return (uint32_t)((int64_t)t->d2[newest] + (int64_t)(d >= 0.0f ? d + 0.5f : d - 0.5f));
}

/* ── Detection [DD-053] ─────────────────────────────────────────── */

typedef enum { MS5607_DETECT_PENDING, MS5607_DETECT_FOUND, MS5607_DETECT_ABSENT } ms5607_detect_result_t;

typedef struct {
    uint8_t addr;
    bool reloading;
    uint32_t due_ms;
} ms5607_detect_t;

void ms5607_detect_begin(ms5607_detect_t *d);
ms5607_detect_result_t ms5607_detect_step(ms5607_detect_t *d, uint32_t now_ms);

/* Compute compensated pressure and temperature from raw D1/D2, with this
 * part's PROM. Always returns true (pure arithmetic, no I2C). */
bool ms5607_compensate(uint32_t d1, uint32_t d2, pressure_reading_t *out);

/* The address ms5607_detect() found the sensor at. */
uint8_t ms5607_address(void);

#endif /* MS5607_DRIVER_H */
