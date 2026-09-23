/* Host stand-in for hardware/watchdog.h.
 *
 * The shim records a missed deadline but does not reboot: there is no
 * processor to reset, and a test that cares whether the MK1C arm window
 * would have survived asks shim_watchdog_expired(). */
#ifndef _HARDWARE_WATCHDOG_H
#define _HARDWARE_WATCHDOG_H
#include "pico/types.h"
void watchdog_enable(uint32_t delay_ms, bool pause_on_debug);
void watchdog_update(void);
bool watchdog_caused_reboot(void);
#endif
