/*
 * The ground test switch (ground_test_switch.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "ground_test_switch.h"
#include <string.h>

void gt_switch_begin(gt_switch_t *w, bool pair) {
    memset(w, 0, sizeof(*w));
    w->pair = pair;
}

gt_switch_out_t gt_switch_step(gt_switch_t *w, bool read_high) {
    if (!w->pair) {
        w->asserted = !read_high;
        return (gt_switch_out_t){false, true};
    }
    bool followed = read_high == w->drive_high;
    w->follows = (uint8_t)(((w->follows << 1) | (followed ? 1u : 0u)) & 3u);
    w->asserted = w->follows == 3u;
    w->drive_high = !w->drive_high;
    return (gt_switch_out_t){w->drive_high, !w->drive_high};
}

bool gt_switch_asserted(const gt_switch_t *w) {
    return w->asserted;
}
