/*
 * Pico SDK board header — Pyro MK1B-8MB, selected by PICO_BOARD=pyro_mk1b_8mb:
 * the stock boards/pico.h, which MK1B uses, with U7 a BY25Q64ESHIG, 64 Mbit =
 * 8 MB. Its boot stage 2 is pico.h's W25Q080 one (../THEORY_OF_OPERATION.md
 * "Flash").
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _BOARDS_PYRO_MK1B_8MB_H
#define _BOARDS_PYRO_MK1B_8MB_H

/* cmake/generic_board.cmake reads this file only, not pico.h: this line is
 * what pico_fota_bootloader's slot map is built from. */
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (8 * 1024 * 1024))
#define PICO_FLASH_SIZE_BYTES (8 * 1024 * 1024)

#include "boards/pico.h"

#endif
