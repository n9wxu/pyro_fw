/*
 * What each MK1C-SD pin may become: MK1C's (../mk1c/pin_caps.h) less J3 and
 * J1.6, which are the SPI bus [DD-075], and so less Lua's pads. src/pin_model.h
 * holds the vocabulary and the build-time checks.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_CAPS_H
#define PIN_CAPS_H

#include "pin_model.h"

#define BOARD_PYRO_TOPOLOGY PYRO_TOPO_LOW_SWITCHED
#define BOARD_PYRO_PROTECTION PYRO_PROT_EFUSE

#define PYRO_PIO_INST pio0 /* the pyro block: released pyro pads run here too */
#define LUA_PIO_INST pio1

#define LUA_PIN_COUNT 0
#define LUA_PIN_LIST {}

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
    X(24, FN_PYRO_FIRE | FN_DIGITAL | FN_PWM, PG_CH2, "match B terminal")                                              \
    X(26, FN_ANALOG, PG_NONE, "ADC0 pack volts")                                                                       \
    X(27, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC1 firing bus")                                                       \
    X(28, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC2 channel A")                                                        \
    X(29, FN_PYRO_SENSE | FN_ANALOG, PG_NONE, "ADC3 channel B")

#endif
