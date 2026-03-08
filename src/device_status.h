#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  state;          // flight_state_t enum
    int32_t  altitude_cm;
    int32_t  max_altitude_cm;
    int32_t  vertical_speed_cms;
    int32_t  pressure_pa;
    bool     pyro1_continuity;
    bool     pyro2_continuity;
    uint16_t pyro1_adc;
    uint16_t pyro2_adc;
    bool     pyro1_fired;
    bool     pyro2_fired;
    bool     pyros_armed;
    bool     under_thrust;
    uint32_t flight_time_ms;
    uint8_t  pyro1_mode;
    uint16_t pyro1_value;
    uint8_t  pyro2_mode;
    uint16_t pyro2_value;
    uint8_t  units;
} device_status_t;

extern volatile device_status_t g_status;

#endif
