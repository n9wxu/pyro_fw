/*
 * Pins this board offers to Lua — Pyro MK1B.
 *
 * J1 breaks out one plain pad:
 *      J1.1/2/3 power and GND
 *      J1.4 TX0/SDA0 = GPIO0
 *      J1.5 RX0/SCL0 = GPIO1
 *      J1.6 GPIO     = GPIO8
 *
 * Only GPIO8. GPIO0/GPIO1 are the telemetry UART: they can be moved to Lua,
 * but that is a configuration decision that costs the downlink, not something
 * to grant by default -- and nothing in the firmware can hand the UART back
 * yet if the VM dies. Until that exists, offering them would trade a
 * diagnostic channel for a pad.
 *
 * The Pico module exposes more free GPIOs on its castellations (2-5, 9,
 * 11-14, 19, 20) but this carrier does not route them, so they are not
 * offered: a pin a script can name but nobody can reach is worse than no pin.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PINS_H
#define LUA_PINS_H

#define LUA_PIO_INST  pio1
#define LUA_PIN_COUNT 1
#define LUA_PIN_LIST                                                                                                   \
    { 8 }

#endif
