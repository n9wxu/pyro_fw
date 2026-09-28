/*
 * What each MK1B pin may become; src/pin_model.h holds the vocabulary and
 * the build-time checks. See THEORY_OF_OPERATION.md "Lua and released pads".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_HIGH_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_PTC_LIMITED

#define PYRO_PIO_INST pio0 /* the pyro block: released pyro pads run here too */
#define LUA_PIO_INST pio1

/* Not GPIO0/1: moving the telemetry UART to Lua costs the downlink. */
#define LUA_PIN_COUNT 1
#define LUA_PIN_LIST                                                                                                   \
    { 8 }

/*        pin  functions                                       group      connector   */
#define BOARD_PIN_CAPS(X)                                                                                              \
    X(0, FN_UART_TX, PG_NONE, "J1 TX")                                                                                 \
    X(1, FN_UART_RX, PG_NONE, "J1 RX")                                                                                 \
    X(6, FN_I2C_SDA, PG_NONE, "BMP280 SDA")                                                                            \
    X(7, FN_I2C_SCL, PG_NONE, "BMP280 SCL")                                                                            \
    X(8, FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL, PG_NONE, "J1 user pad")                                           \
    X(10, FN_I2C_SDA, PG_NONE, "MS5607 SDA")                                                                           \
    X(15, FN_PYRO_COMMON | FN_DIGITAL | FN_BRIDGE, PG_COMMON, "CN1.2-3 common")                                        \
    X(16, FN_BUZZER, PG_NONE, "BUZZER1 via Q1A")                                                                       \
    X(21, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH1, "CN1.1 drogue")                                      \
    X(22, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM | FN_BRIDGE, PG_CH2, "CN1.4 main")                                        \
    X(25, FN_LED, PG_NONE, "D1 status LED")                                                                            \
    X(26, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC0 sense 1")                                                          \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC1 sense 2")

#endif
