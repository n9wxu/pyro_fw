/*
 * Pins this board offers to Lua — Pyro MK1C.
 *
 * J3 is a dedicated user breakout:
 *      J3.1 +3.3V   J3.2 GND
 *      J3.3 GPIO18  J3.4 GPIO19
 *      J3.5 GPIO20  J3.6 GPIO21
 *
 * (verified by netlist export from pyro_mk1c.kicad_sch, not by reading the
 * schematic: J3 pins 3-6 map to U3 QFN pins 29/30/31/32.)
 *
 * pio1, because pio0 is core0's ARM_TOGGLE charge pump and is not shared.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PINS_H
#define LUA_PINS_H

#define LUA_PIO_INST  pio1
#define LUA_PIN_COUNT 4
#define LUA_PIN_LIST                                                                                                   \
    { 18, 19, 20, 21 }

#endif
