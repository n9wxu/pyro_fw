/*
 * Board support — Pyro MK1B (Raspberry Pi Pico module).
 *
 * Implements src/board_if.h. Everything board-independent lives in
 * src/hal_common/.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_if.h"
#include "board_pins.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

void board_early_init(void) {
    /* Silence the buzzer before any slow init (USB, networking) can leave
     * the pin floating and produce a spurious tone at power-on. */
    board_buzzer_init();
}

void board_hw_init(void) {
    /* Heartbeat LED. Start ON so a lit LED confirms the correct GPIO. */
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1);

    /* Telemetry UART pins; hal_common calls uart_init() on the instance. */
    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);

    /* Pyro continuity sense. adc_init() is done by pyro_init(). */
    adc_gpio_init(BOARD_PIN_PYRO1_SENSE);
    adc_gpio_init(BOARD_PIN_PYRO2_SENSE);
}

void board_led_set(bool on) {
    gpio_put(BOARD_PIN_LED, on);
}

void board_led_toggle(void) {
    gpio_xor_mask(1u << BOARD_PIN_LED);
}

void board_buzzer_init(void) {
    gpio_init(BOARD_PIN_BUZZER);
    gpio_set_dir(BOARD_PIN_BUZZER, GPIO_OUT);
    gpio_put(BOARD_PIN_BUZZER, 0);
}

void board_buzzer_on(void) {
    gpio_put(BOARD_PIN_BUZZER, 1);
}

void board_buzzer_off(void) {
    gpio_put(BOARD_PIN_BUZZER, 0);
}

/* MK1B senses continuity per channel only; no bus or pack rail. */
bool board_pyro_raw(board_pyro_raw_t *out) {
    (void)out;
    return false;
}

bool board_pyro_wave_request(int mode) {
    (void)mode;
    return false;
}

int board_pyro_wave_state(void) {
    return 0;
}

uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}
