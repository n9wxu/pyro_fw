/*
 * What hal_common.c reports about the pressure sensor beyond hal.h:
 * the collector's counters.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HAL_PRESSURE_STATS_H
#define HAL_PRESSURE_STATS_H

#include <stdint.h>

#include "pressure_collector.h"

uint32_t hal_pressure_interval_min_us(void);
uint32_t hal_pressure_interval_max_us(void);
uint32_t hal_pressure_stamp_lag_max_us(void);
void hal_pressure_collector(collector_stats_t *out);
uint32_t hal_pressure_rate_hz(void);
uint32_t hal_pressure_flashed(void);

#endif
