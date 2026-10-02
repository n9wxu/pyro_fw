/*
 * Pin map — Pyro MK1C (bare RP2040, QFN-56), checked by netlist export of
 * pyro_mk1c.kicad_sch. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1C"
#define BOARD_SHORT_STR "mk1c"

#define BOARD_HAS_BMP280 0

/* Telemetry: J1.4, J1.5. */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0
#define BOARD_PIN_UART_RX  1

#define BOARD_PIN_LED    8  /* -> R4 1k -> D1 (blue) */
#define BOARD_PIN_BUZZER 11 /* -> Q2 AO3400A gate    */

/* MS5607 U2; PS=3V3 (I2C), CSB=GND. */
#define BOARD_I2C_INST      i2c1
#define BOARD_PIN_I2C_SDA   6
#define BOARD_PIN_I2C_SCL   7
#define BOARD_MS5607_I2C_HZ 400000u /* R3, R5 4k7 [DD-052] */

/* See THEORY_OF_OPERATION.md "Firing bus". GPIO25 is BIAS_B, not an LED. */
#define BOARD_PIN_ARM_TOGGLE 12 /* -> C101 10nF pump -> U9 EN/UVLO */
#define BOARD_PIN_BIAS_A     16 /* -> D104 -> R112 330R -> node A  */
#define BOARD_PIN_FIRE_A     17 /* -> R108 100R -> Q103 gate       */
#define BOARD_PIN_BIAS_BUS   23 /* -> D107 -> R120 330R -> the bus */
#define BOARD_PIN_FIRE_B     24 /* -> R113 100R -> Q104 gate       */
#define BOARD_PIN_BIAS_B     25 /* -> D106 -> R117 330R -> node B  */

#define BOARD_PIN_SNS_VBAT 26
#define BOARD_PIN_SNS_BUS  27
#define BOARD_PIN_SNS_A    28
#define BOARD_PIN_SNS_B    29

#define BOARD_ADC_CH_VBAT 0
#define BOARD_ADC_CH_BUS  1
#define BOARD_ADC_CH_A    2
#define BOARD_ADC_CH_B    3

#endif
