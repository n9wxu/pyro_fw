/*
 * Pin map and capabilities — REFERENCE BOARD (template).
 *
 * Copy boards/reference to boards/<yourboard>, then edit every value
 * marked TODO. Nothing outside this directory needs to change.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* ── Identity ────────────────────────────────────────────────────────
 * src/board_id.h republishes these; they appear in the USB product
 * string, the web UI, /api/status and the flight-log CSV header. */
#define BOARD_NAME_STR  "Pyro REFERENCE" /* TODO */
#define BOARD_SHORT_STR "reference"      /* TODO: match the directory name */

/* ── Capabilities (read by src/hal_common) ───────────────────────────
 * Declare what is fitted. hal_common compiles the matching code paths;
 * a driver you do not list is never referenced, so it need not link. */
#define BOARD_HAS_BMP280 0 /* TODO: 1 if a BMP280 is fitted */

/* ── Telemetry UART ──────────────────────────────────────────────────
 * hal_common owns the ISR-driven TX ring buffer and the ground-test RX
 * line reader; this board only names the peripheral and its pins. */
#define BOARD_UART_INST    uart0    /* TODO */
#define BOARD_UART_IRQ_NUM UART0_IRQ /* TODO: must match BOARD_UART_INST */
#define BOARD_PIN_UART_TX  0        /* TODO */
#define BOARD_PIN_UART_RX  1        /* TODO */

/* ── Indicators ──────────────────────────────────────────────────────
 * The heartbeat LED is driven from the main loop, never from a timer or
 * PWM peripheral: a peripheral keeps blinking after the firmware stops. */
#define BOARD_PIN_LED    25 /* TODO */
#define BOARD_PIN_BUZZER 16 /* TODO */

/* ── Pressure sensor ─────────────────────────────────────────────── */
#define BOARD_I2C_INST    i2c1 /* TODO */
#define BOARD_PIN_I2C_SDA 6    /* TODO */
#define BOARD_PIN_I2C_SCL 7    /* TODO */

/* ── Pyro ────────────────────────────────────────────────────────────
 * Entirely board-specific: MK1B uses AP2192 high-side switches, MK1C a
 * TPS259570 eFuse with a charge-pump arm. Declare whatever pyro_board.c
 * needs and nothing more.
 *
 * SAFETY: check every pyro pin against the schematic netlist rather than
 * a schematic image. On MK1C, GPIO25 is a pyro bias injector while on
 * MK1B it is the LED -- a pin that looks harmless on one board can drive
 * a firing circuit on another. */
/* TODO: your pyro pins */

#endif
