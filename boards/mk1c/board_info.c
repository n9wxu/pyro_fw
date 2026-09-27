/*
 * What `picotool info -a` says about an attached MK1C: the pins that differ
 * between boards, so an image for another board is caught before it is
 * loaded. GPIO25 is the reason it matters most here: it is BIAS_B, where
 * MK1A and MK1B put their LED. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"
#include "pico/binary_info.h"

bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (D1 blue)"));
bi_decl(bi_1pin_with_name(BOARD_PIN_BUZZER, "buzzer (Q2 gate)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX -> J1.4", BOARD_PIN_UART_RX, "UART0 RX <- J1.5"));
bi_decl(bi_2pins_with_names(BOARD_PIN_I2C_SDA, "I2C1 SDA (MS5607)", BOARD_PIN_I2C_SCL, "I2C1 SCL (MS5607)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_ARM_TOGGLE, "PYRO ARM_TOGGLE (U9 charge pump)", BOARD_PIN_FIRE_A,
                            "PYRO FIRE_A (low side)", BOARD_PIN_FIRE_B, "PYRO FIRE_B (low side)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_BIAS_A, "PYRO BIAS_A", BOARD_PIN_BIAS_BUS, "PYRO BIAS_BUS", BOARD_PIN_BIAS_B,
                            "PYRO BIAS_B (NOT an LED)"));
bi_decl(bi_4pins_with_names(BOARD_PIN_SNS_VBAT, "sense VBAT (ADC0)", BOARD_PIN_SNS_BUS, "sense BUS (ADC1)",
                            BOARD_PIN_SNS_A, "sense ch A (ADC2)", BOARD_PIN_SNS_B, "sense ch B (ADC3)"));
