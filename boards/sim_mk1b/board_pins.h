/*
 * Identity — simulated Pyro MK1B.
 *
 * The pin map is the real board's, included rather than copied: this
 * package exists to run boards/mk1b/pyro_board.c unchanged, and a second
 * copy of the pin numbers would be a second thing to keep in step.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_SIM_MK1B_H
#define BOARD_PINS_SIM_MK1B_H

#include "../mk1b/board_pins.h"

/* Override the identity so telemetry and /api/status say which build this
 * is. Everything else -- pins, ADC channels, capabilities -- comes from the
 * real header above. */
#undef BOARD_NAME_STR
#undef BOARD_SHORT_STR
#define BOARD_NAME_STR  "Pyro MK1B (simulated)"
#define BOARD_SHORT_STR "sim_mk1b"

/* No sensors are fitted; the simulation injects pressure directly, exactly
 * as it does for boards/sim. Only the PYRO side is modelled. */
#undef BOARD_HAS_BMP280
#define BOARD_HAS_BMP280 0

#endif
