/*
 * Pin map and capabilities — Pyro MK1B (Raspberry Pi Pico module).
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1B"
#define BOARD_SHORT_STR "mk1b"

/* ── Capabilities (read by src/hal_common) ───────────────────────── */
#define BOARD_HAS_BMP280 1 /* BMP280 fitted alongside the MS5607 */

/* ── Telemetry UART (TRRS jack) ──────────────────────────────────── */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0
#define BOARD_PIN_UART_RX  1

/* ── Indicators ──────────────────────────────────────────────────── */
#define BOARD_PIN_LED    25 /* onboard LED on the Pico module */
#define BOARD_PIN_BUZZER 16

/* ── Pressure sensors (i2c1) ─────────────────────────────────────── */
#define BOARD_I2C_INST     i2c1
#define BOARD_PIN_I2C_SCL  7
#define BOARD_PIN_BMP280_SDA 6
#define BOARD_PIN_MS5607_SDA 10

/* [DD-052] Read from the KiCad board: SCL and the MS5607's SDA have 4k7 (R10,
 * R11), which hold fast mode's 300 ns rise to about 75 pF, so the MS5607
 * runs at its fastest. The BMP280's SDA has no pull-up but the RP2040's own
 * 50-80k, too slow an edge for fast mode, so its probe stays in standard
 * mode. */
#define BOARD_MS5607_I2C_HZ 400000u
#define BOARD_BMP280_I2C_HZ 100000u

/* ── Pyro (AP2192 high-side switches) ────────────────────────────── */
#define BOARD_PIN_PYRO_COMMON_EN 15
#define BOARD_PIN_PYRO1_EN       21
#define BOARD_PIN_PYRO2_EN       22
#define BOARD_PIN_PYRO1_FLAG     17 /* AP2192 FLAG1, active low */
#define BOARD_PIN_PYRO2_FLAG     18 /* AP2192 FLAG2, active low */
#define BOARD_PIN_PYRO1_SENSE    26 /* ADC0 */
#define BOARD_PIN_PYRO2_SENSE    27 /* ADC1 */

#endif
