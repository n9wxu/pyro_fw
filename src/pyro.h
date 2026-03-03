#ifndef PYRO_H
#define PYRO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t raw_adc;
    bool     good;       /* continuity detected (not open, not short) */
    bool     open;       /* no connection */
    bool     shorted;    /* dead short */
} pyro_continuity_t;

void pyro_init(void);
void pyro_check_continuity(pyro_continuity_t *p1, pyro_continuity_t *p2);
void pyro_fire(uint8_t channel);     /* 1 or 2 */
void pyro_update(uint32_t now_ms);   /* call from main loop, manages fire duration */
bool pyro_is_firing(void);

#endif
