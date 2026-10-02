/*
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
 * Reported on every call: the log has to show each command that went
 * nowhere. */

static void mock_fire(uint8_t channel) {
    report(channel, "fire");
}

static void mock_get(uint8_t channel, hal_continuity_t *out) {
    if (!out) {
        return;
    }
    /* Open, not good: see DD-019. */
    out->raw_adc = 0;
    out->good = false;
    out->open = true;
    out->shorted = false;
    report(channel, "continuity");
}

static bool mock_fault(uint8_t channel) {
    (void)channel;
    /* Not reported: it is polled, and the reports would bury the fires. */
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

        /* Spending the claim is the install [DD-020]. */
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
