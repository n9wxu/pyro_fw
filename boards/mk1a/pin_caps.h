/*
 * What each MK1A pin may become; src/pin_model.h holds the vocabulary and
 * the build-time checks. See THEORY_OF_OPERATION.md "Lua and released pads".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_HIGH_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_FUSE_ONESHOT

#define PYRO_PIO_INST pio0 /* the pyro block: released pyro pads run here too */
#define LUA_PIO_INST pio1

/* Not J6.3: it echoes back everything sent on it. */
#define LUA_PIN_COUNT 2
#define LUA_PIN_LIST                                                                                                   \
    { 18, 19 }

/*        pin  functions                                       group      connector   */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE, "J6.3 TX via D7")                                                                        \
    X(1, FN_UART_RX, PG_NONE, "J6.3 RX via R19")                                                                       \
    X(9, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH1, "J3 drogue")                                          \
    X(10, FN_PYRO_COMMON | FN_DIGITAL | FN_BRIDGE, PG_COMMON, "igniter common")                                        \
    X(11, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH2, "J4 main")                                           \
    X(18, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J6 user pad")                                          \
    X(19, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J6 user pad")                                          \
    X(20, FN_I2C_SDA, PG_NONE, "BMP280 SDA")                                                                           \
    X(21, FN_I2C_SCL, PG_NONE, "BMP280 SCL")                                                                           \
    X(25, FN_LED, PG_NONE, "status LED")                                                                               \
    X(26, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC0 sense 1")                                                          \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC1 sense 2")

#endif
