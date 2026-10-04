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
/* True from pyro_fire() until the channel is de-energised. Read straight
 * after pyro_fire() it is what the board observed of the pulse, which the
 * flight records [PYR-FIRE-01]: false means the pulse energised nothing. No
 * board withholds a fire on a reading [PYR-HEALTH-01]. */
bool pyro_is_firing(void);
bool pyro_fault(uint8_t channel); /* a fault the board can sense, for the record */

#endif
