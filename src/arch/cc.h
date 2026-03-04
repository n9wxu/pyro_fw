#ifndef ARCH_CC_H
#define ARCH_CC_H

#define LWIP_PROVIDE_ERRNO 1

typedef int sys_prot_t;

#include <stdio.h>
#include "hardware/uart.h"
void lwip_uart_printf(const char *fmt, ...);
#define LWIP_PLATFORM_DIAG(x) lwip_uart_printf x

#endif
