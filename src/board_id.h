/*
 * Board identity strings, sourced from the selected board package.
 *
 * boards/<name>/board_pins.h is the single place a board names itself;
 * this header just republishes it under the names shared code uses, so
 * nothing outside boards/ has to know which boards exist.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_ID_H
#define BOARD_ID_H

#include "board_pins.h"

#ifndef BOARD_NAME_STR
#error "board_pins.h must define BOARD_NAME_STR (see boards/reference/)"
#endif
#ifndef BOARD_SHORT_STR
#error "board_pins.h must define BOARD_SHORT_STR (see boards/reference/)"
#endif

#define PYRO_BOARD_NAME BOARD_NAME_STR
#define PYRO_BOARD_SHORT BOARD_SHORT_STR

#endif
