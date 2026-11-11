/*
 * Who owns each pyro channel. See pyro_release.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro_release.h"
#include "pad_claim.h"

static bool released[2];

void pyro_release_init(void) {
    released[0] = released[1] = false;
}

int pyro_release_claim(pyro_pads_fn pads_of) {
    int kept = 0;
    for (uint8_t ch = 1; ch <= 2; ch++) {
        uint32_t pads = pads_of ? pads_of(ch) : PAD_NONE;
        bool mine = (pads != PAD_NONE) && pad_claim_take(pads, PAD_FLIGHT);
        released[ch - 1] = !mine;
        kept += mine ? 1 : 0;
    }
    return kept;
}

bool pyro_release_is_released(uint8_t channel) {
    return channel < 1 || channel > 2 || released[channel - 1];
}

bool pyro_release_all(void) {
    return released[0] && released[1];
}
