/*
 * Pins this board offers to Lua — Pyro MK1A.
 *
 * J6, the serial expansion header:
 *      J6.1 SCL1 = GPIO19
 *      J6.2 SDA1 = GPIO18
 *      J6.3 1-wire serial (GPIO0/GPIO1 via D7/R17/R19)
 *      J6.4/5 GND
 *
 * Only the two plain pads are offered. J6.3 is NOT here: R17 pulls it up, D7
 * ties TX into RX and R19 is in series, so it is a single-wire half-duplex
 * bus sharing the telemetry UART -- not a pad that can take an ordinary
 * output, input or one half of a UART. It needs a role of its own before it
 * can be granted, and inventing one silently would give a script something
 * that echoes back everything it sends.
 *
 * pio1, matching the other boards, so a script that works on one is not
 * surprised by another.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PINS_H
#define LUA_PINS_H

#define LUA_PIO_INST  pio1
#define LUA_PIN_COUNT 2
#define LUA_PIN_LIST                                                                                                   \
    { 18, 19 }

#endif
