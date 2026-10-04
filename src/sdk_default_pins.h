/*
 * The pins the SDK board header (PICO_BOARD) names as defaults, for a board's
 * build-time check that none of them is a pyro pin: SDK or BSP code that uses
 * a default -- TinyUSB's board_init() drives PICO_DEFAULT_LED_PIN -- would
 * hand that pin's FET gate to whatever it drives.
 *
 *   SDK_DEFAULT_PIN_IS_FREE(BOARD_PIN_FIRE_A);
 *
 * Include after the board header is in (pico.h, or any SDK header).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDK_DEFAULT_PINS_H
#define SDK_DEFAULT_PINS_H

#define SDK_PIN_NONE (-1)

#ifdef PICO_DEFAULT_UART_TX_PIN
#define SDK_PIN_UART_TX PICO_DEFAULT_UART_TX_PIN
#else
#define SDK_PIN_UART_TX SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_UART_RX_PIN
#define SDK_PIN_UART_RX PICO_DEFAULT_UART_RX_PIN
#else
#define SDK_PIN_UART_RX SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_LED_PIN
#define SDK_PIN_LED PICO_DEFAULT_LED_PIN
#else
#define SDK_PIN_LED SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_WS2812_PIN
#define SDK_PIN_WS2812 PICO_DEFAULT_WS2812_PIN
#else
#define SDK_PIN_WS2812 SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_I2C_SDA_PIN
#define SDK_PIN_I2C_SDA PICO_DEFAULT_I2C_SDA_PIN
#else
#define SDK_PIN_I2C_SDA SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_I2C_SCL_PIN
#define SDK_PIN_I2C_SCL PICO_DEFAULT_I2C_SCL_PIN
#else
#define SDK_PIN_I2C_SCL SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_SPI_SCK_PIN
#define SDK_PIN_SPI_SCK PICO_DEFAULT_SPI_SCK_PIN
#else
#define SDK_PIN_SPI_SCK SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_SPI_TX_PIN
#define SDK_PIN_SPI_TX PICO_DEFAULT_SPI_TX_PIN
#else
#define SDK_PIN_SPI_TX SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_SPI_RX_PIN
#define SDK_PIN_SPI_RX PICO_DEFAULT_SPI_RX_PIN
#else
#define SDK_PIN_SPI_RX SDK_PIN_NONE
#endif
#ifdef PICO_DEFAULT_SPI_CSN_PIN
#define SDK_PIN_SPI_CSN PICO_DEFAULT_SPI_CSN_PIN
#else
#define SDK_PIN_SPI_CSN SDK_PIN_NONE
#endif

#define SDK_DEFAULT_NAMES(pin)                                                                                         \
    ((pin) == SDK_PIN_UART_TX || (pin) == SDK_PIN_UART_RX || (pin) == SDK_PIN_LED || (pin) == SDK_PIN_WS2812 ||        \
     (pin) == SDK_PIN_I2C_SDA || (pin) == SDK_PIN_I2C_SCL || (pin) == SDK_PIN_SPI_SCK || (pin) == SDK_PIN_SPI_TX ||    \
     (pin) == SDK_PIN_SPI_RX || (pin) == SDK_PIN_SPI_CSN)

#define SDK_DEFAULT_PIN_IS_FREE(pin)                                                                                   \
    _Static_assert(!SDK_DEFAULT_NAMES(pin), #pin " is a PICO_DEFAULT_*_PIN in the SDK board header")

#endif
