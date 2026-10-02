/*
 * hal.h's pyro half for a modelled-board build (PYRO_SIM_BOARD_PYRO): the
 * real boards/<board>/pyro_board.c, against sim/hw/ and sim/plant/.
 * boards/sim/hal_sim.c supplies the rest of hal.h.
 *
 * The shim's microsecond clock is brought up to the flight software's
 * millisecond clock each update, never back: board code that blocked has
 * already moved it past the tick. A fire counts when the plant's match
 * lights, not when the firmware commands it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "pyro.h"
#include "plant.h"
#include "rp2040_shim.h"

/* boards/sim/hal_sim.c */
void sim_note_pyro_fire(uint8_t channel);

#if defined(PYRO_SIM_PLANT_MK1A)
#define SIM_PLANT_BOARD PLANT_MK1A
#elif defined(PYRO_SIM_PLANT_MK1B)
#define SIM_PLANT_BOARD PLANT_MK1B
#elif defined(PYRO_SIM_PLANT_MK1C)
#define SIM_PLANT_BOARD PLANT_MK1C
#else
#error "define PYRO_SIM_PLANT_MK1A, _MK1B or _MK1C for a modelled-board build"
#endif

static int seen_fire_events;

void hal_pyro_init(void) {
    /* Before pyro_init()'s safing pass writes its first pad. */
    shim_reset();
    plant_init(SIM_PLANT_BOARD);
    seen_fire_events = plant_fire_events();
    pyro_init();
}

/* No pin assignment to release from: both channels are the flight's. */
int hal_pyro_claim_channels(uint32_t (*pads_of)(uint8_t channel)) {
    (void)pads_of;
    return 2; /* both channels */
}

void hal_pyro_sample(void) {
    pyro_sample();
}

void hal_pyro_get(uint8_t channel, hal_continuity_t *out) {
    pyro_continuity_t c = {0, false, false, false};
    pyro_get(channel, &c);
    out->raw_adc = c.raw_adc;
    out->good = c.good;
    out->open = c.open;
    out->shorted = c.shorted;
}

void hal_pyro_fire(uint8_t channel) {
    pyro_fire(channel);
}

void hal_pyro_update(uint32_t now_ms) {
    shim_advance_to_ms(now_ms);
    pyro_update(now_ms);

    /* Every ignition since the last update, two if both channels lit in
     * one, which the firing rules forbid. */
    int ev = plant_fire_events();
    while (seen_fire_events < ev) {
        seen_fire_events++;
        sim_note_pyro_fire((uint8_t)plant_last_fire_channel());
    }
}

bool hal_pyro_is_firing(void) {
    return pyro_is_firing();
}

bool hal_pyro_fault(uint8_t channel) {
    return pyro_fault(channel);
}
