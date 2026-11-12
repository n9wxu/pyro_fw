/*
 * The reset asked for over USB, acted on by the main loop.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RESET_INTERFACE_H
#define RESET_INTERFACE_H

#include <stdint.h>

extern volatile uint8_t pending_reset; /* 1=BOOTSEL, 2=flash */

#endif
