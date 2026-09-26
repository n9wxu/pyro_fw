/*
 * Board support — Pyro MK1C (bare RP2040).
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

extern void pyro_safe_all_outputs(void); /* boards/mk1c/pyro_board.c */

void board_early_init(void) {
    /* Silence the buzzer before any slow init (USB, networking) can leave
     * the pin floating and produce a spurious tone at power-on. */
    board_buzzer_init();

    /* Drive every pyro output inactive at the earliest opportunity. The
     * RP2040 pad reset state (input with pull-down) already achieves this
     * before any code runs, so this asserts the safe state rather than
     * establishing it. */
    pyro_safe_all_outputs();
}

void board_hw_init(void) {
    /* Heartbeat LED (D1). Start ON so a lit LED confirms the correct GPIO.
     * Must run after the TinyUSB BSP's board_init(). */
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1);

    /* Telemetry UART pins; hal_common calls uart_init() on the instance. */
    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);

    /* The four sense channels are claimed by pyro_init(), which also calls
     * adc_init(). Nothing to do here. */
}

void board_led_set(bool on) {
    gpio_put(BOARD_PIN_LED, on);
}

void board_led_toggle(void) {
    gpio_xor_mask(1u << BOARD_PIN_LED);
}

/* The buzzer pad, runtime rather than compile-time: an operator may move it.
 * A buzzer is fitted, so that is where it starts. */
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

bool board_pyro_raw(board_pyro_raw_t *out) {
    extern bool pyro_raw_sense(board_pyro_raw_t * out);
    return pyro_raw_sense(out);
}


uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}

/* ── Identification for picotool ──────────────────────────────────
 *
 * See boards/mk1a/hal_board.c for why these are here. GPIO25 is the reason
 * they matter most on this board: it is BIAS_B, a pyro bias injector, where
 * MK1A and MK1B use the same pin as the heartbeat LED. */
bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (D1 blue)"));
bi_decl(bi_1pin_with_name(BOARD_PIN_BUZZER, "buzzer (Q2 gate)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX -> J1.4", BOARD_PIN_UART_RX, "UART0 RX <- J1.5"));
bi_decl(bi_2pins_with_names(BOARD_PIN_I2C_SDA, "I2C1 SDA (MS5607)", BOARD_PIN_I2C_SCL, "I2C1 SCL (MS5607)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_ARM_TOGGLE, "PYRO ARM_TOGGLE (U9 charge pump)", BOARD_PIN_FIRE_A,
                            "PYRO FIRE_A (low side)", BOARD_PIN_FIRE_B, "PYRO FIRE_B (low side)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_BIAS_A, "PYRO BIAS_A", BOARD_PIN_BIAS_BUS, "PYRO BIAS_BUS", BOARD_PIN_BIAS_B,
                            "PYRO BIAS_B (NOT an LED)"));
bi_decl(bi_4pins_with_names(BOARD_PIN_SNS_VBAT, "sense VBAT (ADC0)", BOARD_PIN_SNS_BUS, "sense BUS (ADC1)",
                            BOARD_PIN_SNS_A, "sense ch A (ADC2)", BOARD_PIN_SNS_B, "sense ch B (ADC3)"));
