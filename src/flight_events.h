/*
 * Flight events: the codes the flight software tags samples with, and the
 * names flight_log.csv carries for them.
 *
 * Header-only so every HAL's log writer, the ring-buffer export and the host
 * tests spell an event the same way without linking the flight machine. The
 * web UI and any downstream tool find events by these names.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_EVENTS_H
#define FLIGHT_EVENTS_H

#include <stdint.h>

#define EVT_NONE 0
#define EVT_LAUNCH 1
#define EVT_APOGEE 2
#define EVT_PYRO1_FIRE 3
#define EVT_PYRO2_FIRE 4
#define EVT_MACH_LOCK 5     /* the Mach lockout's flag [FLT-MACH-02] */
#define EVT_MACH_UNLOCK 6   /* released on a coast's signature [FLT-MACH-03] */
#define EVT_LANDING 7
#define EVT_MACH_FALLBACK 8 /* the lock never released; apogee from the fallback [FLT-MACH-04] */
#define EVT_SENSOR_STUCK 10 /* a whole window of one reading [SNS-PRES-10] */
#define EVT_ARMED 9
#define EVT_PYRO1_FAULT 11
#define EVT_PYRO2_FAULT 12
#define EVT_PYRO1_NOPEN 13 /* post-fire verify: the channel did not open */
#define EVT_PYRO2_NOPEN 14
#define EVT_PYRO1_REFUSED 15 /* commanded, and the board energised nothing */
#define EVT_PYRO2_REFUSED 16
#define EVT_MAIN_FORCED 17 /* the emergency ladder overrode pyro2's trigger */
#define EVT_SENSOR_LOST 18 /* no sample for 0.5 s in flight [SNS-PRES-11] */

static inline const char *flight_event_name(uint8_t evt) {
    switch (evt) {
    case EVT_LAUNCH:
        return "LAUNCH";
    case EVT_APOGEE:
        return "APOGEE";
    case EVT_PYRO1_FIRE:
        return "PYRO1";
    case EVT_PYRO2_FIRE:
        return "PYRO2";
    case EVT_LANDING:
        return "LANDING";
    case EVT_MACH_LOCK:
        return "LOCK";
    case EVT_MACH_UNLOCK:
        return "UNLOCK";
    case EVT_MACH_FALLBACK:
        return "LOCK_FALLBACK";
    case EVT_ARMED:
        return "ARMED";
    case EVT_PYRO1_FAULT:
        return "PYRO1_FAULT";
    case EVT_PYRO2_FAULT:
        return "PYRO2_FAULT";
    case EVT_PYRO1_NOPEN:
        return "PYRO1_NOPEN";
    case EVT_PYRO2_NOPEN:
        return "PYRO2_NOPEN";
    case EVT_PYRO1_REFUSED:
        return "PYRO1_REFUSED";
    case EVT_PYRO2_REFUSED:
        return "PYRO2_REFUSED";
    case EVT_MAIN_FORCED:
        return "MAIN_FORCED";
    case EVT_SENSOR_STUCK:
        return "SENSOR_STUCK";
    case EVT_SENSOR_LOST:
        return "SENSOR_LOST";
    default:
        return "";
    }
}

#endif
