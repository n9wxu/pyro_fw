/*
 * Reference template — what each pin may become.
 *
 * Copy this with the rest of boards/reference/ and replace every TODO. Rows
 * say what the HARDWARE supports, not what is assigned; src/pin_model.h holds
 * the vocabulary and the build-time checks, and getting a row wrong fails the
 * build rather than producing a board that can be misconfigured.
 *
 * A pin with no row is not assignable to anything, which is the right state
 * for anything the flight software must keep to itself.
 *
 * A pyro pad lists FN_PYRO_* together with what it may become once its
 * channel is released. That pairing is deliberate: holding both at once is an
 * assignment-time rule, not a property of the hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

/* Which side each per-channel element switches, and what protects the common
 * path if both sides of a bridge ever conduct at once. The web UI takes its
 * shoot-through warning from the protection class, so state it honestly. */
#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_HIGH_SWITCHED /* TODO */
#define BOARD_PYRO_PROTECTION PYRO_PROT_FUSE_ONESHOT /* TODO */

/* ── PIO allocation ───────────────────────────────────────────────
 *
 * PIO0 is the pyro block and PIO1 is Lua's. A released pyro pad is still pyro
 * hardware, so a half-bridge on it runs on PIO0 under pyro rules whoever is
 * commanding it -- which also keeps Lua's four state machines free for Lua's
 * own roles. */
#define PYRO_PIO_INST pio0
#define LUA_PIO_INST pio1
#define LUA_PIN_COUNT 0
#define LUA_PIN_LIST                                                                                                   \
    { 0 }

/*        pin  functions                                    group     */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE)  /* TODO */                                                                              \
    X(1, FN_UART_RX, PG_NONE)  /* TODO */                                                                              \
    X(6, FN_I2C_SDA, PG_NONE)  /* TODO */                                                                              \
    X(7, FN_I2C_SCL, PG_NONE)  /* TODO */                                                                              \
    X(16, FN_BUZZER, PG_NONE)  /* TODO */                                                                              \
    X(25, FN_LED, PG_NONE)     /* TODO */

#endif
