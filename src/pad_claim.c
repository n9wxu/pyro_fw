/*
 * The pad ownership table. See pad_claim.h.
 *
 * Deliberately free of every other header: it is linked into the firmware,
 * the simulator and the host tests, and it decides something all three have
 * to agree about.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pad_claim.h"

static uint8_t owner[PAD_CLAIM_MAX_GPIO];

#define PAD_ALL ((PAD_CLAIM_MAX_GPIO >= 32) ? 0xffffffffu : ((1u << PAD_CLAIM_MAX_GPIO) - 1u))

void pad_claim_reset(void) {
    for (int i = 0; i < PAD_CLAIM_MAX_GPIO; i++) {
        owner[i] = PAD_FREE;
    }
}

bool pad_claim_take(uint32_t pads, pad_owner_t who) {
    if (who == PAD_FREE || (pads & ~PAD_ALL)) {
        return false;
    }

    /* Checked whole before anything is written. A partial claim would leave
     * pads owned by someone who believes the claim failed, which is worse
     * than the conflict it was meant to prevent. */
    for (int i = 0; i < PAD_CLAIM_MAX_GPIO; i++) {
        if ((pads & PAD(i)) && owner[i] != PAD_FREE && owner[i] != (uint8_t)who) {
            return false;
        }
    }
    for (int i = 0; i < PAD_CLAIM_MAX_GPIO; i++) {
        if (pads & PAD(i)) {
            owner[i] = (uint8_t)who;
        }
    }
    return true;
}

bool pad_claim_held_by(uint32_t pads, pad_owner_t who) {
    if (pads & ~PAD_ALL) {
        return false;
    }
    for (int i = 0; i < PAD_CLAIM_MAX_GPIO; i++) {
        if ((pads & PAD(i)) && owner[i] != (uint8_t)who) {
            return false;
        }
    }
    return true;
}

pad_owner_t pad_claim_owner(uint8_t pin) {
    return (pin < PAD_CLAIM_MAX_GPIO) ? (pad_owner_t)owner[pin] : PAD_FREE;
}

int pad_claim_count(pad_owner_t who) {
    int n = 0;
    for (int i = 0; i < PAD_CLAIM_MAX_GPIO; i++) {
        if (owner[i] == (uint8_t)who) {
            n++;
        }
    }
    return n;
}
