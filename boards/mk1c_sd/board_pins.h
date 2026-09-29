/*
 * Pin map — Pyro MK1C-SD: MK1C (../mk1c/board_pins.h) with J3 and J1.6 as an
 * SPI bus for an SD card and an LSM6DS3 [DD-075]. See
 * THEORY_OF_OPERATION.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_PINS_MK1C_SD_H
#define BOARD_PINS_MK1C_SD_H

#include "../mk1c/board_pins.h"

#undef BOARD_NAME_STR
#undef BOARD_SHORT_STR
#define BOARD_NAME_STR "Pyro MK1C-SD"
#define BOARD_SHORT_STR "mk1c_sd"

/* SPI0: each J3 pad has exactly one SPI function (rp2040-datasheet_2025-02-20.pdf,
 * section 2.19.2, Table 279, page 237). */
#define BOARD_HAS_SD 1
#define BOARD_SPI_INST spi0
#define BOARD_PIN_SPI_SCK 18  /* J3.3 */
#define BOARD_PIN_SPI_MOSI 19 /* J3.4 */
#define BOARD_PIN_SPI_MISO 20 /* J3.5, pulled up */
#define BOARD_PIN_SD_CS 21    /* J3.6 */
#define BOARD_PIN_IMU_CS 22   /* J1.6 */

/* Wire-wrapped: well below the card's 25 MHz default speed. Measured on the
 * bench (THEORY_OF_OPERATION.md "The SD card"). */
#define BOARD_SD_SPI_HZ 12500000u
/* LSM6DS3_DocID026899_Rev4_2015-04.pdf, page 23: 10 MHz at most. */
#define BOARD_IMU_SPI_HZ 10000000u

#endif
