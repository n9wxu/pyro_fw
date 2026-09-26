/*
 * Pin map and capabilities — Pyro MK1C (bare RP2040, QFN-56).
 *
 * Verified against pyro_mk1c.kicad_sch by netlist export, not by reading
 * the schematic images:
 *     kicad-cli sch export netlist --format kicadsexpr pyro_mk1c.kicad_sch
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1C"
#define BOARD_SHORT_STR "mk1c"

/* ── Capabilities (read by src/hal_common) ───────────────────────── */
#define BOARD_HAS_BMP280 0 /* MS5607 only; no BMP280 is fitted */

/* ── Telemetry UART (out to J1 pins 4/5) ─────────────────────────── */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0 /* TX0/SDA0 -> J1.4 */
#define BOARD_PIN_UART_RX  1 /* RX0/SCL0 -> J1.5 */

/* ── Indicators ──────────────────────────────────────────────────── */
#define BOARD_PIN_LED    8  /* -> R4 1k -> D1 (blue) */
#define BOARD_PIN_BUZZER 11 /* -> Q2 AO3400A gate */

/* ── Pressure sensor (MS5607 U2 on i2c1; PS=3V3, CSB=GND) ────────── */
#define BOARD_I2C_INST     i2c1
#define BOARD_PIN_I2C_SDA  6
#define BOARD_PIN_I2C_SCL  7

/* [DD-052] The MS5607's fastest. R3 and R5, 4k7, hold fast mode's 300 ns rise
 * to about 75 pF of bus. */
#define BOARD_MS5607_I2C_HZ 400000u

/* ── Spare ───────────────────────────────────────────────────────── */
#define BOARD_PIN_SPARE_GPIO 22 /* -> J1.6 */

/* ── Pyro: TPS259570 eFuse + charge-pump arm, 2 low-side channels ──
 *
 * SAFETY: GPIO25 is BIAS_B on this board, NOT an LED. This is why
 * boards/mk1c/sdk/pyro_mk1c.h must not define PICO_DEFAULT_LED_PIN --
 * TinyUSB's BSP board_init() drives that pin as an output.
 *
 * U9's ~FLT and ILM are NOT routed to the MCU (~FLT goes to a pull-up
 * only, ILM to R130 499R -> GND), so the FLT fault path and the ILM
 * capacitance inference of DESIGN.md 7.2 have no hardware source. */
#define BOARD_PIN_ARM_TOGGLE 12 /* -> C101 10nF pump -> U9 EN/UVLO      */
#define BOARD_PIN_BIAS_A     16 /* -> D104 -> R112 330R -> ch A node    */
#define BOARD_PIN_FIRE_A     17 /* -> R108 100R -> Q103 AO3400A gate    */
#define BOARD_PIN_BIAS_BUS   23 /* -> D107 -> R120 330R -> firing bus   */
#define BOARD_PIN_FIRE_B     24 /* -> R113 100R -> Q104 AO3400A gate    */
#define BOARD_PIN_BIAS_B     25 /* -> D106 -> R117 330R -> ch B node    */

#define BOARD_PIN_SNS_VBAT 26 /* ADC0, pack voltage  */
#define BOARD_PIN_SNS_BUS  27 /* ADC1, firing bus    */
#define BOARD_PIN_SNS_A    28 /* ADC2, channel A     */
#define BOARD_PIN_SNS_B    29 /* ADC3, channel B     */

#define BOARD_ADC_CH_VBAT 0
#define BOARD_ADC_CH_BUS  1
#define BOARD_ADC_CH_A    2
#define BOARD_ADC_CH_B    3

#endif
