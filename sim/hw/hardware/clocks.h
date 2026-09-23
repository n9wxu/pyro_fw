/* Host stand-in for hardware/clocks.h. */
#ifndef _HARDWARE_CLOCKS_H
#define _HARDWARE_CLOCKS_H
#include "pico/types.h"
enum clock_index { clk_sys = 5, clk_peri = 6 };
static inline uint32_t clock_get_hz(enum clock_index i) { (void)i; return 125000000u; }
#endif
