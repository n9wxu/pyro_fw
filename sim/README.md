# Pyro Simulation Library

The simulation library packages the Pyro flight software **and** a rocket physics engine as a single WASM module that any web project can import. The flight code runs identically to hardware — same state machine, same pyro logic, same telemetry — but with an in-memory HAL (`boards/sim/hal_sim.c`) instead of real sensors.

## The simulator builds

| Build | What it runs |
|-------|--------------|
| `scripts/build_wasm.sh` | the WASM module below: `boards/sim`'s HAL with a pyro *fixture*, `sim/physics.c` and the Lua VM (`docs/sim.html`, `docs/lua.html`) |
| `sim` target, `PYRO_BOARD=sim` | `pyro_sim`, the same flight software natively, flown by `sim/sim_cli.c` on `sim/physics.c` |
| `sim` target, `PYRO_BOARD=sim_mk1a`, `sim_mk1b`, `sim_mk1c` | `pyro_sim` with the real `boards/<board>/` pyro backend against `sim/plant/` (see `sim/plant/README.md`) |
| `pyro_sim --replay <flight_log.csv>` | a logged flight's readings back through the firmware (`sim/replay.c`) |
| `sim/qemu/` | the real ARM image on an emulated RP2040 (see `sim/qemu/README.md`) |

`sim/mach_plant.c` is the flight tests' plant, not a simulator build.

With the fixture, continuity is whatever `sim.setContinuity()` last set and a
fire is a counter; with a modelled board, a fire counts only when the match
takes its ignition energy.

## Architecture

```
┌──────────────────────────────────────────────────┐
│  Your Web Project                                │
│                                                  │
│  import { createPyroSim } from 'pyro-sim.js'     │
│  const sim = await createPyroSim()               │
│                                                  │
│  ┌─────────────┐        ┌──────────────────┐     │
│  │ sim.physics  │───────►│  sim (flight fw) │     │
│  │ .init(1524)  │  pa    │  .tick(t)        │     │
│  │ .step(t)     │───────►│  .state          │     │
│  │ .altM        │        │  .pyro1Fired     │     │
│  │ .pressurePa  │◄───────│  .pyroFireCount  │     │
│  └─────────────┘ deploy  └──────────────────┘     │
└──────────────────────────────────────────────────┘
                    │
              ┌─────┴─────┐
              │ pyro.wasm  │  ← single WASM binary
              │            │
              │ physics.c  │  rocket physics engine
              │ hal_sim.c  │  in-memory HAL
              │ flight_    │  real flight state machine
              │ states.c   │  (same as hardware)
              │ Lua VM     │  user programs
              └────────────┘
```

## Quick Start (Web / WASM)

### 1. Build the WASM module

```bash
# Requires Emscripten SDK (https://emscripten.org)
./scripts/build_wasm.sh
# Output: docs/wasm/pyro.js + docs/wasm/pyro.wasm
```

The first run clones Lua 5.4.6 into `build-wasm-lua/`. The script also takes
`PYRO_BOARD=sim_mk1a`, `sim_mk1b` or `sim_mk1c`, and takes the board's sources
from its `board.cmake`, as the native `sim` target does.

### 2. Use from any web project

Copy `docs/wasm/pyro.js`, `docs/wasm/pyro.wasm`, and `docs/wasm/pyro-sim.js` into your project:

```html
<script type="module">
import { createPyroSim } from './wasm/pyro-sim.js';

const sim = await createPyroSim();
const phys = sim.physics;
const LANDED = 8;  // flight_state_t; see Flight States below

// Configure and initialize
sim.init("pyro1_mode=delay\npyro1_value=0\npyro2_mode=agl\npyro2_value=200\nunits=ft\n");
sim.setContinuity(1, 50, true, false);
sim.setContinuity(2, 50, true, false);
phys.init(1524);  // 5000 ft target apogee

// Closed-loop simulation
const PAD_DWELL_MS = 9000;
let prevFires = 0;

for (let t = 0; t <= 600000; t++) {
    // Physics → flight computer feedback
    if (sim.pyroFireCount > prevFires) {
        if (sim.lastFireChannel === 1) phys.deployDrogue();
        if (sim.lastFireChannel === 2) phys.deployMain();
        prevFires = sim.pyroFireCount;
    }

    // Step physics (after pad dwell)
    if (t >= PAD_DWELL_MS) {
        phys.step((t - PAD_DWELL_MS) / 1000);
    }

    // Feed physics pressure to flight computer
    sim.setPressure(phys.pressurePa);
    sim.tick(t);

    // Read outputs
    if (t % 1000 === 0) {
        console.log(`t=${t}ms state=${sim.state} alt=${phys.altM.toFixed(0)}m`);
    }

    if (sim.state === LANDED) break;
}

console.log(`Apogee: ${phys.apogeeM.toFixed(0)}m`);
</script>
```

PAD_IDLE comes about 2.7 s after power-on, and the ground reference is a 5 s
rolling mean (GND-CAL-01), so the example ignites at 9 s, as `sim/sim_cli.c`
does. Ignite before PAD_IDLE and the board calibrates on the way up, reading
every altitude low. The HAL feeds the pressure layer one reading per 20 ms of
simulated time, the hardware's loop (DD-065), so tick at least that often.

### 3. Use with script tags (no modules)

`pyro-sim.js` is an ES module and cannot be loaded as a classic script. Without
modules, call the exports on `Module` directly, as `docs/sim.html` does:

```html
<script src="wasm/pyro.js"></script>
<script>
Module.onRuntimeInitialized = function() {
    Module._sim_flight_init(0);        // defaults; pass a malloc'd string for a config
    Module._sim_set_pressure(101325);
    const state = Module._sim_flight_tick(0);
};
</script>
```

## API Reference

### Flight Computer (`sim.*`)

#### Lifecycle
| Method | Description |
|--------|-------------|
| `sim.init(configIni)` | Initialize with optional config string |
| `sim.tick(timeMs)` | Advance to `timeMs`, returns state (0-11) |
| `sim.reset()` | Reset to power-on defaults |

#### Inputs (set before each tick)
| Method | Description |
|--------|-------------|
| `sim.setPressure(pa)` | Barometric pressure in Pascals |
| `sim.setSensorType(type)` | 0 none, 1 MS5607, 2 BMP280 (the default); 0 boots to FAULT |
| `sim.setContinuity(ch, adc, good, open)` | Pyro circuit status |
| `sim.clearPyroFiring()` | Clear firing flag; `tick()` clears it itself |

#### Outputs (read after each tick)
| Property | Description |
|----------|-------------|
| `sim.state` | Flight state (0-11) |
| `sim.stateName` | A name from `pyro-sim.js`'s own list, which does not follow `flight_state_t`: use the table below |
| `sim.altitudeCm` | Filtered altitude (cm) |
| `sim.maxAltCm` | Peak altitude (cm) |
| `sim.vspeedCms` | Vertical speed (cm/s) |
| `sim.pressure` | Filtered pressure (Pa) |
| `sim.pyro1Fired` | Pyro 1 has fired |
| `sim.pyro2Fired` | Pyro 2 has fired |
| `sim.armed` | Pyros armed |
| `sim.pyroFireCount` | Total fires |
| `sim.lastFireChannel` | Last fired channel |
| `sim.buzzerActive` | Buzzer on/off |
| `sim.samples` | Samples in the flight buffer |
| `sim.launchTime` | T+0 (ms) |

### Physics Engine (`sim.physics.*`)

#### Control
| Method | Description |
|--------|-------------|
| `phys.init(targetAltM)` | Configure for target apogee (meters) |
| `phys.reset()` | Reset state |
| `phys.step(flightTimeSec)` | Advance 1ms of physics |
| `phys.deployDrogue()` | Deploy drogue chute |
| `phys.deployMain()` | Deploy main chute |

#### State
| Property | Description |
|----------|-------------|
| `phys.altM` | Current altitude (meters) |
| `phys.velMs` | Velocity (m/s, positive=up) |
| `phys.pressurePa` | Barometric pressure (Pa) |
| `phys.apogeeM` | Peak altitude (meters) |
| `phys.onGround` | On the ground |
| `phys.drogueDeployed` | Drogue deployed |
| `phys.mainDeployed` | Main chute deployed |

#### Utility
| Method | Description |
|--------|-------------|
| `phys.altToPressure(altM)` | Altitude → pressure conversion |
| `phys.snapshot()` | All state as a plain object |

## Flight States

`flight_state_t` in `src/flight_states.h`. States are appended, never
renumbered: the numbers reach the flight log, telemetry and `/api/status`.

| Value | Name | Description |
|-------|------|-------------|
| 0 | BOOT_SETTLE | Waiting for sensors to stabilize |
| 9 | BOOT_SENSOR | The pressure sensor answers, or FAULT |
| 1 | BOOT_CONTINUITY | Checking pyro circuits |
| 2 | BOOT_CALIBRATE | Establishing ground pressure |
| 3 | PAD_IDLE | On pad, waiting for launch |
| 4 | ASCENT | Rocket ascending |
| 5 | FALLING | Past apogee, no canopy steadying the descent |
| 6 | DROGUE_DESCENT | Descending steadily at a drogue's rate |
| 7 | CHUTE_DESCENT | Descending steadily at a main's rate |
| 8 | LANDED | On ground after flight |
| 10 | FAULT | A power-up test failed; terminal |
| 11 | GROUND_TEST | Powered up with the ground test switch held; terminal |

## Files

| File | Purpose |
|------|---------|
| `sim/physics.h` | Physics engine C API |
| `sim/physics.c` | The rocket: U.S. Standard Atmosphere 1976, constant thrust, linear chute damping |
| `sim/pyro_sim.h` | Flight computer simulation C API |
| `sim/main_sim.c` | High-level sim lifecycle functions |
| `sim/sim_cli.c` | `pyro_sim`'s driver, on `sim/physics.c`, and `--replay` |
| `sim/replay.c` | A flight log's readings back through the firmware |
| `sim/hw/` | The Pico SDK calls a board file makes, for the modelled boards |
| `sim/plant/` | The board plant models |
| `sim/mach_plant.c` | The flight tests' atmosphere, rocket and static ports |
| `boards/sim/hal_sim.h` | Internal sim HAL accessors |
| `boards/sim/hal_sim.c` | In-memory HAL implementation |
| `boards/sim_mk1a`, `sim_mk1b`, `sim_mk1c` | Host boards that build a real pyro backend against its plant |
| `docs/wasm/pyro-sim.js` | ES module wrapper for WASM |
| `docs/wasm/pyro.js` | Emscripten glue (generated) |
| `docs/wasm/pyro.wasm` | WASM binary (generated) |
| `scripts/build_wasm.sh` | Build script |

## Examples

- **Interactive browser sim**: `docs/sim.html` — UI driving WASM flight computer, with `docs/physics.js`, a JS copy of `sim/physics.c`
- **Lua in the browser**: `docs/lua.html` — a user program against the simulated platform
- **CLI simulator**: `sim/sim_cli.c` — `sim/physics.c` + flight computer
- **Flights against their truth**: `test/flight_run.c` flies the flight software beside a rocket (`sim/mach_plant.c`) for the host suites: `test/test_flight_profiles.c`, `test/test_fire_rules.c`, `test/test_mach.c`

## Building for C projects

The native simulator is the `sim` target of a host board:

```bash
cmake -B build-sim -DPYRO_BOARD=sim && cmake --build build-sim --target sim
./build-sim/pyro_sim [apogee_m]                   # default 1524
./build-sim/pyro_sim --replay flight_log.csv      # a flight logged with log_rate=full
```

It writes `flight_sim.csv` into the working directory. The physics engine and
flight sim also work as plain C libraries:

```bash
cc -I boards/sim -I sim/ -I src/ \
   sim/physics.c sim/main_sim.c boards/sim/hal_sim.c \
   $(cat src/flight_sources.txt) \
   src/pad_claim.c src/pyro_release.c src/buzzer.c \
   my_app.c -lm -o my_app
```

`src/flight_sources.txt` is the flight software's file list: the firmware, the
host suites, `scripts/build_wasm.sh` and the `sim` target all read it.
