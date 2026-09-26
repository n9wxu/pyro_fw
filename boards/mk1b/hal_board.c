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
#include "pico/binary_info.h"
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

/* The buzzer pad, runtime rather than compile-time: an operator may move it.
 * LS1 is fitted, so that is where it starts. */
static uint8_t buzz_pin = BOARD_PIN_BUZZER;

void board_buzzer_set_pin(uint8_t pin) {
    if (pin == buzz_pin) {
        return;
    }
    /* Hand the old pad back as an input first. Leaving it an output would
     * keep it driven after a reassignment, and on a released pyro pad that is
     * a driven pin nobody believes is driven. */
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 0);
        gpio_set_dir(buzz_pin, GPIO_IN);
    }
    buzz_pin = pin;
    board_buzzer_init();
}

void board_buzzer_init(void) {
    if (buzz_pin == BOARD_BUZZER_NO_PIN) {
        return;
    }
    gpio_init(buzz_pin);
    gpio_set_dir(buzz_pin, GPIO_OUT);
    gpio_put(buzz_pin, 0);
}

void board_buzzer_on(void) {
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 1);
    }
}

void board_buzzer_off(void) {
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 0);
    }
}

/* MK1B senses continuity per channel only; no bus or pack rail. */
bool board_pyro_raw(board_pyro_raw_t *out) {
    (void)out;
    return false;
}


uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}

/* ── Identification for picotool ──────────────────────────────────
 *
 * See boards/mk1a/hal_board.c for why these are here. Note this board leaves
 * PICO_BOARD at the SDK default, so binary info reports pico_board=pico --
 * the program name and these pins are what identify it. */
bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (Pico onboard)"));
bi_decl(bi_1pin_with_name(BOARD_PIN_BUZZER, "buzzer"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX (TRRS)", BOARD_PIN_UART_RX, "UART0 RX (TRRS)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_I2C_SCL, "I2C1 SCL", BOARD_PIN_BMP280_SDA, "I2C1 SDA (BMP280)",
                            BOARD_PIN_MS5607_SDA, "I2C1 SDA (MS5607)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_PYRO_COMMON_EN, "PYRO shared enable (high side)", BOARD_PIN_PYRO1_EN,
                            "PYRO1 enable", BOARD_PIN_PYRO2_EN, "PYRO2 enable"));
bi_decl(bi_4pins_with_names(BOARD_PIN_PYRO1_FLAG, "PYRO1 FLAG (AP2192)", BOARD_PIN_PYRO2_FLAG, "PYRO2 FLAG (AP2192)",
                            BOARD_PIN_PYRO1_SENSE, "PYRO sense 1 (ADC0)", BOARD_PIN_PYRO2_SENSE,
                            "PYRO sense 2 (ADC1)"));
