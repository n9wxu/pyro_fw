/*
 * What the flight software reaches when a pyro channel belongs to Lua.
 *
 * A released channel's pad is not a firing path any more -- it is a Lua
 * output, and core0 driving it would fight core1 for the same SIO register.
 * So every flight-side operation on that channel has to do nothing.
 *
 * NOTHING HERE IS A CHECK
 *
 * The operations are a table installed once at boot, per channel. Firing a
 * released channel is not refused; it reaches a function that does not touch
 * the hardware, because the real one was never installed for it. Same
 * argument as lua_iface.h and as the half-bridge PIO program: a check can be
 * forgotten at a new call site, a pointer that does not go there cannot.
 *
 * NOTHING HERE IS SILENT
 *
 * A mocked operation is reported, counted, and -- in flight -- written to the
 * log next to the event that commanded it. A flight log saying PYRO1 with no
 * note beside it would be a record of something that did not happen.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_RELEASE_H
#define PYRO_RELEASE_H

#include "hal.h"
#include <stdbool.h>
#include <stdint.h>

/* The flight-side operations on one channel. Deliberately the three that
 * touch a channel individually; the shared ones (sample, update) are not here
 * because they cannot be answered per channel -- see pyro_release_all(). */
typedef struct {
    void (*fire)(uint8_t channel);
    void (*get)(uint8_t channel, hal_continuity_t *out);
    bool (*fault)(uint8_t channel);
} pyro_ch_ops_t;

/* Where a mocked operation is reported. The hardware HAL sends it to the
 * flight log and the debug console; the host tests capture it. */
typedef void (*pyro_mock_report_fn)(uint8_t channel, const char *what);

/* Install the real operations. Must have static storage. Call before
 * pyro_release_apply(). */
void pyro_release_init(const pyro_ch_ops_t *real, pyro_mock_report_fn report);

/* Claim each channel's pads and install accordingly: the real methods for a
 * channel the flight software owns, the mocked ones for a channel it does
 * not. Call at boot once the pads have owners and before core1 exists.
 *
 * pads_of(channel) is how this asks which pads a channel switches -- the
 * board's capability table answers, through pin_store_pyro_pads(), so nothing
 * here knows a pin number. Returns how many channels got the real methods. */
typedef uint32_t (*pyro_pads_fn)(uint8_t channel);
int pyro_release_claim(pyro_pads_fn pads_of);

/* What the flight software reaches. Never NULL once initialised; an
 * out-of-range channel gets the mocked table rather than a null pointer,
 * because a caller that passes 3 has a bug and should not fire anything. */
const pyro_ch_ops_t *pyro_ch(uint8_t channel);

bool pyro_release_is_released(uint8_t channel);

/* True when no channel is left for the shared stimulus to measure.
 *
 * The continuity stimulus and the background sense cycle drive the COMMON
 * element, and the common is Lua's exactly when both channels are released.
 * That is the one thing a per-channel table cannot express, so the two shared
 * entry points ask this instead. */
bool pyro_release_all(void);

/* How many flight-side operations have been mocked away. Reported on
 * /api/status so a board that quietly fires nothing says why. */
uint32_t pyro_release_mocks(void);

#endif
