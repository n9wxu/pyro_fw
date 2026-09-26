/*
 * MS5607-02BA03 pressure sensor driver interface.
 *
 * Synchronous (blocking), at init only: ms5607_detect() probes both I2C
 * addresses and reads the PROM; ms5607_read() makes one full conversion.
 *
 * One-shot, in flight [DD-051]: each loop takes the conversion its alarm
 * finished and commands the next. The alarm's handler reads the ADC from RAM,
 * so nothing a flash erase does can reach it. The compensation, and the
 * temperature it needs, stay in the loop.
 *
 * Figures are from docs/datasheets/MS5607-02BA03_2017-06.pdf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MS5607_DRIVER_H
#define MS5607_DRIVER_H

#include "pressure_sensor.h"
#include <stdbool.h>
#include <stdint.h>

/* One conversion at OSR 4096 and its read: the main loop's period [DD-051]. */
#define MS5607_CONV_MS 10

/* OSR 4096's worst case is 9.04 ms (datasheet page 3); a read issued earlier
 * answers 0. Timed from the end of the command. */
#define MS5607_CONV_DONE_US 9100u

/* [SNS-PRES-08] A reading describes the middle of its D1 conversion (OSR 4096
 * takes up to 9.04 ms), not the moment it is read. From the hardware timer,
 * which keeps counting while a flash erase stalls the loop. */
#define MS5607_HALF_CONV_US 4500u
static inline uint64_t ms5607_sample_time_us(uint64_t d1_command_us) {
    return d1_command_us + MS5607_HALF_CONV_US;
}

/* The datasheet's first-order compensation (page 8), from the PROM's C1-C6. */
static inline void ms5607_compensate_prom(const uint16_t prom[8], uint32_t d1, uint32_t d2,
                                          pressure_reading_t *out) {
    int32_t dT = (int32_t)d2 - ((int32_t)prom[5] << 8);
    int32_t temp = 2000 + (int32_t)(((int64_t)dT * prom[6]) >> 23);
    int64_t off = ((int64_t)prom[2] << 17) + (((int64_t)prom[4] * dT) >> 6);
    int64_t sens = ((int64_t)prom[1] << 16) + (((int64_t)prom[3] * dT) >> 7);
    int32_t p = (int32_t)((((int64_t)d1 * sens >> 21) - off) >> 15);
    out->temperature_c = temp / 100.0f;
    out->pressure_pa = (float)p;
}

/* ── Temperature, once in MS5607_D2_EVERY conversions [DD-051] ───────
 *
 * The other nine measure pressure, each compensated with the temperature
 * carried to its own time along the line through the last MS5607_TEMPS
 * readings. Never reused as read: with the datasheet's example coefficients a
 * temperature 1 °C stale moves the pressure about 240 Pa -- the bridge's own
 * temperature coefficient, which the compensation exists to remove. A line
 * through several readings rather than two, because a two-point slope carries
 * both readings' noise into every pressure. */
#define MS5607_D2_EVERY 10u
#define MS5607_TEMPS 4u
/* Carried no further past the newest reading: after a gap, the line's end. */
#define MS5607_TEMPS_AHEAD_US 200000u

typedef struct {
    uint32_t d2[MS5607_TEMPS];
    uint64_t us[MS5607_TEMPS]; /* the middle of each conversion */
    uint8_t head, n;
    uint8_t since; /* pressure conversions since the last temperature */
} ms5607_temps_t;

/* Whether the next conversion is the temperature: until one has been read,
 * and then once in MS5607_D2_EVERY. */
static inline bool ms5607_temperature_due(const ms5607_temps_t *t) {
    return t->n == 0 || t->since + 1u >= MS5607_D2_EVERY;
}

static inline void ms5607_conversion_started(ms5607_temps_t *t, bool temperature) {
    t->since = temperature ? 0u : (uint8_t)(t->since + 1u);
}

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
    int64_t ahead = (int64_t)(at_us - t->us[newest]);
    if (ahead < 0)
        ahead = 0;
    if (ahead > (int64_t)MS5607_TEMPS_AHEAD_US)
        ahead = MS5607_TEMPS_AHEAD_US;
    float d = my + sxy / sxx * ((float)ahead * 1e-6f - mx);
    return (uint32_t)((int64_t)t->d2[newest] + (int64_t)(d >= 0.0f ? d + 0.5f : d - 0.5f));
}

/* ── Synchronous API (blocking, init time only) ─────────────────── */

bool ms5607_detect(void);
bool ms5607_read(pressure_reading_t *reading);

/* Compute compensated pressure and temperature from raw D1/D2, with this
 * part's PROM. Always returns true (pure arithmetic, no I2C). */
bool ms5607_compensate(uint32_t d1, uint32_t d2, pressure_reading_t *out);

/* ── One-shot conversion [DD-051] ─────────────────────────────────── */

typedef enum { MS5607_STARTED, MS5607_BUSY, MS5607_BUS_ERROR } ms5607_start_t;

typedef struct {
    uint32_t raw;        /* D1 or D2 */
    uint64_t command_us; /* when the conversion began */
    bool temperature;
    bool ok; /* the read completed; false: a NACK or a stuck bus */
} ms5607_conversion_t;

/* Claims a hardware alarm for the one-shot and installs its handler. */
bool ms5607_async_begin(void);

/* Command a conversion and arm its one-shot. BUSY: the last one is still in
 * flight, and nothing was sent. */
ms5607_start_t ms5607_async_start(bool temperature);

/* The conversion the one-shot finished, once. */
bool ms5607_async_take(ms5607_conversion_t *out);

#endif /* MS5607_DRIVER_H */
