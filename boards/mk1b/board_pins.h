/*
 * Pin map — Pyro MK1B (a bare RP2040, U6), read from the
 * pyro_mk1b.kicad_sch netlist. See THEORY_OF_OPERATION.md "Pins".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1B"
#define BOARD_SHORT_STR "mk1b"

#define BOARD_HAS_BMP280 1

/* Telemetry: J1.4, J1.5. */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0
#define BOARD_PIN_UART_RX  1

#define BOARD_PIN_LED    25 /* -> R4 1k -> D3 (blue) */
#define BOARD_PIN_BUZZER 16 /* Q1A's gate: BUZZER1 from VIN */

/* Two sensors' SDA pads on one SCL; a board carries one sensor. See
 * THEORY_OF_OPERATION.md "Pressure sensor". */
#define BOARD_I2C_INST       i2c1
#define BOARD_PIN_I2C_SCL    7
#define BOARD_PIN_BMP280_SDA 6
#define BOARD_PIN_MS5607_SDA 10
#define BOARD_MS5607_I2C_HZ  400000u /* R10, R11 4k7 [DD-052]        */
#define BOARD_BMP280_I2C_HZ  100000u /* no pull-up but the RP2040's */

/* See THEORY_OF_OPERATION.md "Pyro circuit". */
#define BOARD_PIN_PYRO_LOW 15 /* -> Q1B gate, the shared low side (net PYRO_COMMON_EN) */
#define BOARD_PIN_PYRO1_EN       21 /* -> U5 EN2 -> OUT2, channel 1     */
#define BOARD_PIN_PYRO2_EN       22 /* -> U5 EN1 -> OUT1, channel 2     */
#define BOARD_PIN_PYRO1_FLAG     17 /* <- U5 FLG2, active low           */
#define BOARD_PIN_PYRO2_FLAG     18 /* <- U5 FLG1, active low           */
#define BOARD_PIN_PYRO1_SENSE    26
#define BOARD_PIN_PYRO2_SENSE    27
#define BOARD_ADC_CH_SENSE1      0
#define BOARD_ADC_CH_SENSE2      1

#endif
