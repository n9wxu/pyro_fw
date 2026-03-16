# Pyro MK1B Simulation Library

The simulation library packages the Pyro MK1B flight computer **and** a rocket physics engine as a single WASM module that any web project can import. The flight code runs identically to hardware — same state machine, same pyro logic, same telemetry — but with an in-memory HAL instead of real sensors.

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
              └────────────┘
```

## Quick Start (Web / WASM)

### 1. Build the WASM module

```bash
# Requires Emscripten SDK (https://emscripten.org)
./scripts/build_wasm.sh
# Output: docs/wasm/pyro.js + docs/wasm/pyro.wasm
```

### 2. Use from any web project

Copy `docs/wasm/pyro.js`, `docs/wasm/pyro.wasm`, and `docs/wasm/pyro-sim.js` into your project:

```html
<script type="module">
import { createPyroSim, FlightState } from './wasm/pyro-sim.js';

const sim = await createPyroSim();
const phys = sim.physics;

// Configure and initialize
sim.init("pyro1_mode=delay\npyro1_value=0\npyro2_mode=agl\npyro2_value=200\nunits=ft\n");
sim.setContinuity(1, 50, true, false);
sim.setContinuity(2, 50, true, false);
phys.init(1524);  // 5000 ft target apogee

// Closed-loop simulation
const PAD_DWELL_MS = 2000;
let prevFires = 0;

for (let t = 0; t <= 120000; t++) {
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
    sim.clearPyroFiring();
    sim.tick(t);

    // Read outputs
    if (t % 1000 === 0) {
        console.log(`t=${t}ms state=${sim.stateName} alt=${phys.altM.toFixed(0)}m`);
    }

    if (sim.state === FlightState.LANDED) break;
}

console.log(`Apogee: ${phys.apogeeM.toFixed(0)}m`);
</script>
```

### 3. Use with script tags (no modules)

```html
<script src="wasm/pyro.js"></script>
<script src="wasm/pyro-sim.js"></script>
<script>
Module.onRuntimeInitialized = function() {
    const sim = wrapModule(Module);  // from pyro-sim.js
    // ... same API as above
};
</script>
```

## API Reference

### Flight Computer (`sim.*`)

#### Lifecycle
| Method | Description |
|--------|-------------|
| `sim.init(configIni)` | Initialize with optional config string |
| `sim.tick(timeMs)` | Advance one tick, returns state (0-7) |
| `sim.reset()` | Reset to power-on defaults |

#### Inputs (set before each tick)
| Method | Description |
|--------|-------------|
| `sim.setPressure(pa)` | Barometric pressure in Pascals |
| `sim.setContinuity(ch, adc, good, open)` | Pyro circuit status |
| `sim.clearPyroFiring()` | Clear firing flag |

#### Outputs (read after each tick)
| Property | Description |
|----------|-------------|
| `sim.state` | Flight state (0-7) |
| `sim.stateName` | State name string |
| `sim.altitudeCm` | Filtered altitude (cm) |
| `sim.maxAltCm` | Peak altitude (cm) |
| `sim.vspeedCms` | Vertical speed (cm/s) |
| `sim.pyro1Fired` | Pyro 1 has fired |
| `sim.pyro2Fired` | Pyro 2 has fired |
| `sim.armed` | Pyros armed |
| `sim.pyroFireCount` | Total fires |
| `sim.lastFireChannel` | Last fired channel |
| `sim.buzzerActive` | Buzzer on/off |

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

| Value | Name | Description |
|-------|------|-------------|
| 0 | BOOT_INIT | Loading config, init hardware |
| 1 | BOOT_SETTLE | Waiting for sensors to stabilize |
| 2 | BOOT_CONTINUITY | Checking pyro circuits |
| 3 | BOOT_CALIBRATE | Establishing ground pressure |
| 4 | PAD_IDLE | On pad, waiting for launch |
| 5 | ASCENT | Rocket ascending |
| 6 | DESCENT | Past apogee, descending |
| 7 | LANDED | On ground after flight |

## Files

| File | Purpose |
|------|---------|
| `sim/physics.h` | Physics engine C API |
| `sim/physics.c` | Physics implementation (atmosphere, thrust, drag) |
| `sim/pyro_sim.h` | Flight computer simulation C API |
| `sim/hal_sim.h` | Internal sim HAL accessors |
| `sim/hal_sim.c` | In-memory HAL implementation |
| `sim/main_sim.c` | High-level sim lifecycle functions |
| `docs/wasm/pyro-sim.js` | ES module wrapper for WASM |
| `docs/wasm/pyro.js` | Emscripten glue (generated) |
| `docs/wasm/pyro.wasm` | WASM binary (generated) |
| `scripts/build_wasm.sh` | Build script |

## Examples

- **Interactive browser sim**: `docs/sim.html` — UI driving WASM flight computer
- **CLI simulator**: `sim/sim_cli.c` — C physics + flight computer
- **Closed-loop tests**: `test/test_closedloop.c` — 13 tests × 4 altitudes

## Building for C projects

The physics engine and flight sim also work as plain C libraries:

```bash
cc -I sim/ -I src/ \
   sim/physics.c sim/main_sim.c sim/hal_sim.c \
   src/flight_states.c src/telemetry.c src/buzzer.c \
   my_app.c -lm -o my_app
```
