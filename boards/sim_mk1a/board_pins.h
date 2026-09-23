/*
 * Identity — simulated Pyro MK1A.
 *
 * The pin map is the real board's, included rather than copied: this
 * package exists to run boards/mk1a/pyro_board.c unchanged, and a second
 * copy of the pin numbers would be a second thing to keep in step.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_SIM_MK1A_H
#define BOARD_PINS_SIM_MK1A_H

#include "../mk1a/board_pins.h"

/* Override the identity so telemetry and /api/status say which build this
 * is. Everything else -- pins, ADC channels, capabilities -- comes from the
 * real header above. */
#undef BOARD_NAME_STR
#undef BOARD_SHORT_STR
#define BOARD_NAME_STR  "Pyro MK1A (simulated)"
#define BOARD_SHORT_STR "sim_mk1a"

/* No sensors are fitted; the simulation injects pressure directly, exactly
 * as it does for boards/sim. Only the PYRO side is modelled. */
#undef BOARD_HAS_BMP280
#define BOARD_HAS_BMP280 0

#endif
