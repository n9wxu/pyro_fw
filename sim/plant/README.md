# Board plant models

`sim/physics.c` and `sim/sim_cli.c` model the rocket. This models the **board**: the firing
bus, the sense dividers, the bias injectors, the switches and the e-match.

It exists so the flight software can be tested against something that
answers like hardware. `boards/sim` runs the flight software with a pyro
*fixture* — continuity is whatever a test last wrote with
`sim_set_continuity()`, and a fire is a counter. That is the right thing
for flight-logic questions and the wrong thing for board questions, because
the board's own pyro backend never executes.

The plant closes that gap by running the **real board file**:

```
sim/sim_cli.c ──pressure──► flight_states.c
                                  │ hal_pyro_fire()
                                  ▼
                       sim/hw/pyro_sim_glue.c
                                  │ pyro_fire()
                                  ▼
                 boards/mk1c/pyro_board.c        ← the actual files:
                 pyro_measure.c, pyro_faults.c,     MK1C's backend
                 pyro_sequence.c, arm_pump.c
                                  │ gpio_put() / adc_read()
                                  ▼
                  sim/hw/rp2040_shim.c           ← Pico SDK stand-in
                                  │ plant_set_gpio() / plant_adc_counts()
                                  ▼
                 sim/plant/plant_mk1c.c          ← the electrical network
                                  │ the match takes its ignition energy
                                  └──────────► physics deploys a chute
```

Nothing in `boards/mk1a`, `boards/mk1b`, `boards/mk1c` or `src/` is aware of
any of it. The plant reads MK1C's bench fit from `boards/mk1c/pyro_sense.h`,
the constants the firmware reads (DD-054).

## Using it

```sh
# the flight software against a modelled MK1C board
cmake -B build-sim-mk1c -DPYRO_BOARD=sim_mk1c
cmake --build build-sim-mk1c --target sim        # ./pyro_sim
cmake --build build-sim-mk1c --target plant_tests
cmake --build build-sim-mk1c --target board_pyro_mk1c_tests
```

`sim_mk1a` and `sim_mk1b` work the same way. `-DPYRO_BOARD=sim` selects the
fixture. `plant_tests` and `board_pyro_mk1c_tests` build in any configured
tree, since each compiles its own board's files.

`scripts/build_wasm.sh` takes `PYRO_BOARD=sim_mk1a|sim_mk1b|sim_mk1c`, but
those builds do not compile: it puts `boards/sim` ahead of the board's own
`board_pins.h`, and leaves out MK1B's `pin_store_sim.c` and MK1C's
`pyro_measure.c`, `pyro_faults.c`, `pyro_sequence.c` and `arm_pump.c`.

## What is modelled

Every board is a resistive network with node capacitance, solved by
backward Euler in `net_solve.c`. Solving rather than tabulating is what
makes the *time constants* real: MK1A's 10.1 ms open-channel rise and
MK1C's bias-release decay fall out of the network, so firmware that samples
too early reads a partly settled node here exactly as it would on the
bench. That is how `report_settle_margins()` in `test/test_plant.c` finds
what it finds.

| | MK1A | MK1B | MK1C |
|---|---|---|---|
| high side | per channel (Q6/Q1) | per channel (AP2192) | shared eFuse (TPS259570) |
| low side | one shared (Q2) | one shared (AO6800 Q1B) | per channel (Q103/Q104) |
| sense | 100k pull-up, 1k + 100nF | 100k pull-up, 100R + 100nF | 4 divided taps, 0.333 |
| stimulus | assert the low side | assert the low side | GPIO through BAT54WS + 330R |
| pack current to sense | none | none | none (0.2 mA of bias) |
| fault output | none | AP2192 FLAG ×2 | none (~FLT not routed) |
| modelled extras | — | FLAG assertion | charge pump, dVdT ramp, ILIM, latch-off, U9's reverse path |

Sources: MK1A from `boards/mk1a/THEORY_OF_OPERATION.md`; MK1C from the MK1C
design record (`pyro_mk1c/DESIGN.md`) and the bench MK1C, whose bias diodes
and U9 reverse path are fitted in `boards/mk1c/pyro_sense.h` (DD-054); MK1B
from a netlist export of its schematic (`pyro_mk1b.kicad_sch`), as its
`THEORY_OF_OPERATION.md` is. The design record and the schematics are on the
author's machine and not in this repository.

The e-match is the same device on all three boards and carries the M1–M13
properties from the design record's §1.2, including both ignition criteria — an
energy criterion for a fast pulse and a sustained-current criterion for a
slow drive, since neither implies the other.

## What is not modelled

- **Temperature.** Every value is at room temperature. The S9 trip point
  and the MOSFET SOA both move with it.
- **Most non-linear devices.** MK1C's bias Schottkys and U9's reverse path
  are junctions, linearised about each step's node voltage; the rest are
  not. The pump's diodes are a fixed 0.3 V drop, the TVS is either absent or
  a short, and a FET is either 30 mΩ or open.
- **MK1B's AP2192A.** The part fitted discharges its outputs while disabled,
  which holds both sense nodes near 0 V (DD-059, task B-U5). The model is the
  board as fitted, so a simulated MK1B reads every channel shorted and never
  fires; `plant_set_mk1b_u5(MK1B_U5_AP2192)` models the netlist's part.
- **The RP2040 itself.** `sim/hw/` is about sixty functions — GPIO, the ADC
  and its FIFO, DMA, the PIO state machine MK1C's pump runs on, the watchdog —
  not an emulator. There is no interrupt model and no core 1.
- **U9's latch-off clears only at a plant reset.** The part clears it when
  its enable is cycled (DESIGN.md 5.0), which the next arm does; the model
  keeps it, so a test that trips it cannot fire again.

## Validating it

`test/test_plant.c` splits into assertions and reports, deliberately.

**Assertions** are levels and time constants that the board files, the
bench and DESIGN.md state independently of the model — the three
`_Static_assert` anchors in `boards/mk1c/pyro_sense.h`, the DESIGN.md §4
table, the bench MK1C's levels and bus decay, the counts MK1A's theory of
operation quotes, the 0.89 V/ms slew. If one fails, the model is wrong, or a
component value moved under it.

**Reports** print rather than assert. They say what the model thinks of the
*firmware's* thresholds and settle times, and which injected faults move a
reading. Freezing those into assertions would make a finding invisible the
moment someone fixed it, so they stay as output to read.
