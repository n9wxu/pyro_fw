/*
 * Which pyro channels the flight software owns, and which belong to Lua.
 *
 * A released channel's pad is not a firing path any more -- it is a Lua
 * output, and core0 driving it would fight core1 for the same SIO register.
 *
 * NOTHING IN THE FLIGHT PATH ASKS
 *
 * Ownership is settled once, at boot, by the pad claim. The fire plan takes
 * it from here (fire_plan_from()): a released channel is not enabled, so the
 * fire rules never command it, and the HAL's fire is a direct call with no
 * choice in it [FLT-RT-07]. The pad's health read leaves a released channel
 * alone and calls it open, which is what is true: there is no igniter
 * circuit the flight software controls there.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_RELEASE_H
#define PYRO_RELEASE_H

#include <stdbool.h>
#include <stdint.h>

/* Both channels the flight software's, until the claim says otherwise. */
void pyro_release_init(void);

/* Claim each channel's pads. A channel whose pads Lua already holds cannot
 * take them, and is released. Call at boot once the pads have owners and
 * before core1 exists.
 *
 * pads_of(channel) is how this asks which pads a channel switches -- the
 * board's capability table answers, through pin_store_pyro_pads(), so nothing
 * here knows a pin number. Returns how many channels the flight software
 * owns. */
typedef uint32_t (*pyro_pads_fn)(uint8_t channel);
int pyro_release_claim(pyro_pads_fn pads_of);

/* An out-of-range channel is released: a caller that passes 3 has a bug and
 * should not fire anything. */
bool pyro_release_is_released(uint8_t channel);

/* True when no channel is left for the shared stimulus to measure.
 *
 * The continuity stimulus and the background sense cycle drive the COMMON
 * element, and the common is Lua's exactly when both channels are released. */
bool pyro_release_all(void);

#endif
