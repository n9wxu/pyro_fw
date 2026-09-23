/* Host stand-in for hardware/uart.h.
 *
 * src/board_if.h includes this for the uart_inst_t in board_uart(). The
 * sim board's telemetry goes out through hal_telemetry_send(), so nothing
 * here is ever dereferenced. */
#ifndef _HARDWARE_UART_H
#define _HARDWARE_UART_H
#include "pico/types.h"
typedef struct uart_inst uart_inst_t;
extern uart_inst_t *const uart0;
extern uart_inst_t *const uart1;
#define UART0_IRQ 20
#define UART1_IRQ 21
#endif
