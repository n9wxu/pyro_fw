/*
 * Simulation HAL accessors — shared between hal_sim.c and main_sim.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef HAL_SIM_H
#define HAL_SIM_H

#include <stdint.h>
#include <stdbool.h>

void    sim_reset(void);
void    sim_set_time(uint32_t ms);
void    sim_set_pressure(float pa);
void    sim_set_sensor_type(int type);
void    sim_set_continuity(int ch, uint16_t adc, bool good, bool open);
void    sim_clear_pyro_firing(void);
int     sim_get_pyro_fire_count(void);
uint8_t sim_get_pyro_last_channel(void);
bool    sim_get_buzzer_state(void);
const char *sim_get_telemetry(void);
int     sim_get_telemetry_len(void);
void    sim_clear_telemetry(void);

#endif
