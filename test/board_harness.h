/*
 * The board, driven the way the hardware drives it, for suites that fly a
 * truth beside the firmware (pressure_chain_tests, mach_tests).
 *
 * The first tick runs flight_init(), which reads the reset cause and the
 * sensor, so anything a test sets up before it -- a marker, a reset cause, a
 * pad pressure -- is what the board finds at power-on. Then each tick is one
 * pass of main_hardware.c's loop, in its order.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_HARNESS_H
#define BOARD_HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include "../src/flight_states.h"

#define SENSOR_RMS_PA 1.2f /* MS5607 at OSR 4096 */

extern flight_context_t ctx;

/* What a test sets before power-on, applied after flight_init() as the
 * hardware's main loop would find it. */
typedef struct {
    bool powered;
    bool usb;
    int landing_timeout; /* -1: the default */
} board_power_t;
extern board_power_t power;

/* Nothing primed, from BOOT_SETTLE, on a sea-level pad, with the MS5607's
 * noise seeded by seed. */
void boot_like_hardware(uint32_t seed);
void tick(uint32_t t);
uint32_t run_to_pad(uint32_t *t); /* returns the time PAD_IDLE began */
bool booting(void);

void write_marker(int32_t ground_pa);
bool marker_valid(void);

#endif
