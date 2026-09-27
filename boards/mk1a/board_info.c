/*
 * What `picotool info -a` says about an attached MK1A: the pins that differ
 * between boards, so an image for another board is caught before it is
 * loaded. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"
#include "pico/binary_info.h"

bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (D3)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX -> J6.3", BOARD_PIN_UART_RX, "UART0 RX <- J6.3"));
bi_decl(bi_2pins_with_names(BOARD_PIN_I2C_SDA, "I2C0 SDA (BMP280)", BOARD_PIN_I2C_SCL, "I2C0 SCL (BMP280)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_FIRE1, "PYRO FIRE1 (Q6 high side)", BOARD_PIN_PYRO_LOW,
                            "PYRO shared low side (Q2)", BOARD_PIN_FIRE2, "PYRO FIRE2 (Q1 high side)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_PYRO1_SENSE, "PYRO sense 1 (ADC0)", BOARD_PIN_PYRO2_SENSE,
                            "PYRO sense 2 (ADC1)"));
