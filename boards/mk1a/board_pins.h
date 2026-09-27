/*
 * Pin map — Pyro MK1A (bare RP2040, QFN-56), checked against
 * ~/Documents/Pyro_mk1a.pdf. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1A"
#define BOARD_SHORT_STR "mk1a"

#define BOARD_HAS_BMP280 1

/* Telemetry: one half-duplex wire, J6.3. */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0 /* -> D7 1N4148 -> the J6.3 node */
#define BOARD_PIN_UART_RX  1 /* <- the J6.3 node               */

#define BOARD_PIN_LED 25 /* -> R12 1k -> D3 */
/* No buzzer is fitted, so no BOARD_PIN_BUZZER: it starts on no pad. */

/* BMP280 U4, address 0x77. */
#define BOARD_I2C_INST    i2c0
#define BOARD_PIN_I2C_SDA 20
#define BOARD_PIN_I2C_SCL 21
#define BOARD_BMP280_I2C_HZ 400000u /* R1, R2 4k7 [DD-052] */

/* See THEORY_OF_OPERATION.md "Pyro circuit". */
#define BOARD_PIN_FIRE1    9  /* -> Q6A gate, channel 1 high side */
#define BOARD_PIN_PYRO_LOW 10 /* -> Q2 gate, the shared low side  */
#define BOARD_PIN_FIRE2    11 /* -> Q1A gate, channel 2 high side */

#define BOARD_PIN_PYRO1_SENSE 26 /* via R5 1k + C6 100nF  */
#define BOARD_PIN_PYRO2_SENSE 27 /* via R14 1k + C5 100nF */
#define BOARD_ADC_CH_SENSE1   0
#define BOARD_ADC_CH_SENSE2   1

#endif
