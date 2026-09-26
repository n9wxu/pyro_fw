/*
 * Pin map and capabilities — Pyro MK1A (bare RP2040, QFN-56).
 *
 * Checked against the MK1A schematic, ~/Documents/Pyro_mk1a.pdf, on
 * 2026-09-26: every GPIO matches. There is no KiCad source for this board to
 * export a netlist from; ~/Documents/pyro/ is a different design.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_NAME_STR  "Pyro MK1A"
#define BOARD_SHORT_STR "mk1a"

/* ── Capabilities (read by src/hal_common) ───────────────────────── */
#define BOARD_HAS_BMP280 1 /* U4 BMP280 is the only sensor fitted; no MS5607 */

/* ── Telemetry UART: J6.3, the header's one-wire serial ─────────────
 *
 * TX and RX meet at one node, R17 1k pulls it to +3V3, and R19 1k takes it to
 * J6.3. TX can only pull it low, through D7, so the line is half-duplex and
 * RX hears everything TX sends. J6.4 is +VSW and J6.5 GND. */
#define BOARD_UART_INST    uart0
#define BOARD_UART_IRQ_NUM UART0_IRQ
#define BOARD_PIN_UART_TX  0 /* -> D7 1N4148 -> the J6.3 node */
#define BOARD_PIN_UART_RX  1 /* <- the J6.3 node               */

/* ── Indicators ──────────────────────────────────────────────────── */
#define BOARD_PIN_LED 25 /* -> R12 1k -> D3; D4 (R13) is the +3V3 power light */

/* No buzzer is fitted. board_buzzer_*() in hal_board.c are no-ops, which is
 * why there is no BOARD_PIN_BUZZER here: an unused pin number invites
 * someone to drive a pad that goes nowhere. */

/* ── Pressure sensor (BMP280 U4 on i2c0, address 0x77) ───────────
 *
 * SDA0/SCL0 are GPIO20/21, which is i2c0 in the RP2040 function table.
 * SDA1/SCL1 (GPIO18/19, i2c1) go only to the J6 expansion header and their
 * pull-ups R18/R20 are marked do-not-populate, so nothing on i2c1 is fitted. */
#define BOARD_I2C_INST    i2c0
#define BOARD_PIN_I2C_SDA 20
#define BOARD_PIN_I2C_SCL 21

/* [DD-052] The BMP280's fastest. R1 and R2, 4k7, hold fast mode's 300 ns rise
 * to about 75 pF of bus. */
#define BOARD_BMP280_I2C_HZ 400000u

/* ── Pyro: per-channel high side, ONE shared low side ────────────
 *
 * Both igniters return through a single node:
 *
 *   VBATT -> Q6 DMC2053UVT (ch1 high side, gate from FIRE1)  -> J3 -> |
 *   VBATT -> Q1 DMC2053UVT (ch2 high side, gate from FIRE2)  -> J4 -> |
 *                                                    Initiator_ground |
 *            Initiator_ground -> F1 8A -> Q2 AO3400A -> GND, gate PYRO_LOW
 *
 * So current through an igniter needs BOTH its own high side AND the shared
 * low side. Either one off breaks the circuit. That is the same two-key
 * property MK1B gets from PYRO_COMMON_EN, with the shared element moved to
 * the low side.
 *
 * R9/R10 (100k) weakly pull each igniter's HIGH node to +3V3, and SENSE1/
 * SENSE2 tap that same node through a 1k series resistor with a 100nF filter.
 * Asserting PYRO_LOW alone therefore reads continuity without any current
 * from VBATT: a connected igniter ties the node down against the pull-up.
 * See boards/mk1a/pyro_board.c for the two-phase cycle and its timing.
 *
 * NOTE, hardware: during a fire pulse the high side puts VBATT on that node,
 * so SENSE_n sees the pack voltage through R5/R14 (1k). On a 2S pack that is
 * about 5 mA into the RP2040's ADC clamp for the 500 ms of the pulse. It is
 * survivable and firmware cannot change it, but it is worth knowing. */
#define BOARD_PIN_FIRE1    9  /* -> Q6A gate, R4 1k pull-down */
#define BOARD_PIN_PYRO_LOW 10 /* -> Q2 gate, R6 1k pull-down  */
#define BOARD_PIN_FIRE2    11 /* -> Q1A gate, R8 1k pull-down */

#define BOARD_PIN_PYRO1_SENSE 26 /* ADC0, via R5 1k + C6 100nF  */
#define BOARD_PIN_PYRO2_SENSE 27 /* ADC1, via R14 1k + C5 100nF */
#define BOARD_ADC_CH_SENSE1   0
#define BOARD_ADC_CH_SENSE2   1

/* Q1 and Q6 are plain dual MOSFETs, not protected switches: there is no FLAG
 * or fault output anywhere on this board, so pyro_fault() has no hardware
 * source. Compare MK1B, whose AP2192 does provide one. */

#endif
