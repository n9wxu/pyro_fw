# Pyro WASM Simulation — Integration Guide for AI Assistants

> **Purpose**: This file is intended for AI coding assistants (Copilot, Cline, Cursor, ChatGPT, etc.)
> that need to integrate the Pyro flight computer simulation into another web project.
> Read this file first when asked to "add the pyro simulation" or "integrate the rocket sim."

## What This Is

Pyro is a family of model rocket flight computers (MK1A, MK1B, MK1C). Its firmware (state machine,
pyro firing logic, telemetry) is compiled to WebAssembly along with a physics engine. Together they form a
**closed-loop rocket flight simulation** that runs entirely in the browser.

The WASM module contains two independent subsystems:
1. **Flight Computer** — the real firmware (state machine, pyro logic, telemetry, buzzer)
2. **Physics Engine** — standard atmosphere model, thrust, drag, chute deployment

The pyro channels are a fixture: continuity is whatever `sim.setContinuity()` last set, and a
fire is a counter. The module also carries the Lua VM that `docs/lua.html` drives.

## Files to Copy

Copy these 3 files from the `docs/wasm/` directory into your project:

```
docs/wasm/pyro.js       ← Emscripten glue code (do not edit)
docs/wasm/pyro.wasm     ← WASM binary (do not edit)
docs/wasm/pyro-sim.js   ← ES module wrapper (clean JavaScript API)
```

If these files are stale or missing, rebuild them:
```bash
cd pyro_fw && ./scripts/build_wasm.sh
```
This requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html).
`PYRO_BOARD=sim_mk1a|sim_mk1b|sim_mk1c` builds of this script do not compile; the default does.

## Integration Pattern

### Step 1: Import the module

```javascript
// ES module import (recommended)
import { createPyroSim } from './wasm/pyro-sim.js';

// flight_state_t numbers (see Flight States). pyro-sim.js's FlightState,
// STATE_NAMES and sim.stateName do not follow them: compare sim.state.
const PAD_IDLE = 3, LANDED = 8, FAULT = 10;
```

`pyro-sim.js` is an ES module. Without modules, load `wasm/pyro.js` with a
script tag and call the exports on `Module` in `Module.onRuntimeInitialized`,
as `docs/sim.html` does (`Module._sim_flight_tick(t)`, `Module._sim_set_pressure(pa)`, ...).

### Step 2: Initialize

```javascript
const sim = await createPyroSim();
const phys = sim.physics;

// Configure the flight computer (same format as config.ini on the real device)
sim.init([
    "pyro1_mode=delay",    // Fire drogue at apogee (0s delay)
    "pyro1_value=0",
    "pyro2_mode=agl",      // Fire main at 200ft AGL
    "pyro2_value=200",
    "units=ft",
].join("\n"));

// Set pyro continuity (both channels good)
sim.setContinuity(1, 50, true, false);  // ch=1, adc=50, good=true, open=false
sim.setContinuity(2, 50, true, false);

// Initialize physics for target apogee (meters)
phys.init(1524);  // ~5000 ft
```

### Step 3: Run the simulation loop

```javascript
const PAD_DWELL_MS = 9000;  // Boot, then let the 5 s ground reference fill
let prevFires = 0;

for (let t = 0; t <= 600000; t++) {
    // 1. Check if flight computer fired a pyro → deploy chute in physics
    if (sim.pyroFireCount > prevFires) {
        const ch = sim.lastFireChannel;
        if (ch === 1) phys.deployDrogue();
        if (ch === 2) phys.deployMain();
        prevFires = sim.pyroFireCount;
    }

    // 2. Step physics (after pad dwell period)
    if (t >= PAD_DWELL_MS) {
        phys.step((t - PAD_DWELL_MS) / 1000);  // seconds since launch
    }

    // 3. Feed physics output to flight computer
    sim.setPressure(phys.pressurePa);

    // 4. Tick the flight computer (it ends the previous fire pulse itself)
    sim.tick(t);

    // 5. Read outputs for your UI
    // sim.state          → 0-11 (see Flight States)
    // sim.altitudeCm     → filtered altitude in cm
    // sim.maxAltCm       → peak altitude in cm
    // sim.vspeedCms      → vertical speed in cm/s
    // sim.pyro1Fired     → boolean
    // sim.pyro2Fired     → boolean
    // sim.armed          → boolean
    // sim.buzzerActive   → boolean
    // phys.altM          → actual altitude in meters
    // phys.velMs         → actual velocity in m/s
    // phys.apogeeM       → peak altitude in meters

    if (sim.state === LANDED) break;
}
```

### Step 4: Real-time animation (browser)

For browser rendering, use `setInterval` or `requestAnimationFrame`:

```javascript
let simTime = 0;
const STEPS_PER_FRAME = 160;  // 10× real-time at 60fps

function frame() {
    for (let i = 0; i < STEPS_PER_FRAME; i++) {
        simTime++;
        // ... same loop body as above ...
    }
    updateUI(sim, phys);  // your rendering code
    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
```

## API Quick Reference

### Flight Computer (`sim.*`)

| Method/Property | Type | Description |
|----------------|------|-------------|
| `sim.init(configStr)` | method | Initialize with config.ini string |
| `sim.tick(timeMs)` | method → int | Advance to `timeMs`, returns state |
| `sim.reset()` | method | Reset to power-on |
| `sim.setPressure(pa)` | method | Set barometric pressure |
| `sim.setSensorType(t)` | method | 0 none (boots to FAULT), 1 MS5607, 2 BMP280 (default) |
| `sim.setContinuity(ch,adc,good,open)` | method | Set pyro continuity |
| `sim.clearPyroFiring()` | method | Clear fire flag; `tick()` does this itself |
| `sim.state` | int | Current state (0-11) |
| `sim.stateName` | string | From `pyro-sim.js`'s own list, which does not follow `flight_state_t`: use `sim.state` |
| `sim.altitudeCm` | int | Filtered altitude (cm) |
| `sim.maxAltCm` | int | Peak altitude (cm) |
| `sim.vspeedCms` | int | Vertical speed (cm/s) |
| `sim.pressure` | int | Filtered pressure (Pa) |
| `sim.pyro1Fired` | bool | Pyro 1 fired |
| `sim.pyro2Fired` | bool | Pyro 2 fired |
| `sim.armed` | bool | Pyros armed |
| `sim.pyroFireCount` | int | Total fire count |
| `sim.lastFireChannel` | int | Last fired channel (1 or 2) |
| `sim.buzzerActive` | bool | Buzzer state |
| `sim.snapshot()` | object | All outputs as plain object |

### Physics Engine (`sim.physics.*`)

| Method/Property | Type | Description |
|----------------|------|-------------|
| `phys.init(targetAltM)` | method | Configure for target apogee |
| `phys.step(flightTimeSec)` | method | Advance 1ms of physics |
| `phys.reset()` | method | Reset physics |
| `phys.deployDrogue()` | method | Deploy drogue chute |
| `phys.deployMain()` | method | Deploy main chute |
| `phys.altM` | float | Current altitude (meters) |
| `phys.velMs` | float | Velocity (m/s, +up) |
| `phys.pressurePa` | float | Barometric pressure (Pa) |
| `phys.apogeeM` | float | Peak altitude (meters) |
| `phys.onGround` | bool | Rocket on ground |
| `phys.drogueDeployed` | bool | Drogue deployed |
| `phys.mainDeployed` | bool | Main chute deployed |
| `phys.altToPressure(altM)` | method → float | Convert altitude to pressure |
| `phys.snapshot()` | object | All state as plain object |

## Flight States

```
0  = BOOT_SETTLE      (sensor warmup, 2.5s)
9  = BOOT_SENSOR      (the pressure sensor answers, or FAULT)
1  = BOOT_CONTINUITY  (checking pyro circuits)
2  = BOOT_CALIBRATE   (10 pressure samples → ground reference)
3  = PAD_IDLE         (ready for launch)
4  = ASCENT           (climbing; pyros arm once past 10 m/s and slowing below it)
5  = FALLING          (past apogee, no canopy steadying the descent)
6  = DROGUE_DESCENT   (steady at a drogue's rate)
7  = CHUTE_DESCENT    (steady at a main's rate)
8  = LANDED           (on ground after flight)
10 = FAULT            (terminal: a power-up test failed)
11 = GROUND_TEST      (terminal: powered up with the ground test switch held)
```

`flight_state_t` in `src/flight_states.h`: states are appended, never renumbered. The
descent phase comes from the measured rate, not from which pyro fired (DD-023). Pressure is
read every 20 ms of simulated time in every state (DD-065).

The boot sequence runs automatically and reaches PAD_IDLE about 2.7 s after power-on. The
flight computer detects launch at 100 ft above the ground reference while climbing faster than
5 m/s, both held for 100 ms (FLT-LAUNCH-07). No explicit "launch" command is needed — just
start feeding decreasing pressure (increasing altitude) from the physics engine, after the
5 s ground reference has filled. Ignite before PAD_IDLE and the board calibrates on the way
up, reading every altitude low.

## Pyro Configuration Modes

| Mode | Value meaning | Example |
|------|--------------|---------|
| `delay` | Seconds after apogee | `pyro1_mode=delay`, `pyro1_value=0` (at apogee) |
| `agl` | Altitude above ground (in configured units) | `pyro2_mode=agl`, `pyro2_value=200` (200 ft) |
| `fallen` | Altitude fallen from apogee (in units) | `pyro1_mode=fallen`, `pyro1_value=100` |
| `speed` | Descent speed (in units/s) | `pyro1_mode=speed`, `pyro1_value=30` |
| `none` | The channel never fires | `pyro2_mode=none` |

Units: `cm`, `m`, or `ft` (set via `units=ft` in config).

## Common Integration Scenarios

### "Add a rocket simulation to my dashboard"
1. Copy the 3 WASM files
2. Add a canvas and start/reset buttons
3. Use the loop pattern above with `requestAnimationFrame`
4. Plot `phys.altM` vs time on the canvas
5. Show `sim.stateName`, `sim.altitudeCm`, `sim.maxAltCm` in the UI

### "Run the sim headlessly for testing"
```javascript
const sim = await createPyroSim();
sim.init("pyro1_mode=delay\npyro1_value=0\nunits=ft\n");
sim.setContinuity(1, 50, true, false);
sim.setContinuity(2, 50, true, false);
sim.physics.init(304.8);  // 1000 ft

for (let t = 0; t <= 120000; t++) {
    if (sim.pyroFireCount > 0 && sim.lastFireChannel === 1) sim.physics.deployDrogue();
    if (t >= 9000) sim.physics.step((t - 9000) / 1000);
    sim.setPressure(sim.physics.pressurePa);
    sim.tick(t);
    if (sim.state === 8) break;  // LANDED
}
console.log("Apogee:", sim.physics.apogeeM, "m");
console.log("Max alt (firmware):", sim.maxAltCm / 100, "m");
```

### "Use my own physics engine instead of the built-in one"
Just don't call `sim.physics.*`. Feed your own pressure values:
```javascript
sim.setPressure(myPhysicsEngine.getPressurePa());
sim.tick(t);
// Check sim.pyroFireCount to know when to deploy chutes in your engine
```

## Troubleshooting

| Issue | Solution |
|-------|---------|
| `WASM load timeout` | Ensure `pyro.js` and `pyro.wasm` are served from the same directory |
| Flight computer stays in BOOT | Let it run for ~3000ms (PAD_IDLE at ~2.7 s) |
| State 10 (FAULT) | Sensor type 0 at boot: no sensor, no flight (FLT-BOOT-12) |
| Pyros don't fire | Check: (1) continuity set, (2) armed: the climb passed 10 m/s, (3) apogee detected, (4) config mode/value correct |
| State stuck at PAD_IDLE | Launch needs 100 ft and a climb faster than 5 m/s, held 100 ms |
| Two pyros fire simultaneously | By design they don't — P2 waits for P1's pulse to end (PYR-DEPLOY-02) |

## Source Repository

GitHub: https://github.com/n9wxu/pyro_fw

Key files:
- `sim/pyro_sim.h` — C API documentation
- `sim/physics.h` — Physics engine C API
- `sim/README.md` — Detailed architecture, API tables and the other simulator builds
- `docs/sim.html` — Working example (interactive browser simulation)
- `test/test_closedloop.c` — 25 closed-loop tests, with their own physics and four rocket profiles
