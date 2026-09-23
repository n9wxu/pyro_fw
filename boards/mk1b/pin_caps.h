/*
 * Pyro MK1B — what each pin may become.
 *
 * Rows say what the HARDWARE supports, not what is assigned. See
 * src/pin_model.h for the vocabulary and the build-time checks.
 *
 * Same topology as MK1A: two switched high sides and one common low side.
 * U5 AP2192 is a dual high-side switch whose EN1/EN2 gate OUT1/OUT2 into
 * Switched_BAT1/2, and Q1B AO6800 is the common low side gated by sw_gnd.
 * CN1 carries igniter 1 on pin 1, igniter 2 on pin 4 and the common return on
 * pins 2-3, so shorting a channel pin to the common pins gives the bridge
 * midpoint.
 *
 * Protection differs from MK1A and the UI warning must follow it: F2 is a
 * 1.5 A self-resetting PTC and the AP2192 current-limits with FLG1/FLG2
 * wired back to the MCU, so a shoot-through here trips and recovers.
 *
 * Note for anything reading sense on this board: the AP2192 has an internal
 * ~100 ohm output bleed. Against the 100k pull-ups (R26, R19) that holds the
 * sense node near 4 counts with the high side off, so continuity
 * discrimination is a few counts here where MK1A gets a full-scale swing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_HIGH_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_PTC_LIMITED

/* ── PIO allocation ───────────────────────────────────────────────
 *
 * PIO0 is the pyro block and PIO1 is Lua's. A released pyro pad is still pyro
 * hardware, so a half-bridge on it runs on PIO0 under pyro rules whoever is
 * commanding it -- which also keeps Lua's four state machines free for Lua's
 * own roles. */
#define PYRO_PIO_INST pio0
#define LUA_PIO_INST pio1

/* Only GPIO8. GPIO0/GPIO1 are the telemetry UART: they can be moved to Lua,
 * but that is a configuration decision that costs the downlink, and nothing
 * in the firmware can hand the UART back yet if the VM dies. */
#define LUA_PIN_COUNT 1
#define LUA_PIN_LIST                                                                                                   \
    { 8 }

/*        pin  functions                                              group     */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE)                                                                                          \
    X(1, FN_UART_RX, PG_NONE)                                                                                          \
    X(6, FN_I2C_SDA, PG_NONE)  /* BMP280 SDA                          */                                               \
    X(7, FN_I2C_SCL, PG_NONE)                                                                                          \
    X(8, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE) /* J1 user pad */                                        \
    X(10, FN_I2C_SDA, PG_NONE) /* MS5607 SDA                          */                                               \
    X(15, FN_PYRO_COMMON | FN_DIGITAL | FN_BRIDGE, PG_COMMON) /* Q1B, common low */                                               \
    X(16, FN_BUZZER, PG_NONE)                                                                                          \
    X(21, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH1) /* AP2192 EN   */                                               \
    X(22, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH2) /* AP2192 EN   */                                               \
    X(25, FN_LED, PG_NONE)                                                                                             \
    X(26, FN_PYRO_SENSE | FN_ANALOG, PG_NONE) /* ADC0                 */                                               \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE) /* ADC1                 */

#endif
