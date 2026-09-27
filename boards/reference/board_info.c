/*
 * What `picotool info -a` says about an attached board. Name the pins that
 * differ between boards -- above all the pyro pins -- so an image for the
 * wrong board is caught before it is loaded. See THEORY_OF_OPERATION.md
 * "Files".
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"
#include "pico/binary_info.h"

bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART TX", BOARD_PIN_UART_RX, "UART RX"));
/* TODO: bi_decl() each pyro pin, as boards/mk1c/board_info.c does. */
