/*
 * The flight software driven as the hardware drives it, through the mocked
 * HAL [HAL-05, TST-01]: power on, then one pass of the main loop per tick.
 *
 * Whatever a test stores before the first tick -- a configuration file, a pad
 * record, a reset cause, a pad pressure -- is what the board finds at
 * power-on.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_HARNESS_H
#define BOARD_HARNESS_H

#include "../src/beep_codes.h"
#include "../src/flight_states.h"
#include "../src/ground_test_seq.h"
#include <stdbool.h>
#include <stdint.h>

#define SENSOR_RMS_PA 1.2f /* MS5607 at OSR 4096 */

extern flight_context_t ctx;

/* Resets the mocks: a sea-level pad, the MS5607's noise seeded by seed. */
void boot_like_hardware(uint32_t seed);
/* config.ini as the board will find it at power-on [CFG-10]. */
void harness_config(const char *ini);
void harness_usb(bool attached);
/* Before power-on: the pin assignment gives this pyro channel's pad to the
 * script [PYR-HEALTH-02]. */
void harness_give_to_script(uint8_t channel);
void tick(uint32_t t);
/* The processor starts again: its RAM is gone, its stored files are not. */
void harness_restart(reset_cause_t cause);
uint32_t run_to_pad(uint32_t *t); /* returns the time PAD_IDLE began */
bool booting(void);

/* The loop clock, late against the sample clock by what this returns for
 * each tick. NULL, the default: no lag. */
extern uint32_t (*loop_lag_ms)(uint32_t t);

/* What the buzzer was asked to say. */
extern int harness_spec_plays, harness_usb_ok_plays, harness_altitude_plays, harness_stops;
extern beep_spec_t harness_last_spec;
extern int32_t harness_last_altitude;
extern gt_sound_t harness_gt_sounds[32];
extern int harness_gt_sound_n;
bool harness_last_said(beep_reason_t reason);

void write_pad_record(int32_t ground_pa);
bool pad_record_stored(void);

/* The flight log, rendered as its download renders it: how many rows carry
 * this event, and the flight time of the first, in seconds (negative: none). */
int harness_log_events(const char *event);
float harness_log_event_time(const char *event);

#endif
