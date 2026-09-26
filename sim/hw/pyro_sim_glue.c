/*
 * Glue between the simulator's HAL and a real board's pyro backend.
 *
 * boards/sim/hal_sim.c implements all of hal.h except the pyro half, which
 * it leaves out when PYRO_SIM_BOARD_PYRO is defined. This file supplies
 * that half by calling the actual board file -- boards/mk1a, mk1b or mk1c's
 * pyro_board.c -- which is running against sim/hw/ and sim/plant/.
 *
 * So the call chain in a sim_mk1c build is
 *
 *     flight_states.c  ->  hal_pyro_fire()      [here]
 *                      ->  pyro_fire()          [boards/mk1c/pyro_board.c]
 *                      ->  gpio_put()           [sim/hw/rp2040_shim.c]
 *                      ->  plant_set_gpio()     [sim/plant/plant_mk1c.c]
 *
 * and back up through adc_read(). Nothing in boards/ or src/ is aware of
 * any of it.
 *
 * ── Two clocks ───────────────────────────────────────────────────
 *
 * The flight software has a millisecond clock that the simulation driver
 * sets, and the shim has a microsecond clock that board code advances by
 * blocking. They are reconciled once per update, in the only direction
 * that is safe: forward. Board code that blocks has moved the shim past
 * the tick, and that is a real result, not an error to correct.
 *
 * ── What counts as a fire ────────────────────────────────────────
 *
 * The simulation's fire count follows the PLANT's ignition latch, not the
 * firmware's command. Firing into an open channel increments nothing, so a
 * closed-loop driver that deploys a chute on sim.pyroFireCount will not
 * deploy one for a match that never lit. That is the distinction the whole
 * model exists to make, and it is free here.
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
    /* The plant must exist before the board file's first gpio_put(), which
     * happens inside pyro_init()'s safing pass. */
    shim_reset();
    plant_init(SIM_PLANT_BOARD);
    seen_fire_events = plant_fire_events();
    pyro_init();
}

/* The board model has no pin assignment to release from, so both channels
 * are always the flight software's. Present because hal.h asks for it:
 * a HAL that silently omits an entry point is one that links until
 * something calls it. */
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

    /* Report ignitions the plant has latched since the last update. A
     * single update can span more than one only if both channels lit
     * inside it, which the firing rules are supposed to prevent -- so the
     * loop is also a way for a test to catch that they did not. */
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
