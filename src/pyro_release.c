/*
 * Installing the mocked pyro operations. See pyro_release.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro_release.h"
#include "pad_claim.h"

static const pyro_ch_ops_t *real_ops;
static pyro_mock_report_fn reporter;
static const pyro_ch_ops_t *ch_ops[2];
static bool released[2];
static uint32_t mocks;

static void report(uint8_t channel, const char *what) {
    mocks++;
    if (reporter) {
        reporter(channel, what);
    }
}

/* ── The mocked channel ───────────────────────────────────────────
 *
 * Reported on every call rather than once. A flight log has to show each
 * command that went nowhere, not that one of them did at some point. */

static void mock_fire(uint8_t channel) {
    report(channel, "fire");
}

static void mock_get(uint8_t channel, hal_continuity_t *out) {
    if (!out) {
        return;
    }
    /* Open rather than good. There is no igniter circuit the flight software
     * controls here any more, and reporting continuity would let it arm and
     * then "fire" a channel that cannot. Open is what is true. */
    out->raw_adc = 0;
    out->good = false;
    out->open = true;
    out->shorted = false;
    report(channel, "continuity");
}

static bool mock_fault(uint8_t channel) {
    (void)channel;
    /* Not reported: this is polled, and a fault line that reads clear is the
     * honest answer for a channel with no fire in progress. Reporting it
     * would bury the fire attempts that matter. */
    return false;
}

static const pyro_ch_ops_t mock_ops = {mock_fire, mock_get, mock_fault};

void pyro_release_init(const pyro_ch_ops_t *real, pyro_mock_report_fn rep) {
    real_ops = real;
    reporter = rep;
    ch_ops[0] = ch_ops[1] = real;
    released[0] = released[1] = false;
    mocks = 0;
}

int pyro_release_claim(pyro_pads_fn pads_of) {
    int kept = 0;
    for (uint8_t ch = 1; ch <= 2; ch++) {
        uint32_t pads = pads_of ? pads_of(ch) : PAD_NONE;

        /* Spending the claim is the install. A channel whose pads Lua already
         * holds cannot take them, so it cannot be given the real methods --
         * there is no branch here that could be written the other way. */
        bool mine = (pads != PAD_NONE) && pad_claim_take(pads, PAD_FLIGHT);
        ch_ops[ch - 1] = mine ? real_ops : &mock_ops;
        released[ch - 1] = !mine;
        kept += mine ? 1 : 0;
    }
    return kept;
}

const pyro_ch_ops_t *pyro_ch(uint8_t channel) {
    if (channel == 1 || channel == 2) {
        const pyro_ch_ops_t *o = ch_ops[channel - 1];
        if (o) {
            return o;
        }
    }
    return &mock_ops;
}

bool pyro_release_is_released(uint8_t channel) {
    return (channel == 1 || channel == 2) && released[channel - 1];
}

bool pyro_release_all(void) {
    return released[0] && released[1];
}

uint32_t pyro_release_mocks(void) {
    return mocks;
}
