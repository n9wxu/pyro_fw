/*
 * Who owns a pad: one owner, decided once, and the claim is the only way to
 * a vtable (pyro_release_claim(), lua_iface_publish()). See DD-020.
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
    /* Core0's, whatever drives it: the exclusivity that matters is against
     * the other core. */
    PAD_FLIGHT,
    PAD_LUA, /* a script's, reachable only through the interface table */
} pad_owner_t;

/* Drop every claim. Boot only, before anything claims. */
void pad_claim_reset(void);

/* All or nothing: a mask that overlaps another owner's pads, or holds a pad
 * past PAD_CLAIM_MAX_GPIO, changes nothing and returns false. Re-claiming
 * your own pads succeeds; two resources on one pad are refused by the
 * duplicate-name rule instead. */
bool pad_claim_take(uint32_t pads, pad_owner_t owner);

/* True when this owner holds every pad in the mask. An empty mask is held by
 * everyone -- a resource that drives no pad cannot conflict over one. */
bool pad_claim_held_by(uint32_t pads, pad_owner_t owner);

pad_owner_t pad_claim_owner(uint8_t pin);

/* Reported, so a board that ends up driving nothing says why. */
int pad_claim_count(pad_owner_t owner);

#endif
