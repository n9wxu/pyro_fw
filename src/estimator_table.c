/*
 * The estimators this build carries. See estimator.h. A build with another
 * set links another table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimator.h"
#include <string.h>

extern const estimator_vt estimator_constacc_vt;
extern const estimator_vt estimator_lumped_vt;

static const estimator_vt *const estimators_vt[] = {
    &estimator_lumped_vt,
    &estimator_constacc_vt,
};
#define COUNT (uint8_t)(sizeof(estimators_vt) / sizeof(estimators_vt[0]))
_Static_assert(sizeof(estimators_vt) / sizeof(estimators_vt[0]) <= ESTIMATORS_MAX, "raise ESTIMATORS_MAX");

uint8_t estimator_count(void) {
    return COUNT;
}

const estimator_vt *estimator_at(uint8_t index) {
    return estimators_vt[index < COUNT ? index : 0];
}

uint8_t estimator_index(const char *name) {
    for (uint8_t i = 0; i < COUNT; i++)
        if (name && strcmp(name, estimators_vt[i]->name) == 0)
            return i;
    return 0;
}
