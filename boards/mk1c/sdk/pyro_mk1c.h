/*
 * Pico SDK board header — Pyro MK1C
 *
 * Bare RP2040 (QFN-56), 12MHz crystal, XT25F128FWOIGT-W 16MB QSPI flash.
 * Selected by CMake via PICO_BOARD=pyro_mk1c.
 *
 * CRITICAL: this header must NOT define PICO_DEFAULT_LED_PIN.
 *
 * TinyUSB's RP2040 BSP (lib/tinyusb/hw/bsp/rp2040/family.c:152-155) does
 *     gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT);
 * inside board_init() for whatever PICO_DEFAULT_LED_PIN says. The stock
 * boards/pico.h sets that to 25 — which on MK1C is BIAS_B, a pyro bias
 * injector. Leaving it undefined is the entire reason this board header
 * exists rather than reusing PICO_BOARD=pico.
 *
 * The MK1C heartbeat LED (D1, blue) is GPIO8 and is driven directly by
 * hal_hardware.c, not by the BSP.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _BOARDS_PYRO_MK1C_H
#define _BOARDS_PYRO_MK1C_H

// For board detection
#define RASPBERRYPI_PYRO_MK1C

// --- UART (telemetry + ground-test RX, out to J1 pins 4/5) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// Deliberately NOT defined. See the header comment above.
// #define PICO_DEFAULT_LED_PIN 8   <-- do not do this

// --- I2C (MS5607 pressure sensor on i2c1) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- SPI (unused on this board; give the SDK sane defaults) ---
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
#define PICO_DEFAULT_SPI_RX_PIN 16
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// --- FLASH: XT25F128FWOIGT-W, 128 Mbit = 16 MB ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

/* The pico_board_cmake_set_default() line is NOT a comment and NOT optional.
 * cmake/generic_board.cmake greps board headers for it to set the CMake-side
 * variable of the same name. pico_fota_bootloader's linker_definitions.in
 * substitutes @PICO_FLASH_SIZE_BYTES@ from that CMake variable, so omitting
 * this line yields "__FLASH_SIZE =  - __FILESYSTEM_SIZE" -- an empty
 * substitution producing a NEGATIVE flash size and a garbage A/B slot map
 * that still links. Keep the directive and the #define in agreement. */
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// Bootloader button is a dedicated switch (SW1 -> nBOOTSEL), not a GPIO.
#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
