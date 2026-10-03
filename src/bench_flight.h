/*
 * A flight on the bench [DD-078]: a profile from flight_sim.h whose pressure
 * replaces the sensor's, so the flight software and both logs fly it on the
 * real board.
 *
 * Every call is the flight task's: start and stop come through flight_call(),
 * the pressure from the sensor's tick, the channels from the flight machine.
 *
 * From the start of one, every fire is mocked and a fired channel reads open,
 * as a lit charge does, until the board reboots: a flight stopped part-way
 * hands the machine the pad's pressure mid-descent, and a main set by
 * altitude would fire into it. The channels read good until they fire, so
 * the whole plan flies on a bench with nothing connected.
 *
 * Pure: host-tested (test_flight_sim.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BENCH_FLIGHT_H
#define BENCH_FLIGHT_H

#include <stdbool.h>
#include <stdint.h>
#include "flight_sim.h"

typedef enum {
    BF_STARTED = 0,
    BF_NOT_ON_PAD,  /* the machine is not in PAD_IDLE */
    BF_NOT_TESTING, /* test mode is off: on USB nothing would fly [USB-08] */
    BF_RUNNING,     /* one is already flying */
    BF_BAD_PROFILE, /* fsim_start() refused it */
} bf_start_t;

/* ground_pa: the flight software's own ground reference. */
bf_start_t bench_flight_start(const fsim_params_t *p, float ground_pa, bool on_pad, bool test_mode, uint64_t now_us);

/* The profile stops feeding pressure; the channels stay mocked. */
void bench_flight_stop(void);

/* The profile's pressure at a reading's own time, in place of the reading.
 * landed: the flight machine is in LANDED, which ends the profile once it
 * has landed too. False, and *pa untouched, when none is flying. */
bool bench_flight_pressure(uint64_t stamp_us, bool landed, float *pa);

/* Whether the channels are the bench's: from the first start until reboot. */
bool bench_flight_mocked(void);

/* A mocked fire, and what the channel reads after it. A fire is energised
 * for BENCH_PULSE_MS, as a real one is, so the machine records it as fired
 * and holds the other channel off the common until it ends [PYR-DEPLOY-02]. */
#define BENCH_PULSE_MS 500u
void bench_flight_fire(uint8_t channel, uint32_t now_ms);
bool bench_flight_fired(uint8_t channel);
bool bench_flight_firing(uint32_t now_ms);

typedef struct {
    bool flying, mocked;
    fsim_params_t p;
    fsim_phase_t phase;
    float t_s, alt_m, pa, ground_pa, peak_m;
    float t_apogee_s; /* the profile's, from the start */
    uint32_t flights;
    uint32_t fires[2];
} bench_flight_status_t;

void bench_flight_status(bench_flight_status_t *out);

/* At start-up: no flight, and the channels are the board's. */
void bench_flight_init(void);

#endif
