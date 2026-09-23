# Board plant models

`sim/physics.c` models the rocket. This models the **board**: the firing
bus, the sense dividers, the bias injectors, the switches and the e-match.

It exists so the flight software can be tested against something that
answers like hardware. `boards/sim` runs the flight software with a pyro
*fixture* — continuity is whatever a test last wrote with
`sim_set_continuity()`, and a fire is a counter. That is the right thing
for flight-logic questions and the wrong thing for board questions, because
the 810 lines of `boards/mk1c/pyro_board.c` never execute.

The plant closes that gap by running the **real board file**:

```
sim/physics.c ──pressure──► flight_states.c
                                  │ hal_pyro_fire()
                                  ▼
                       sim/hw/pyro_sim_glue.c
                                  │ pyro_fire()
                                  ▼
                 boards/mk1c/pyro_board.c        ← the actual file
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
any of it, and none of those files were changed to make it work.

## Using it

```sh
# the flight software against a modelled MK1C board
cmake -B build-sim-mk1c -DPYRO_BOARD=sim_mk1c
cmake --build build-sim-mk1c --target sim        # ./pyro_sim
cmake --build build-sim-mk1c --target plant_tests

# same, in the browser
PYRO_BOARD=sim_mk1c ./scripts/build_wasm.sh      # docs/wasm/pyro_sim_mk1c.js
```

`sim_mk1a` and `sim_mk1b` work the same way. `-DPYRO_BOARD=sim` still
selects the fixture and is unchanged.

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
| stimulus | assert the low side | assert the low side | 330R bias injection |
| pack current to sense | none | none | none (0.2 mA of bias) |
| fault output | none | AP2192 FLAG ×2 | none (~FLT not routed) |
| modelled extras | — | FLAG assertion | charge pump, dVdT ramp, ILIM, latch-off |

Sources: MK1A and MK1C from the block comments in their own `pyro_board.c`
and from `~/Documents/pyro_mk1c/DESIGN.md`; MK1B from a netlist export of
`~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch`, because nothing in this
repository describes its sense network.

The e-match is the same device on all three boards and carries the M1–M13
properties from DESIGN.md §1.2, including both ignition criteria — an
energy criterion for a fast pulse and a sustained-current criterion, since
variant A fires by the second and never by the first.

## What is not modelled

- **Temperature.** Every value is at room temperature. The S9 trip point
  and the MOSFET SOA both move with it.
- **Non-linear devices.** The bias Schottkys are a fixed 0.3 V drop, the
  TVS is either absent or a short, and a FET is either 30 mΩ or open.
- **The RP2040 itself.** `sim/hw/` is about thirty functions, not an
  emulator. There is no interrupt model and no core 1.
- **MK1C firing.** The plant supports it, but `boards/mk1c/pyro_board.c`
  is a sense-only build whose `pyro_fire()` refuses, so nothing exercises
  it yet. That is the firmware's state, not a gap in the model.

## Validating it

`test/test_plant.c` splits into assertions and reports, deliberately.

**Assertions** are levels and time constants that the board files and
DESIGN.md state independently of the model — the four `_Static_assert`
anchors in `pyro_board.c`, the DESIGN.md §4 table, the counts quoted in
MK1A's own comments, the 0.89 V/ms slew. If one fails, the model is wrong,
or a component value moved under it.

**Reports** print rather than assert. They say what the model thinks of the
*firmware's* thresholds and settle times, and which injected faults move a
reading. Freezing those into assertions would make a finding invisible the
moment someone fixed it, so they stay as output to read.
