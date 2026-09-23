/*
 * Pyro MK1A — what each pin may become.
 *
 * Rows say what the HARDWARE supports, not what is assigned. See
 * src/pin_model.h for the vocabulary and the build-time checks.
 *
 * Topology, from the header comment in pyro_board.c:
 *
 *   VBATT -> Q6 [FIRE1] -> J3 igniter -.
 *   VBATT -> Q1 [FIRE2] -> J4 igniter -+-> Initiator_ground
 *                    Initiator_ground -> F1 8A -> Q2 [PYRO_LOW] -> GND
 *
 * Two switched high sides and one common low side, so a released FIRE plus a
 * released PYRO_LOW is a half-bridge with the igniter terminals shorted as
 * the midpoint. F1 is 8 A and does not reset, which is why this board's
 * protection class differs from MK1B's.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_HIGH_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_FUSE_ONESHOT

/* Lua's PIO block. Claimed on core0 at boot; see lua_pio_platform.c. */
#define LUA_PIO_INST pio1

/* J6.3 is deliberately absent: it is half-duplex and needs a role of its own
 * before it can be granted. Granting it silently would give a script a pin
 * that echoes back everything it sends. */
#define LUA_PIN_COUNT 2
#define LUA_PIN_LIST                                                                                                   \
    { 18, 19 }

/*        pin  functions                                              group     */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE)                       /* -> R19 -> J6.4          */                                      \
    X(1, FN_UART_RX, PG_NONE)                       /* -> J6.5                 */                                      \
    X(9, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH1)  /* Q6A, high side  */                                      \
    X(10, FN_PYRO_COMMON | FN_DIGITAL | FN_BRIDGE, PG_COMMON)     /* Q2, common low  */                                     \
    X(11, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH2) /* Q1A, high side  */                                      \
    X(18, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE) /* J6 user pad  */                                      \
    X(19, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE) /* J6 user pad  */                                      \
    X(20, FN_I2C_SDA, PG_NONE)                                                                                         \
    X(21, FN_I2C_SCL, PG_NONE)                                                                                         \
    X(25, FN_LED, PG_NONE)                                                                                             \
    X(26, FN_PYRO_SENSE | FN_ANALOG, PG_NONE)                     /* ADC0, sense 1   */                                      \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE)                     /* ADC1, sense 2   */

#endif
