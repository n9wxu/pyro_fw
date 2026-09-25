/*
 * Pyro MK1C — what each pin may become.
 *
 * Rows say what the HARDWARE supports, not what is assigned. See
 * src/pin_model.h for the vocabulary and the build-time checks.
 *
 * Topology is the mirror of MK1A and MK1B: two switched LOW sides (FIRE_A,
 * FIRE_B into AO3400A gates) and a common HIGH side, the U9 TPS259570 eFuse.
 *
 * The common is not a plain gate. U9's enable is driven by the ARM_TOGGLE
 * charge pump, which must keep toggling to stay on -- see arm_pump.pio and
 * DESIGN.md invariant 5. So ARM_TOGGLE carries no FN_DIGITAL: holding it at a
 * level does not hold the eFuse on, and a script that assumed otherwise would
 * be reasoning about a pin that behaves like no other output on any board.
 * That also means this board cannot offer a half-bridge.
 *
 * GPIO25 is BIAS_B here, NOT an LED. See board_pins.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_LOW_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_EFUSE

/* ── PIO allocation ───────────────────────────────────────────────
 *
 * PIO0 is the pyro block and PIO1 is Lua's. A released pyro pad is still pyro
 * hardware, so a half-bridge on it runs on PIO0 under pyro rules whoever is
 * commanding it -- which also keeps Lua's four state machines free for Lua's
 * own roles. */
#define PYRO_PIO_INST pio0
#define LUA_PIO_INST pio1

#define LUA_PIN_COUNT 4
#define LUA_PIN_LIST                                                                                                   \
    { 18, 19, 20, 21 }

/*        pin  functions                                       group      connector   */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE, "J1.4 TX")                                                                               \
    X(1, FN_UART_RX, PG_NONE, "J1.5 RX")                                                                               \
    X(6, FN_I2C_SDA, PG_NONE, "MS5607 SDA")                                                                            \
    X(7, FN_I2C_SCL, PG_NONE, "MS5607 SCL")                                                                            \
    X(8, FN_LED, PG_NONE, "status LED")                                                                                \
    X(11, FN_BUZZER, PG_NONE, "buzzer")                                                                                \
    X(12, FN_PYRO_COMMON, PG_COMMON, "arm disconnect")                                                                 \
    X(17, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM, PG_CH1, "match A terminal")                                              \
    X(18, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J3.3")                                                 \
    X(19, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J3.4")                                                 \
    X(20, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J3.5")                                                 \
    X(21, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J3.6")                                                 \
    X(22, FN_DIGITAL | FN_PWM, PG_NONE, "J1.6 spare")                                                                  \
    X(24, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM, PG_CH2, "match B terminal")                                              \
    X(26, FN_ANALOG, PG_NONE, "ADC0 pack volts")                                                                       \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC1 firing bus")                                                       \
    X(28, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC2 channel A")                                                        \
    X(29, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC3 channel B")

#endif
