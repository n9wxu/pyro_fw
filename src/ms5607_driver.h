/*
 * MS5607-02BA03 pressure sensor driver interface.
 *
 * Detection, at boot: ms5607_detect_step(), once a loop, resets each I2C
 * address in turn and reads its PROM once the reset has reloaded it.
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

/* The fastest SCLK its I2C allows (datasheet page 5). A board runs its bus at
 * this or, where its PCB cannot, slower: BOARD_MS5607_I2C_HZ [DD-052]. */
#define MS5607_I2C_MAX_HZ 400000u

/* OSR 4096's worst case is 9.04 ms (datasheet page 3); a read issued earlier
 * answers 0. Timed from the end of the command. */
#define MS5607_CONV_DONE_US 9100u

/* [SNS-PRES-08] A reading describes the middle of its conversion (OSR 4096
 * takes up to 9.04 ms), not the moment it is read. */
#define MS5607_HALF_CONV_US 4500u

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

/* ── Detection [DD-053] ─────────────────────────────────────────── */

/* The PROM reloads for 2.8 ms after a reset (datasheet pages 10-11). */
#define MS5607_RESET_MS 3u

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

/* ── One-shot conversion [DD-051] ─────────────────────────────────── */

/* HELD: the conversion taken failed, so nothing was started and the caller
 * backs off. */
typedef enum { MS5607_STARTED, MS5607_BUSY, MS5607_NOT_BEGUN, MS5607_HELD } ms5607_start_t;

typedef struct {
    uint32_t raw;   /* D1 or D2 */
    uint64_t at_us; /* [SNS-PRES-08] the middle of the conversion, stamped by
                     * the handler from the hardware timer as it began */
    bool temperature;
    bool ok; /* false: the sensor did not answer, or the bus stuck */
} ms5607_conversion_t;

/* Claims a hardware alarm for the one-shot and installs its handler. */
bool ms5607_async_begin(void);

/* Hands the handler a conversion to command. BUSY: the last one is still in
 * flight, and nothing was sent. */
ms5607_start_t ms5607_async_start(bool temperature);

/* The conversion the one-shot finished, once. */
bool ms5607_async_take(ms5607_conversion_t *out);

/* Once a loop: takes the finished conversion into *out, notes a temperature
 * on *t, and starts the next conversion, the temperature when due, before
 * returning. Whatever the caller then does with a pressure cannot delay the
 * next command. False when nothing was finished. */
bool ms5607_async_cycle(ms5607_temps_t *t, ms5607_conversion_t *out, ms5607_start_t *started);

#endif /* MS5607_DRIVER_H */
