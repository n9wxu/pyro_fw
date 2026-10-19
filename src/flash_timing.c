/*
 * The boot flash's datasheet figures. See flash_timing.h.
 *
 * Every board's part, from docs/datasheets/:
 *   W25Q16JV  (MK1B)      W25Q16JV_RevI_2024-12-24.pdf: 75h and 7Ah p. 39-40;
 *                         tSUS, tPP, tSE p. 63; tSUS also bounds resume to suspend
 *   W25Q128JV (MK1A)      W25Q128JV_RevH_2021-03-10.pdf: p. 42-43; p. 65
 *   XT25F128F (MK1C)      XT25F128F-W_Rev0.6_2022-02-26.pdf: p. 43; tSUS, tRS,
 *                         tPP p. 55; tSE p. 56, revised to 800 ms
 *   BY25Q64ES (MK1B-8MB)  BY25Q64ES_Rev2.9_2024-10-29.pdf: erase suspend only,
 *                         and a program allowed while suspended, p. 62; tESL,
 *                         tERS, tPP, tSE p. 71-72, the slower of its tables.
 *                         Its erase, resumed and suspended in each slot on
 *                         MK1B-8MB, had not ended after 2,000 slots: by
 *                         measurement it does not suspend [FLT-LOG-10].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_timing.h"

#include <stddef.h>

typedef struct {
    uint8_t jedec[3];
    flash_timing_t t;
} part_t;

static const part_t parts[] = {
    {{0xEF, 0x40, 0x15}, {"W25Q16JV", 3000u, 400000u, 20u, 20u, true, true}},
    {{0xEF, 0x40, 0x18}, {"W25Q128JV", 3000u, 400000u, 20u, 20u, true, true}},
    {{0xEF, 0x70, 0x18}, {"W25Q128JV", 3000u, 400000u, 20u, 20u, true, true}},
    {{0x0B, 0x40, 0x18}, {"XT25F128F", 2000u, 800000u, 20u, 500u, true, true}},
    {{0x68, 0x40, 0x17}, {"BY25Q64ES", 2400u, 400000u, 35u, 20u, false, false}},
};

const flash_timing_t *flash_timing_for(const uint8_t jedec[3]) {
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (parts[i].jedec[0] == jedec[0] && parts[i].jedec[1] == jedec[1] && parts[i].jedec[2] == jedec[2])
            return &parts[i].t;
    return NULL;
}
