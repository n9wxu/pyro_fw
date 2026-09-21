#ifndef PYRO_H
#define PYRO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t raw_adc;
    bool good;    /* continuity detected (not open, not short) */
    bool open;    /* no connection */
    bool shorted; /* dead short */
} pyro_continuity_t;

void pyro_init(void);
/* One shared-stimulus measurement, then per-channel reads. See the note on
 * hal_pyro_sample()/hal_pyro_get() in hal.h for why these are separate. */
void pyro_sample(void);
void pyro_get(uint8_t channel, pyro_continuity_t *out);
void pyro_fire(uint8_t channel);   /* 1 or 2 */
void pyro_update(uint32_t now_ms); /* call from main loop, manages fire duration */
bool pyro_is_firing(void);
bool pyro_fault(uint8_t channel); /* AP2192 FLAG pin: true = overcurrent fault */

#endif
