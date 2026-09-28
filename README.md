# pyro_fw
Firmware for the Pyro MK1 rocket flight computers: MK1A, MK1B and MK1C

**[🚀 Try the web interface — no hardware needed](https://n9wxu.github.io/pyro_fw/)**

Flying a board? Start with the [Operator's guide](#operators-guide).

## Features
- **Dual Pyrotechnic Control** - two channels, each fired only when its own switch and a shared element are both on; the switches differ by board (see Hardware)
- **Five Firing Modes** - none, fallen distance, AGL altitude, descent speed, timed delay
- **Fault Detection** - post-fire verification on every board; MK1B's AP2192 flags overcurrent; MK1C latches bus shorts and refuses a fire it cannot make safely
- **Flight Log** - binary records (`flight_log.bin`) written only inside core0's flash window, rendered as CSV on download; three logging plans (DD-062, DD-064)
- **Real-time Telemetry** - `$PYRO` NMEA or JSON on UART0 through an interrupt-driven ring; `telem_rate_hz` in flight (10 Hz default), 1 Hz on the ground; event sentences at apogee, fire and landing
- **Pressure Sensing** - MS5607 (MK1B, MK1C) or BMP280 (MK1A), 50 conversions a second, each stamped at its measurement (DD-051, DD-066, DD-067)
- **Altitude and Speed** - hypsometric altitude against a 5 s ground mean; speed and triggers from a quadratic fit to the last second of pressure (DD-048)
- **Mach Lockout** - apogee is not believed while the flow past the ports may be supersonic (DD-049)
- **Continuity Checking** - presence and shorts, continuously on the pad
- **Web Interface** - live dashboard and configuration over USB networking
- **OTA Firmware Updates** - A/B bootloader with automatic rollback
- **Lua on core1** - user scripts on MK1A, MK1B and MK1C, on pads and pyro channels released to them
- **WASM Simulation** - the flight software runs in a browser against a physics engine
- **Status Beep Codes** - four outcomes, Eggtimer-style by default, configurable
- **Altitude Beep-out** - max altitude announced after landing, in cm, m or ft
- **Ground Test** - serial ARM/FIRE commands on the pad, and a power-up procedure started by a switch (DD-071)

## Hardware
All three boards are RP2040s. Each has a theory of operation in `boards/<name>/THEORY_OF_OPERATION.md`.

| | MK1A | MK1B | MK1C |
|---|---|---|---|
| Form | bare RP2040 | bare RP2040 | bare RP2040 |
| Flash | 16 MB, 8 MB littlefs | 2 MB (W25Q16JV), 984 KB littlefs | 16 MB, 8 MB littlefs |
| Pressure | BMP280, I2C0 at 400 kHz | an MS5607 or a BMP280 pad, I2C1 | MS5607, I2C1 at 400 kHz |
| Channel switch | a MOSFET high side per channel | U5 AP2192 high side per channel | Q103/Q104 low side per channel |
| Shared element | Q2 low side, 8 A fuse | Q1B low side, 1.5 A PTC | U9 TPS259570 eFuse bus, armed by a PIO charge pump |
| Buzzer | none fitted; assignable to a pad | BUZZER1 through Q1A | through Q2 |
| Lua pads | GPIO18, 19 | GPIO8 | GPIO18–21 |

The `reference` board is a template for new hardware: it reports no continuity and refuses every fire. The `sim` boards run the firmware on a host (see WASM Simulation).

## Operator's guide

For flying and bench-testing a built board. The sections after this one, and
each board's `boards/<name>/THEORY_OF_OPERATION.md`, carry the detail.

### Safety

- **Igniters out on the bench.** On USB the board does not detect a launch
  and fires nothing by itself (USB-01), but three things do fire on USB:
  test mode, the ground test switch and the serial `FIRE` command.
- **MK1B as built cannot fire.** Its U5 (AP2192A) holds both sense nodes at
  0 V, so every channel reads shorted: the board beeps check pyro 1 and
  fires neither channel until U5 is the base AP2192 (DD-059, B-U5 in
  `docs/outstanding_tasks.md`).
- **A channel released to Lua is not checked.** Release both and the board
  says OK to fly with no pyro channel at all.
- **The pad verdict is only as good as the sensing.** MK1C checks that an
  igniter is present and not shorted, and nothing more (DD-055).
- **In flight, the pyros arm late.** They arm only once the board
  has seen the motor burn -- faster than 10 m/s, then slower -- and the
  rocket is about 30 m up (DD-017, FLT-MACH-06). Use your airframe's own
  arming switch as well; the firmware's gates are not a substitute.

### Connecting

Plug the board into a computer's USB port. It appears as a network adapter
and gives the computer an address by DHCP. Browse to **http://pyro.local/**
(**pyro-1.local** for a second board attached at the same time). Each board
also has a fixed address, **192.168.N.1**, where N comes from its unique ID;
it is the router address of the new adapter.

While a computer is attached the board treats itself as on the bench: it
detects no launch, announces nothing but one double chirp on attach, and
writes no pad marker (USB-01..04). Unplug it and it carries on as on
battery.

### Setting up: the Config tab

| Setting | What it does |
|---|---|
| Rocket ID, Rocket Name | Up to 8 characters each, written into the flight log's header. |
| Units | cm, meters (default) or feet, for every altitude and speed below and for the altitude beep-out. |
| Pyro 1 (drogue), Pyro 2 (main) | Each a mode and a value. **Delay**: seconds after apogee (pyro 1's default is 0: at apogee). **AGL**: fires descending through this height above the pad (pyro 2's default is 300 m). **Fallen**: fires this far below the peak. **Speed**: fires when the descent is faster than this. **Disabled**: never fires. |
| Flight log | **1 row a second** (default); **High rate 1**: that, and every sample within 1 s of an event; **High rate 2**: every sample, 50 a second. The estimate beside it is how long a flight fits (DD-062, DD-064). |
| Release pyro pins to Lua | Gives a channel's pads to a Lua script; that channel then never fires. |
| Buzzer | Which pad drives the buzzer. MK1A fits none: wire one to a J6 user pad and choose it here. |
| Ground test | The ground test switch's wiring (see below). |

**Save**, then **Reboot**. Pin changes take effect only after the reboot.

### On the pad

Power the board from its battery. It waits 2.5 s, tests the pressure sensor,
reads both pyro channels, calibrates the ground, then announces its verdict
and repeats it every 5 s until launch (BUZ-01, BUZ-02):

| Sound | Meaning | Do |
|---|---|---|
| a rapid chirp | OK to fly | fly |
| 5 beeps | check pyro 1 | fix pyro 1's igniter or leads |
| 4 beeps | check pyro 2 | fix pyro 2's igniter or leads |
| 2 beeps | system failure | safe the rocket and walk away: it cannot be fixed at the pad |

These are the shipped sounds; the **Beep Codes** tab changes them. Silence
means something is wrong: a working board never stops repeating. About ten
seconds after its checks the board writes a pad marker, which lets it rejoin
the flight if its power drops out in the air.

### In flight

The board declares a launch above 100 ft and 5 m/s, held 100 ms
(FLT-LAUNCH-01, DD-016).
It arms once the burn is over, finds apogee from the pressure itself, and
fires each channel as its mode says. While the rocket may be past about
Mach 0.62, the Mach lockout holds apogee back until the readings are
trustworthy again (`docs/mach_lockout.md`). It lands when still for 1 s below
30 m, or when the landing timeout (60 s by default) has passed since apogee
and it is still.

After landing it beeps the peak altitude, in the chosen units, over and
over: a long beep, then each digit counted out, zero as ten beeps (BUZ-03..06).

### After the flight

Connect USB and open the **Flight Data** tab: the duration, apogee, each
channel's outcome and the altitude profile. **Download Flight CSV** saves
the log. Download it before the next flight: the board keeps one flight
log, and the next launch starts it afresh.

### Test mode: flying on USB

**Status** tab, **Test mode: fly on USB**. The board then detects a launch,
fires and beeps on USB as it does on battery -- for a vacuum-chamber test
with the web page open (USB-08). A reboot ends it.

### Ground test

A switch that fires the pyros on the ground, in a fixed, announced order
(DD-071).

1. **Wire the switch** and choose its wiring on the Config tab, under
   **Ground test**: a switch from one pad to ground, or a switch across two
   pads -- on MK1A, the two J6 user pads, GPIO18 and GPIO19.
2. **Power up with the switch closed.** Three long beeps and a pause,
   repeating: the board is in ground test mode.
3. **Open the switch** (after at least a second). The countdown: five fast
   beeps, then four, one count a second, down to none. At zero pyro 1 fires.
4. A steady tone for 3 s, then the countdown again. At zero pyro 2 fires.
5. Three long beeps, then silence.

Only an enabled channel fires: one whose mode is not Disabled and whose pads
are not released to Lua. With one channel enabled step 4 is skipped; with
none, the countdown leads straight to step 5. **Close the switch again to
stop** before the next fire: the board goes back to step 2, and opening the
switch again starts over from the first countdown. The board stays in ground
test mode until it is next powered up. With no buzzer fitted it keeps the same schedule, silently.

### Serial ground test commands

On the telemetry UART (115200 baud), and only on the pad (GND-TEST-01..04):
`BEEP STATUS`, `BEEP ALT <n>`, `ARM 1` or `ARM 2` then `FIRE 1` or `FIRE 2`
within 3 s, and `STATUS`.

### Updating the firmware

**Update** tab: **Check for Updates**, or **Upload Firmware** with a `.bin`.
From a computer: `support/upload_fw.sh <image.bin> <address>`. The board
reboots into the new image.

### Board notes

- **MK1A**: BMP280 sensor; no buzzer fitted (see Buzzer above).
- **MK1B**: MS5607 sensor. Cannot fire as built (above). Its buzzer disturbs
  its pressure readings while it sounds (B-BZ).
- **MK1C**: MS5607 sensor. Fires from the battery through an electronic
  fuse; refuses a fire below a 3.0 V pack, or on a channel it has not seen
  present (PYR-ARM-02).

## Flight States
1. **BOOT_SETTLE → BOOT_SENSOR → BOOT_CONTINUITY → BOOT_CALIBRATE** - 2.5 s settle, the sensor and filesystem checked, then continuity, then a 10-reading ground reference
2. **PAD_IDLE** - launch at 100 ft and 5 m/s, held 100 ms; never while a USB host is attached, unless test mode is on (USB-01, USB-08)
3. **ASCENT** - the pyros arm once the climb has passed 10 m/s and slowed below it again, above about 30 m (DD-017); apogee comes from the pressure fit
4. **FALLING, DROGUE_DESCENT, CHUTE_DESCENT** - pyros fire by their modes; the phase is read from the descent rate (DD-023); an emergency ladder answers a canopy that did not work (DD-028)
5. **LANDED** - still for 1 s near the ground, or the landing timeout; the log closes and the altitude beep-out starts
6. **FAULT** - terminal: no sensor, no filesystem, or no readings to calibrate from within 10 s
7. **GROUND_TEST** - terminal until power-off: the switch-started ground test procedure

Apogee is the ASCENT → FALLING transition, not a state. `docs/flight_states.md` has every transition and threshold.

## Pyro Modes
| Mode | Description |
|------|-------------|
| none | Channel disabled |
| fallen | Distance fallen from apogee (in configured units) |
| agl | Altitude above ground level (in configured units) |
| speed | Downward vertical speed threshold (configured units/second) |
| delay | Seconds after apogee event |

A channel fires only with continuity (PYR-SAFE-01), and only after apogee (PYR-SAFE-04).

## Configuration
Three files in littlefs, each edited from the web interface.

**`config.ini`** - the flight settings (Config and Lua tabs, `GET`/`POST /api/config`). A POST is merged over the running configuration and applied at once, on the pad only (CFG-06):

```ini
[pyro]
id=PYRO001
name=MyRocket
pyro1_mode=delay
pyro1_value=0
pyro2_mode=agl
pyro2_value=300
units=m
telem_format=0
telem_rate_hz=10
log_rate=1hz
landing_timeout=60
lua_enabled=false
lua_baud=9600
lua_pixels=0
```

These are the defaults, written when the file is missing.

- **Units:** `cm`, `m` or `ft` - applies to pyro values and altitude reporting
- **telem_format:** `0` NMEA, `1` JSON
- **log_rate:** `1hz`, `events` or `full` (see Data Logging)
- **landing_timeout:** seconds of descent before a still rocket counts as landed (FLT-LAND-07)

**`pins.ini`** - the hardware map (Config and Lua tabs, `/api/pins`): pyro channels released to Lua (`pyro1_released`, `pyro2_released`), `buzzer_pin`, the ground test switch (`ground_test=none|ground|pair`, `ground_test_pin`, `ground_test_drive_pin`), and each Lua pad's role and name. `GET /api/pins/caps` serves the board's capability table and the vocabulary to read it, so the UI holds no board knowledge. A change takes effect at the next reboot.

**`beep.ini`** - the beep personalities (Beep Codes tab, `/api/beeps`).

A `pins.ini` or `beep.ini` that fails validation is rejected whole, and the board falls back to its defaults and says why (`pins_reason` on `/api/status`, `reason` on `/api/beeps`).

## Status Beep Codes
A beep says what to do. There are four outcomes; the diagnosis behind them is on `/api/status` (`faults[]`).

| Outcome | Default sound | Means |
|---------|---------------|-------|
| ok_to_fly | rapid chirp, never counted | sensor, filesystem and both pyro channels are good |
| check_pyro_1 | 5 beeps | pyro 1's igniter or leads need attention |
| check_pyro_2 | 4 beeps | pyro 2's igniter or leads need attention |
| system_failure | 2 beeps | safe the system and leave the pad |

The defaults follow Eggtimer Rocketry's convention. There are three personality slots: each sets every outcome's sound (chirp, tone, a count of 1-9 in one or two groups, or silent), the gap between repeats, the repeat count, and whether the two channels get separate codes. By default the announcement repeats every 5 s until launch (BUZ-02). A counted beep is 100 ms on, 200 ms off, with 300 ms between groups.

With a USB host attached the board plays one double chirp and announces nothing else until detached (USB-02, USB-03); test mode lifts this.

## Pin Assignments
`boards/<name>/board_pins.h` is each board's map, and `pin_caps.h` says what each pin may become.

| Function | MK1A | MK1B | MK1C |
|---|---|---|---|
| UART0 TX, RX (telemetry) | 0, 1 (one wire, J6.3) | 0, 1 (TRRS jack) | 0, 1 (J1.4, J1.5) |
| Pressure I2C | SDA 20, SCL 21 | SCL 7; SDA 10 (MS5607), 6 (BMP280) | SDA 6, SCL 7 |
| Channel 1, 2 switch | 9, 11 | 21, 22 | 17, 24 |
| Shared element | 10 (low side) | 15 (low side) | 12 (ARM_TOGGLE pump) |
| Fault flags | — | 17, 18 (active low) | — |
| Sense ADC | 26, 27 | 26, 27 | 26 pack, 27 bus, 28 A, 29 B |
| Bias injectors | — | — | 16 A, 25 B, 23 bus |
| LED | 25 | 25 | 8 |
| Buzzer | none | 16 | 11 |

## Data Logging
From launch to landing the flight software hands every sample to `hal_log_sample()`. The log's plan decides what `flight_log.bin` keeps (FLT-LOG-07):

- **`1hz`** (default) - a sample row a second
- **`events`** - that, and every sample within 1 s of an event
- **`full`** - every sample, 50 a second; the only plan `pyro_sim --replay` accepts (DAT-08)

Every event row is kept at its own time under each plan. Records go to a 4 KB RAM buffer. Nothing is written to flash for the first 2 s or until the buffer fills (FLT-LOG-05); then core0 writes it inside its flash window and syncs the file once a second, so a flight that never lands keeps its record (FLT-LOG-06).

- **CSV export:** `/api/flight.csv` renders the binary log as CSV, with a header naming the board, the configuration, the ground pressure and the log rate
- **Columns:** `time_ms, pressure_pa, altitude_cm, state, thrust, raw_pa, temp_c, event`
- **Events:** LAUNCH, ARMED, APOGEE, PYRO1, PYRO2, LANDING, and when they happen the Mach lock's (LOCK, UNLOCK, LOCK_FALLBACK), refusals, faults, failed verifies, MAIN_FORCED, SENSOR_STUCK and SENSOR_LOST
- **Lua:** a script's `log.line()` and `log.write()` output lands in the log as `LUA` rows, during a flight only; `print()` goes to `/api/lua/console`
- **Space:** `/api/log/space` reports the room the next flight has; the Config tab turns it into the longest flight the chosen plan holds
- **Erase:** `POST /api/flight/erase`

In flight the flight log holds the filesystem: every other file request is answered 423 (DD-058).

## Telemetry (UART0)
**Format:** `$PYRO` NMEA sentences (default) or JSON (`telem_format=1`), 115200 baud

```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags_hex,p1adc,p2adc,0,0*XX\r\n
```

- **Rate:** `telem_rate_hz` (default 10, at most 50) in ASCENT and the descent states; 1 Hz in PAD_IDLE and LANDED; none in boot states or GROUND_TEST
- **State:** 0=PAD_IDLE, 1=ASCENT, 2=FALLING, 3=DROGUE_DESCENT, 4=CHUTE_DESCENT, 5=LANDED
- **Flags:** bit0=P1_CONT, bit1=P2_CONT, bit2=P1_FIRED, bit3=P2_FIRED, bit4=ARMED, bit5=APOGEE
- **Last two fields:** always 0
- **Checksum:** XOR of all bytes between `$` and `*`
- **Diagnostics:** lines beginning `!` (a FAULT board sends `!FAULT <diagnosis>` every 5 s and no `$PYRO`)

**Event sentences (NMEA format):**
```
$PYRO_APO,max_alt_cm,flight_time_ms*XX\r\n
$PYRO_FIRE,channel,alt_cm,flight_time_ms*XX\r\n
$PYRO_LAND,max_alt_cm,flight_time_ms*XX\r\n
```

**JSON format** (`telem_format=1`):
```json
{"t":"state","seq":42,"st":1,"thr":0,"alt":150000,"spd":2150,"max":150000,"pa":84300,"ms":8500,"fl":19,"a1":12,"a2":15}
{"t":"apogee","max":152400,"ms":9200}
{"t":"fire","ch":1,"alt":152400,"ms":9201}
{"t":"landing","max":152400,"ms":98000}
```

**Example (NMEA):**
```
$PYRO,42,1,0,150000,2150,150000,84300,8500,13,12,15,0,0*0E
```

See `docs/ground-station-interface-spec.md` for the ground-station contract.

## Ground Test Interface

### Serial commands
From a PC or handset on UART0 RX (115200 baud), plain-text commands, read only in PAD_IDLE:

| Command | Description |
|---------|-------------|
| `STATUS` or `?` | Send a `$PYRO` sentence now |
| `BEEP` or `BEEP STATUS` | Replay the pad verdict on the active personality |
| `BEEP ALT <n>` | Beep out the number n |
| `ARM 1` or `ARM 2` | Arm a channel that has continuity, for 3 seconds |
| `FIRE 1` or `FIRE 2` | Fire the armed channel (must ARM first within 3 s) |

Responses are NMEA-style `$GT,...*XX` sentences with XOR checksum:
```
$GT,ARMED,1*7D
$GT,FIRED,1*7E
$GT,ERR,not_armed*03
```

Other answers: `GT,DISARMED,timeout`, and `GT,ERR,` with `arm_expired`, `busy` (the other channel's pulse is running), `refused,<n>`, `p1_no_cont`, `p2_no_cont` or `unknown_cmd`.

### Switch procedure (DD-071)
A switch assigned in `pins.ini`, either to ground or across two pads. Power up with it closed (held for the last 0.5 s of the settle) and, once the sensor and continuity are checked, the board enters GROUND_TEST and announces it: three long beeps and a pause, repeating. After it has been closed 1 s in the mode, opening it starts the procedure:

1. a countdown, a count a second, five fast beeps down to none; at zero pyro 1 fires;
2. a 3 s steady tone and a second countdown; at zero pyro 2 fires;
3. three long beeps, once, then silence until power-off.

A channel whose mode is none, or whose pads are released to Lua, is skipped: with one channel left there is one countdown and no tone; with none, the countdown leads to the all-clear. Closing the switch during a countdown or the tone stops the procedure before the next fire. GROUND_TEST never detects a launch or opens a log. A board with no buzzer fires on the same clock, silently. The bench check with dummy loads is owed (docs/outstanding_tasks.md, GT-1).

## Safety Features
1. **Continuity before firing** - a channel without continuity never fires (PYR-SAFE-01)
2. **Two-key firing** - a channel's own switch and the shared element must both be on
3. **Current protection** - MK1A an 8 A fuse, MK1B a 1.5 A PTC and the AP2192's limit, MK1C the eFuse's current limit
4. **Fault monitoring** - MK1B's FLAG pins; MK1C's short latches and fire preconditions
5. **Post-fire verification** - 500-600 ms after a fire a channel still reading good is logged as not opened; pyro 1 is then re-fired once if the descent shows no canopy (PYR-VERIFY-01, PYR-REFIRE-01)
6. **A failed sensor deploys nothing** - a stuck or silent sensor holds every pressure trigger (DD-050)
7. **USB means grounded** - with a host attached there is no launch detection and no pad marker, unless test mode is on (USB-01, USB-08)
8. **Ground test safety** - serial: 3 s ARM→FIRE window, PAD_IDLE only; switch: power-up only, after the sensor and continuity checks

## Continuity Detection
- **MK1A, MK1B:** the shared low side is the stimulus against a 100 kΩ pull-up, read on the 12-bit ADC. With it on, under 500 counts is an igniter and over 3000 open; with it off, a channel still low is shorted (DD-059). MK1A checks every 500 ms, MK1B every second.
- **MK1B as built cannot sense continuity:** its AP2192A discharges the sense nodes while disabled, so every channel reads shorted and none fires (DD-059, task B-U5).
- **MK1C:** checks presence and shorts, nothing else (DD-055). Every 500 ms the bus is biased; a channel is present when it reads at least half the bus. A bus that will not rise, or that sits at the pack with nothing armed, latches a short.

## WASM Simulation — Use in Other Projects

The flight software and a physics engine compile to **WebAssembly**, so a web project can run closed-loop rocket flights in the browser. The module runs the real state machine, pyro logic and telemetry against an in-memory HAL (`boards/sim/hal_sim.c`).

### Quick Start

```bash
# Build the WASM module (requires Emscripten SDK)
./scripts/build_wasm.sh
```

Copy 3 files into your project:
```
docs/wasm/pyro.js       ← Emscripten glue (generated)
docs/wasm/pyro.wasm     ← WASM binary (generated)
docs/wasm/pyro-sim.js   ← ES module API wrapper
```

Then in your JavaScript:
```javascript
import { createPyroSim } from './wasm/pyro-sim.js';

const sim = await createPyroSim();
sim.init("pyro1_mode=delay\npyro1_value=0\nunits=ft\n");
sim.setContinuity(1, 50, true, false);
sim.setContinuity(2, 50, true, false);
sim.physics.init(1524);  // 5000 ft target apogee

// Closed-loop: physics feeds pressure → flight computer fires pyros → physics deploys chutes
for (let t = 0; t <= 120000; t++) {
    if (sim.pyroFireCount > 0 && sim.lastFireChannel === 1) sim.physics.deployDrogue();
    if (t >= 2000) sim.physics.step((t - 2000) / 1000);
    sim.setPressure(sim.physics.pressurePa);
    sim.clearPyroFiring();
    sim.tick(t);
    if (sim.state === 8) break;  // LANDED (src/flight_states.h)
}
console.log("Apogee:", sim.physics.apogeeM.toFixed(0), "m");
```

`sim.state` is the firmware's `flight_state_t`. The `FlightState` table `pyro-sim.js` exports does not match `src/flight_states.h`; compare against the header.

### Integration Documentation

| Document | Audience | Contents |
|----------|----------|----------|
| **[sim/INTEGRATION.md](sim/INTEGRATION.md)** | AI assistants & developers | Step-by-step integration guide, full API reference, troubleshooting, code examples |
| **[sim/README.md](sim/README.md)** | Developers | Architecture diagram, API tables, build instructions, C library usage |
| **[docs/sim.html](https://n9wxu.github.io/pyro_fw/sim.html)** | Anyone | Interactive browser demo — working example of WASM integration |

### For C/C++ Projects (No WASM)

The simulator also builds as a native program:
```bash
cmake -B build-sim -DPYRO_BOARD=sim && cmake --build build-sim --target sim
./build-sim/pyro_sim 300                  # a flight sized for a 300 m apogee
./build-sim/pyro_sim --replay flight.csv  # a logged flight, replayed
```

`PYRO_BOARD=sim_mk1a`, `sim_mk1b` or `sim_mk1c` runs a board's real pyro backend against a model of that board. See `sim/pyro_sim.h` and `sim/physics.h` for the C API, and `boards/sim/THEORY_OF_OPERATION.md`.

## Building
Requires Pico SDK 2.2.0 or later. The default board is MK1B:
```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```

Any other board builds in its own directory:
```bash
cmake -B build-mk1c -DPYRO_BOARD=mk1c && cmake --build build-mk1c
```

This produces:
- `_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2` — A/B bootloader
- `pyro_fw_<board>.uf2` — application firmware
- `pyro_fw_c_fota_image.bin` — OTA update image

A local build increments the patch number in `VERSION`.

## Flash Layout
Set by each board's `board.cmake`.

MK1B and the reference template (2 MB):
```
0x000000  Bootloader           36 KB   (pico_fota_bootloader)
0x009000  Info block             4 KB   (swap flags, rollback state)
0x00A000  App Slot A           512 KB   (active firmware)
0x08A000  App Slot B           512 KB   (OTA download target)
0x10A000  LittleFS             984 KB   (config, web files, flight data)
0x200000  End of flash
```

MK1A and MK1C (16 MB):
```
0x000000  Bootloader           36 KB
0x009000  Info block             4 KB
0x00A000  App Slot A          4076 KB
0x405000  App Slot B          4076 KB
0x800000  LittleFS            8192 KB
0x1000000 End of flash
```

## Initial Flash (one-time via BOOTSEL)
1. Hold BOOTSEL, plug in USB
2. Copy `pico_fota_bootloader.uf2` to the Pico drive
3. Hold BOOTSEL again
4. Copy `pyro_fw_<board>.uf2` to the Pico drive
5. Upload web files: `./support/upload_www.sh`

## Flashing via picotool
After the initial flash, use picotool for all subsequent flashing (no BOOTSEL button needed):
```bash
./support/flash_picotool.sh [build_dir]
```
This forces BOOTSEL via the vendor reset interface, loads both bootloader and app, and reboots. It then pings 192.168.7.1 for 15 s; a board on another subnet is up but reported as not responding.

## OTA Firmware Updates
For routine updates without reflashing the bootloader:
```bash
./support/upload_fw.sh [path_to_bin] [host]
```
Or use the "Firmware Update" button in the web interface at http://pyro.local/. The image is not checked against the board: send one built for it.

The A/B bootloader ([pico_fota_bootloader](https://github.com/JZimnol/pico_fota_bootloader)) provides:
- **Safe updates** — new firmware is written to the inactive slot while the device keeps running
- **Automatic rollback** — if new firmware doesn't call `pfb_firmware_commit()`, the bootloader reverts on next reboot; the firmware commits once it has started USB, networking and the filesystem
- **No bricking** — a failed or interrupted OTA leaves the current firmware intact

## Web Interface
Connect the board via USB. It appears as a network adapter (RNDIS on Windows, ECM on macOS and Linux) with DHCP.

- **Address:** http://pyro.local/, or http://192.168.N.1/, where N is the last octet of the board's MAC (`subnet` on `/api/status`). Each board has its own /24, so several can be attached at once.
- **Status tab:** live state, altitude, speed, pressure, pyro channels, USB and test mode
- **Config tab:** guided pyro editor, logging plan with the flight time it holds, pyro release, buzzer pad and ground test switch
- **Flight Data tab:** summary, CSV download, erase, altitude graph
- **Beep Codes tab:** the personalities, with an audition button
- **Lua tab:** Lua pads, the program editor with check, export and import, and the console
- **Update tab:** firmware and web upload, GitHub release checker
- **Test mode:** with it on, a board on USB detects a launch, fires and beeps as on battery. It lives in RAM, so every boot starts with it off, and it cannot change from launch to landing (USB-08, DD-038).
- **APIs** (all CORS enabled):
  - `GET /api/status`, `/api/net` (network counters, WEB-API-13), `/api/pressure/trace` (the last 256 conversions, DD-063), `/api/log/space`, `/api/flight.csv`, `/api/pins/caps`, `/api/lua/console`
  - `GET`/`POST /api/config`, `/api/pins`, `/api/beeps`, `/api/lua/script`
  - `POST /api/beeps/play`, `/api/lua/check`, `/api/flight/erase`, `/api/test_mode/on`, `/api/test_mode/off`, `/api/reboot`, `/api/ota`, `/api/serial`, `/www/<file>`
  - `GET /<path>` serves any littlefs file

The API and USB stay live in flight; only the flight log touches the filesystem then, and a request that needs a file is answered 423 (DD-058). Config and pins are taken in PAD_IDLE only (409 otherwise).

### Network Architecture
Each tracker advertises a `_pyro._tcp` DNS-SD service via mDNS. A board whose name is taken renames itself `pyro-1`, `pyro-2`, and so on.

**Single device:** Browse to http://pyro.local/ — just works.

**Multiple devices:** A data collection server browses for `_pyro._tcp` to discover all attached trackers automatically.

A frame the USB endpoint cannot take yet is held, eight deep, and sent as it frees (DD-070).

## Testing

The host suites (`test/README.md`) and the Playwright web UI suite (`test/web/run_web_tests.sh`) run in CI on every push, with cppcheck, clang-format, pmccabe, `support/trace_check.py`, `support/wait_check.py` and `support/prove_core0.py`.

```bash
cd build && ninja host_tests integration_tests closedloop_tests
```

On a live board:

```bash
# Network, HTTP and mDNS; interactive mode guides a new user
python3 support/test_network.py

# The same against one board
python3 support/test_network.py pyro.local

# Full suite with UART monitoring and log file
python3 support/test_network.py --all --uart /dev/tty.usbmodem201202 --log test.log

# Analyze a log file from a remote user
python3 support/test_network.py --analyze test.log

# Every HTTP route, and the server against a byte stream
python3 support/api_check.py pyro.local
python3 support/http_stream_check.py pyro.local
```

See [support/README.md](support/README.md) for full documentation.

## Support Tools

All development and deployment scripts are in the `support/` directory:

| Script | Purpose |
|--------|---------|
| `support/test_network.py` | Network/API test suite with TUI |
| `support/api_check.py` | Bench check of every HTTP route (erases the flight log) |
| `support/http_stream_check.py` | The HTTP server against awkwardly split requests |
| `support/pressure_trace.py` | Judge a board's sensor from `/api/pressure/trace` |
| `support/noise_baseline.py` | A board's pressure noise from `/api/status` |
| `support/flash_picotool.sh` | Flash via picotool (no BOOTSEL button) |
| `support/upload_fw.sh` | OTA firmware update (local build) |
| `support/upload_www.sh` | Upload web files |
| `support/update_from_release.py` | Update firmware from GitHub releases |
| `support/register_board.py` | Record attached boards in `boards/BOARD_REGISTRY.json` |
| `support/trace_check.py` | Requirement, decision and function names agree with the code |
| `support/wait_check.py` | No sleep or busy-wait in the firmware (DD-053) |
| `support/prove_core0.py` | Core1 cannot hang core0; the MS5607 handler is RAM-only |

## Releasing

Releases are automated via GitHub Actions:

- **Every push to main or lua-all-boards** builds MK1B, MK1C, MK1A and the reference template, runs the tests and uploads MK1B's artifacts
- **Git tags** (`v*`) create a GitHub Release with downloadable binaries

To create a release:
```bash
# Set version in VERSION file
echo "2.0.0" > VERSION
git add VERSION && git commit -m "release: v2.0.0"
git tag v2.0.0
git push && git push --tags
```

GitHub Actions will build the default board, MK1B, and publish `pyro_fw_mk1b.uf2`, `pyro_fw_c_fota_image.bin`, `pico_fota_bootloader.uf2` and `pyro-mk1b-support.zip` as release assets.

## Self-Update from GitHub

Update a device to the latest release directly from GitHub. Releases carry MK1B's image, so this is for an MK1B:
```bash
# Check for updates
python3 support/update_from_release.py --check --host pyro.local

# Update to latest
python3 support/update_from_release.py --host pyro.local

# Update to specific version
python3 support/update_from_release.py --host pyro.local --version 2.0.0
```

Without `--host` the tool uses 192.168.7.1. It checks the device's current version, compares with GitHub releases, downloads the OTA binary, pushes it to the device, and verifies the update.

## Development Status

Current work, open decisions and bench checks still owed are tracked in [docs/outstanding_tasks.md](docs/outstanding_tasks.md). Among them: MK1B cannot sense continuity until its U5 is changed (B-U5), MK1C's bench fire into a dummy load (F1), and the ground test procedure on the bench (GT-1).

- [REQUIREMENTS.md](REQUIREMENTS.md) and [TRACEABILITY.md](TRACEABILITY.md) - requirements and the tests that verify them
- [DECISIONS.md](DECISIONS.md) - design decisions, DD-001 to DD-071
- [IMPLEMENTATION.md](IMPLEMENTATION.md) and [ARCHITECTURE_V2.md](ARCHITECTURE_V2.md) - how the firmware is built
- [docs/flight_states.md](docs/flight_states.md) - the state machine
- `boards/<name>/THEORY_OF_OPERATION.md` - each board
- [SPECIFICATION.md](SPECIFICATION.md) - a summary for resuming work
- [STATUS.md](STATUS.md) - a record of findings to 2026-09-24

## License
See LICENSE file for details.
