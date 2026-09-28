# Simulator boards — Theory of Operation

The flight software, with no board under it. Four packages share this file:

| Board | Pyro side | Question it answers |
|---|---|---|
| `sim` | a fixture | what does the flight logic decide, and when? |
| `sim_mk1a`, `sim_mk1b`, `sim_mk1c` | the real board file, against a model of that board | what does the board do when the flight logic decides? |

All four are `host` boards (`PYRO_BOARD_KIND host`): no Pico SDK, no
`src/hal_common/`. They build natively, and to WebAssembly for the browser
simulator in `docs/sim.html`.

## Overview

A pico board implements `src/board_if.h` and lets `src/hal_common/` implement
`src/hal.h` on top of it. A host board has no SDK to build that on, so it
implements `src/hal.h` itself, in `hal_sim.c`. That file is the degenerate case
of the board contract: a complete HAL in one file, linking no common HAL.

The flight software above it — `src/flight_states.c`, the pressure chain, the
buzzer, telemetry, config — is the same source the boards run.

## The sim board

`hal_sim.c` replaces each piece of hardware with something a driver can set or
read:

| Hardware | In the simulator |
|---|---|
| clock | `sim_set_time()`; the driver steps it |
| pressure sensor | `sim_set_pressure()`, from the physics engine, fed every 20 ms; reported as a BMP280 |
| pyro channels | continuity is whatever `sim_set_continuity()` last wrote; a fire is a counter |
| buzzer | a flag the WASM page turns into sound |
| telemetry UART | an 8 KB buffer (`sim_get_telemetry()`) |
| filesystem | four files of 64 KB in memory |
| reset cause | always a software reset: there is no power event, so no brownout recovery |
| ground test switch | none: `hal_ground_test_asserted()` is `flight_states.c`'s weak default, always false, so ground test mode never starts |

`sim/main_sim.c` and `sim/sim_cli.c` drive it from a trajectory — a physics
model or a replayed log (`sim/replay.c`) — and `sim_note_pyro_fire()` feeds each
fire back into the physics, so a chute opens when the flight software fires it.

Lua runs in the browser build: `lua_platform_sim.c` implements
`src/lua/lua_platform.h` on simulated pins and a simulated serial port,
mirroring MK1C's resource set so a script written in the browser runs
unchanged on the board. `lua_tests` builds the sandbox against it too.

## The simulated MK1 boards

`sim_mk1a`, `sim_mk1b` and `sim_mk1c` define `PYRO_SIM_BOARD_PYRO`. `hal_sim.c`
then leaves the pyro half of `src/hal.h` out, and `sim/hw/pyro_sim_glue.c`
supplies it by calling the **real** `boards/<name>/pyro_board.c`, compiled
against `sim/hw/` — a stand-in for the fifty-odd Pico SDK functions the board
files use — which drives the plant model in `sim/plant/`:

```
flight_states.c -> hal_pyro_fire()   sim/hw/pyro_sim_glue.c
                -> pyro_fire()       boards/mk1c/pyro_board.c
                -> gpio_put()        sim/hw/rp2040_shim.c
                -> plant_set_gpio()  sim/plant/plant_mk1c.c
```

and back up through `adc_read()`. Each package's `board_pins.h` includes the
real board's pin map and overrides only the identity, so there is one copy of
the pin numbers.

**Two clocks.** The flight software's millisecond clock is the driver's; the
shim's microsecond clock is advanced by anything that costs time — an ADC
conversion is 2 µs. They are reconciled once an update, forward only.

**A fire is the plant's.** With a modelled board the fire count follows the
plant's ignition latch, not the command: a fire into an open channel counts
nothing, so a chute deploys only for a match that lit.

`sim_mk1b` releases nothing to Lua, which it does not run, so it supplies a
`pin_store_owns()` that answers true for every pad (`pin_store_sim.c`).

What the plant models, and what it does not, is in `sim/plant/README.md`.

## Running

```bash
cmake -B build-sim -DPYRO_BOARD=sim && cmake --build build-sim --target sim
./build-sim/pyro_sim 300                  # a flight sized for a 300 m apogee
./build-sim/pyro_sim --replay flight.csv  # a logged flight, replayed

cmake -B build-sim-mk1c -DPYRO_BOARD=sim_mk1c && cmake --build build-sim-mk1c --target sim

./scripts/build_wasm.sh                   # sim, to docs/wasm/, for docs/sim.html
PYRO_BOARD=sim_mk1c ./scripts/build_wasm.sh
```

## Tests

The host test suites build their own HAL (`test/hal_test.c`) rather than this
one, so each can drive the flight software sample by sample. The simulator
boards are for running flights; `board_pyro_mk1c_tests` and `board_pyro_tests`
are where the board files are tested against the same stand-ins.
