/*
 * Pico SDK board header — Pyro MK1C, selected by PICO_BOARD=pyro_mk1c.
 *
 * Every PICO_DEFAULT_*_PIN here is a pin SDK or BSP code may drive, so none
 * may be a pyro pin; boards/mk1c/board_info.c checks that at build time. The
 * stock boards/pico.h fails it twice: its LED, 25, is BIAS_B -- TinyUSB's
 * board_init() drives PICO_DEFAULT_LED_PIN (lib/tinyusb/hw/bsp/rp2040/
 * family.c) -- and its SPI RX and CSn, 16 and 17, are BIAS_A and FIRE_A.
 * The LED, D1 on GPIO8, is board_defaults.c's. ../THEORY_OF_OPERATION.md "Pins"
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _BOARDS_PYRO_MK1C_H
#define _BOARDS_PYRO_MK1C_H

// For board detection
#define RASPBERRYPI_PYRO_MK1C

// --- UART0: telemetry and ground-test commands, J1.4/J1.5 ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED: none. See the header comment. ---

// --- I2C1: the MS5607 ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- SPI0 on J3, as mk1c_sd uses it: ../THEORY_OF_OPERATION.md "J3 as an SPI port" ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 20
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 21
#endif

// --- FLASH: XT25F128FWOIGT-W, 128 Mbit = 16 MB ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

/* Not a comment: cmake/generic_board.cmake greps board headers for this line
 * to set the CMake variable pico_fota_bootloader's linker_definitions.in
 * substitutes. Without it __FLASH_SIZE is empty, and the A/B slot map is
 * garbage that still links. Keep the directive and the #define in step. */
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
