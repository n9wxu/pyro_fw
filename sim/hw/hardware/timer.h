/* Host stand-in for hardware/timer.h: the four hardware alarms, fired by
 * shim_advance_us() as its clock passes each target. See rp2040_shim.h. */
#ifndef _HARDWARE_TIMER_H
#define _HARDWARE_TIMER_H
#include "pico/types.h"

typedef void (*hardware_alarm_callback_t)(uint alarm_num);

int hardware_alarm_claim_unused(bool required);
void hardware_alarm_set_callback(uint alarm_num, hardware_alarm_callback_t callback);
bool hardware_alarm_set_target(uint alarm_num, absolute_time_t t); /* true: the target was missed */
void hardware_alarm_cancel(uint alarm_num);

#endif
