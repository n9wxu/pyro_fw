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
/* 5, 6 and 8 are in logs written before DD-092 and are no longer written. */
#define EVT_MACH_LOCK 5
#define EVT_MACH_UNLOCK 6
#define EVT_LANDING 7
#define EVT_MACH_FALLBACK 8
#define EVT_SENSOR_STUCK 10 /* a whole window of one reading [SNS-PRES-10] */
#define EVT_ARMED 9
#define EVT_PYRO1_FAULT 11
#define EVT_PYRO2_FAULT 12
#define EVT_PYRO1_NOPEN 13 /* post-fire verify: the channel did not open */
#define EVT_PYRO2_NOPEN 14
/* 15 to 17 are in logs written before DD-082 and are no longer written. */
#define EVT_PYRO1_REFUSED 15
#define EVT_PYRO2_REFUSED 16
#define EVT_MAIN_FORCED 17
#define EVT_SENSOR_LOST 18  /* no sample for 0.5 s in flight [SNS-PRES-11] */
#define EVT_PYRO1_REFIRE 19 /* [PYR-REFIRE-01] */
#define EVT_PYRO2_REFIRE 20
#define EVT_EMERGENCY_FIRE 21 /* [FLT-EMRG-04] */
#define EVT_RESUMED 22        /* [FLT-BROWN-07] */
/* The row's altitude is the flight's peak [FLT-APO-08]. */
#define EVT_PEAK 23
#define EVT_PEAK_AT_LEAST 24

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
    case EVT_PYRO1_REFIRE:
        return "PYRO1_REFIRE";
    case EVT_PYRO2_REFIRE:
        return "PYRO2_REFIRE";
    case EVT_EMERGENCY_FIRE:
        return "EMERGENCY_FIRE";
    case EVT_RESUMED:
        return "RESUMED";
    case EVT_PEAK:
        return "PEAK";
    case EVT_PEAK_AT_LEAST:
        return "PEAK_AT_LEAST";
    default:
        return "";
    }
}

#endif
