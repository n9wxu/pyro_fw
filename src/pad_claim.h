/*
 * Who owns a pad. One owner, decided once, and the only way to get a vtable.
 *
 * THE PROBLEM THIS SOLVES
 *
 * The flight software's pyro operations and Lua's resources are both tables
 * of function pointers, and until this existed they were independent. Nothing
 * structural stopped a pad from appearing in both -- only pin_assign_validate()
 * and the order of two calls at boot, which is to say a check and a
 * convention. Get either wrong and two cores drive one FET gate.
 *
 * HOW IT IS PREVENTED RATHER THAN CHECKED
 *
 * A pad has exactly one owner because there is one array and one write per
 * pad: pin_store_claim_pads() walks the assignment once and assigns, so there
 * is no code path that can write two owners for one pad. It does not compare
 * anything.
 *
 * After that, a claim is the only currency either table accepts.
 * lua_iface_publish() takes the pads its resource drives and refuses unless
 * Lua owns every one. pyro_claim_channels() installs the real pyro methods
 * for a channel only if pyro owns that channel's pads, and the mocked ones
 * otherwise. So a pad in the Lua table cannot also be in the pyro table: the
 * second table's entry cannot be created, because the claim it would need was
 * already spent.
 *
 * Pads are a bitmask, not a list: a resource driving several pads is one
 * claim that either succeeds whole or fails whole, which is what stops a
 * half-claimed bridge from existing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PAD_CLAIM_H
#define PAD_CLAIM_H

#include <stdbool.h>
#include <stdint.h>

#define PAD_CLAIM_MAX_GPIO 30 /* RP2040 has GPIO0..29 */

/* A pad mask. PAD(n) is GPIO n. */
#define PAD(n) (1u << (n))
#define PAD_NONE 0u

typedef enum {
    PAD_FREE = 0,
    /* The flight software's: a pyro element, the buzzer, the telemetry UART,
     * the sensor bus. Not split finer, because the exclusivity that matters
     * is against PAD_LUA -- two cores driving one pad -- and everything on
     * this side is the same core. */
    PAD_FLIGHT,
    PAD_LUA, /* a script's, reachable only through the interface table */
} pad_owner_t;

/* Drop every claim. Boot only, before anything claims. */
void pad_claim_reset(void);

/* Claim every pad in the mask for one owner.
 *
 * All or nothing: a mask that overlaps another owner's pads changes nothing
 * and returns false. Re-claiming what you already own succeeds, so a board
 * that publishes two resources on one pad is refused by the duplicate-name
 * rule rather than by this.
 *
 * Returns false for a mask containing a pad above PAD_CLAIM_MAX_GPIO. */
bool pad_claim_take(uint32_t pads, pad_owner_t owner);

/* True when this owner holds every pad in the mask. An empty mask is held by
 * everyone -- a resource that drives no pad cannot conflict over one. */
bool pad_claim_held_by(uint32_t pads, pad_owner_t owner);

pad_owner_t pad_claim_owner(uint8_t pin);

/* How many pads this owner holds. Reported so a board that ends up driving
 * nothing says why. */
int pad_claim_count(pad_owner_t owner);

#endif
