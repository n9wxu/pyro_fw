/*
 * The src/board_if.h functions every board so far implements the same way:
 * plain GPIO on the pins board_pins.h names. Each is weak, so a board with
 * different hardware behind one -- a piezo driver, a PWM slice -- defines its
 * own in hal_board.c and that one links instead.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_if.h"
#include "board_pins.h"
#include "hardware/gpio.h"

#ifndef BOARD_PIN_BUZZER
#define BOARD_PIN_BUZZER BOARD_BUZZER_NO_PIN
#endif

#define BOARD_DEFAULT __attribute__((weak))

BOARD_DEFAULT void board_hw_init(void) {
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1); /* lit from the start: the pin is the LED */

    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);
}

BOARD_DEFAULT void board_led_set(bool on) {
    gpio_put(BOARD_PIN_LED, on);
}

BOARD_DEFAULT void board_led_toggle(void) {
    gpio_xor_mask(1u << BOARD_PIN_LED);
}

static uint8_t buzzer_pad = BOARD_PIN_BUZZER;

static bool buzzer_fitted(void) {
    return buzzer_pad != BOARD_BUZZER_NO_PIN;
}

static void release_pad(uint8_t pad) {
    gpio_put(pad, 0);
    gpio_set_dir(pad, GPIO_IN);
}

BOARD_DEFAULT void board_buzzer_init(void) {
    if (!buzzer_fitted())
        return;
    gpio_init(buzzer_pad);
    gpio_set_dir(buzzer_pad, GPIO_OUT);
    gpio_put(buzzer_pad, 0);
}

BOARD_DEFAULT void board_buzzer_set_pin(uint8_t pin) {
    if (pin == buzzer_pad)
        return;
    if (buzzer_fitted())
        release_pad(buzzer_pad);
    buzzer_pad = pin;
    board_buzzer_init();
}

BOARD_DEFAULT uint8_t board_buzzer_pin(void) {
    return buzzer_pad;
}

BOARD_DEFAULT void board_buzzer_on(void) {
    if (buzzer_fitted())
        gpio_put(buzzer_pad, 1);
}

BOARD_DEFAULT void board_buzzer_off(void) {
    if (buzzer_fitted())
        gpio_put(buzzer_pad, 0);
}

BOARD_DEFAULT bool board_pyro_raw(board_pyro_raw_t *out) {
    (void)out;
    return false; /* no bus-level sensing: /api/status leaves the fields out */
}

BOARD_DEFAULT uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

BOARD_DEFAULT uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}

BOARD_DEFAULT void board_pyro_limits(pyro_limits_t *out) {
    *out = (pyro_limits_t)PYRO_LIMITS_GENERAL;
}
