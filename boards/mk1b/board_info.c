/*
 * What `picotool info -a` says about an attached MK1B: the pins that differ
 * between boards, so an image for another board is caught before it is
 * loaded. PICO_BOARD is the SDK's `pico`, so these and the program name are
 * what identify it. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"
#include "pico/binary_info.h"
#include "sdk_default_pins.h"

SDK_DEFAULT_PIN_IS_FREE(BOARD_PIN_PYRO_LOW);
SDK_DEFAULT_PIN_IS_FREE(BOARD_PIN_PYRO1_EN);
SDK_DEFAULT_PIN_IS_FREE(BOARD_PIN_PYRO2_EN);

bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (D3 blue)"));
bi_decl(bi_1pin_with_name(BOARD_PIN_BUZZER, "buzzer"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX -> J1.4", BOARD_PIN_UART_RX, "UART0 RX <- J1.5"));
bi_decl(bi_3pins_with_names(BOARD_PIN_I2C_SCL, "I2C1 SCL", BOARD_PIN_BMP280_SDA, "I2C1 SDA (BMP280)",
                            BOARD_PIN_MS5607_SDA, "I2C1 SDA (MS5607)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_PYRO_LOW, "PYRO shared low side (Q1B)", BOARD_PIN_PYRO1_EN,
                            "PYRO1 high side (U5 EN2)", BOARD_PIN_PYRO2_EN, "PYRO2 high side (U5 EN1)"));
bi_decl(bi_4pins_with_names(BOARD_PIN_PYRO1_FLAG, "PYRO1 FLAG (AP2192)", BOARD_PIN_PYRO2_FLAG, "PYRO2 FLAG (AP2192)",
                            BOARD_PIN_PYRO1_SENSE, "PYRO sense 1 (ADC0)", BOARD_PIN_PYRO2_SENSE,
                            "PYRO sense 2 (ADC1)"));
