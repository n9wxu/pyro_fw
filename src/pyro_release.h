/*
 * What the flight software reaches when a pyro channel belongs to Lua: a
 * table installed once at boot, per channel, whose mocked half touches no
 * hardware and reports every call. See DD-019 and DD-020.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_RELEASE_H
#define PYRO_RELEASE_H

#include "hal.h"
#include <stdbool.h>
#include <stdint.h>

/* The per-channel operations. The shared ones (sample, update) drive the
 * common and cannot be answered per channel: see pyro_release_all(). */
typedef struct {
    void (*fire)(uint8_t channel);
    void (*get)(uint8_t channel, hal_continuity_t *out);
    bool (*fault)(uint8_t channel);
} pyro_ch_ops_t;

/* Where a mocked operation is reported. The hardware HAL sends it to the
 * flight log and the debug console; the host tests capture it. */
typedef void (*pyro_mock_report_fn)(uint8_t channel, const char *what);

/* real must have static storage. Call before pyro_release_claim(). */
void pyro_release_init(const pyro_ch_ops_t *real, pyro_mock_report_fn report);

/* The real operations for a channel whose pads the flight software could
 * claim, the mocked ones otherwise. Call at boot once the pads have owners
 * and before core1 exists. pads_of answers which pads a channel switches, so
 * nothing here knows a pin number. Returns how many channels got the real
 * operations. */
typedef uint32_t (*pyro_pads_fn)(uint8_t channel);
int pyro_release_claim(pyro_pads_fn pads_of);

/* Never NULL; an out-of-range channel gets the mocked table. */
const pyro_ch_ops_t *pyro_ch(uint8_t channel);

bool pyro_release_is_released(uint8_t channel);

/* Both channels released: the common is then Lua's, and the shared entry
 * points must not drive it. */
bool pyro_release_all(void);

/* For /api/status: a board that quietly fires nothing says why. */
uint32_t pyro_release_mocks(void);

#endif
