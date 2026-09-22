/*
 * Pico SDK board header — Pyro MK1A
 *
 * Bare RP2040 (QFN-56), 12MHz crystal Y1, W25Q128JVS 16MB QSPI flash.
 * Selected by CMake via PICO_BOARD=pyro_mk1a.
 *
 * This header exists for the flash size, not for the LED. GPIO25 really is
 * an LED here (via R12/R13 to D3/D4), so unlike MK1C it is safe for
 * TinyUSB's BSP to drive PICO_DEFAULT_LED_PIN -- but the stock boards/pico.h
 * also declares 2MB of flash, which would give pico_fota_bootloader a wrong
 * A/B slot map on a 16MB part.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _BOARDS_PYRO_MK1A_H
#define _BOARDS_PYRO_MK1A_H

// For board detection
#define RASPBERRYPI_PYRO_MK1A

// --- UART (telemetry + ground-test RX, out to J6) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED: GPIO25 is a real LED on this board (contrast MK1C) ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// --- I2C (BMP280 on i2c0, SDA0/SCL0) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 20
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 21
#endif

// --- FLASH: W25Q128JVS, 128 Mbit = 16 MB ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

/* Not a comment and not optional -- cmake/generic_board.cmake greps board
 * headers for this line to set the CMake-side variable that
 * pico_fota_bootloader's linker script substitutes. See the longer note in
 * boards/mk1c/sdk/pyro_mk1c.h. Keep the directive and the #define in step. */
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
