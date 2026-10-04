#ifndef PYRO_H
#define PYRO_H

#include <stdint.h>
#include <stdbool.h>

/* All three false: no verdict yet (see pyro_get()). */
typedef struct {
    uint16_t raw_adc;
    bool good;    /* continuity detected (not open, not short) */
    bool open;    /* no connection */
    bool shorted; /* dead short */
} pyro_continuity_t;

void pyro_init(void);
/* Empty on every board: each checks in the background from pyro_update(). */
void pyro_sample(void);
/* The newest completed check. A fired channel has no verdict until a check
 * begun after its pulse completes [PYR-VERIFY-01]. channel is 1 or 2; any
 * other value leaves *out unmodified. */
void pyro_get(uint8_t channel, pyro_continuity_t *out);
/* [PYR-DEPLOY-02] Energises nothing for a channel other than 1 or 2, or while
 * a pulse is in progress. */
void pyro_fire(uint8_t channel);
void pyro_update(uint32_t now_ms); /* every loop: the check, the pulse's end */
/* True from pyro_fire() until the channel is de-energised. Read straight
 * after pyro_fire() it is what the board observed of the pulse, which the
 * flight records [PYR-FIRE-01]: false means the pulse energised nothing. No
 * board withholds a fire on a reading [PYR-HEALTH-01]. */
bool pyro_is_firing(void);
bool pyro_fault(uint8_t channel); /* a fault the board can sense, for the record */

#endif
