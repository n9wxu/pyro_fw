/* Host stand-in for hardware/structs/watchdog.h.
 *
 * MK1C clears WATCHDOG_CTRL_ENABLE_BITS directly because the SDK has no
 * watchdog_disable(). The shim honours that write. */
#ifndef _HARDWARE_STRUCTS_WATCHDOG_H
#define _HARDWARE_STRUCTS_WATCHDOG_H
#include <stdint.h>
typedef struct { volatile uint32_t ctrl; volatile uint32_t load; } watchdog_hw_t;
extern watchdog_hw_t *const watchdog_hw;
#define WATCHDOG_CTRL_ENABLE_BITS 0x40000000u
void hw_clear_bits(volatile uint32_t *addr, uint32_t bits);
void hw_set_bits(volatile uint32_t *addr, uint32_t bits);
#endif
