# Pyro MK1 Flight Computer Specification

## AI Restart Summary

This section contains everything needed to resume development with a new AI session.

### Project Overview
Dual-deployment rocket flight computer on the RP2040, for three boards: MK1A, MK1B (the default build) and MK1C. Logs flight data to littlefs flash, serves a web dashboard via USB network (RNDIS/ECM), fires two pyrotechnic channels for parachute deployment, outputs $PYRO NMEA or JSON telemetry via UART (see docs/ground-station-interface-spec.md), and runs user Lua scripts on core1. OTA firmware updates via A/B bootloader. Open work is in `docs/outstanding_tasks.md`; decisions in `DECISIONS.md`.

### Current Implementation Status
- **Event-driven state machine**: `src/flight_states.c` — a detector per state, a transition table, actions; twelve states (`docs/flight_states.md`)
- **Main loop**: `src/main_hardware.c` — one iteration every 20 ms (`src/loop_period.h`, DD-065); the slack runs USB, lwIP and HTTP work units
- **HTTP web interface**: `src/http_server.c`, `src/http_conn.c`, `src/http_work.c` + `src/net_glue.c` — handlers run as work units in the slack or on core1 (DD-061); dashboard at pyro.local / 192.168.N.1
- **USB composite device**: ECM/RNDIS network + vendor reset (picotool support); a frame the endpoint cannot take yet is held (`src/net_txq.c`, DD-070)
- **mDNS/DNS-SD**: pyro.local hostname, _pyro._tcp service discovery
- **OTA updates**: A/B bootloader (pico_fota_bootloader), web UI + CLI + GitHub release update
- **Pressure sensors**: `src/ms5607_driver.c` + `src/ms5607_oneshot.c` (MK1B, MK1C: a pressure and a temperature every loop, read by an alarm handler in RAM, DD-051, DD-066); `src/bmp280_driver.c` (MK1A: one forced conversion a loop, DD-067). Bring-up in `src/pressure_single_sensor.c`, or `boards/mk1b/pressure_board.c` for MK1B's two pads
- **Pressure processing**: `src/pressure_processing.c` (median of three, IIR, ground reference) and `src/pressure_fit.c` (the quadratic fit the detectors read, DD-048)
- **Pyro backends**: `boards/<board>/pyro_board.c` behind `src/pyro.h`; MK1C's split into measure, faults and sequence files, with its PIO arm pump
- **littlefs driver**: `src/littlefs_driver.c` — 984 KB on MK1B, 8 MB on MK1A and MK1C
- **Flight log**: `src/flight_log.c` (binary records, CSV on read) and `src/log_plan.c` (the `log_rate` plans), written by `src/hal_common/hal_common.c` inside the flash window (`src/flash_window.c`)
- **Pin assignment**: `pins.ini` (`src/pin_assign.c`, `src/pin_store.c`), each board's `pin_caps.h`, one owner per pad (`src/pad_claim.c`, DD-020)
- **Lua**: `src/lua/` — Lua 5.4 on core1, on MK1A, MK1B and MK1C (`PYRO_HAS_LUA` in `boards/<board>/board.cmake`)
- **CI/CD**: GitHub Actions build + release pipeline
- **Test suite**: 29 host suites and the Playwright web UI suite, all in CI (`test/README.md`)
- **Config parser**: X-macro INI parser (`src/config_fields.h`, `src/config.c`), web config editor
- **Beep codes**: four outcomes, three personalities in `beep.ini` (`src/beep_codes.c`, `src/beep_store.c`); altitude beep-out after landing
- **Telemetry UART**: $PYRO NMEA or JSON at `telem_rate_hz` in flight (default 10) / 1 Hz on the ground
- **Event logging**: LAUNCH, ARMED, APOGEE, PYRO1, PYRO2, LANDING; Mach lock, refusal, fault, verify, MAIN_FORCED and sensor events
- **Pyro fault detection**: post-fire continuity verification; MK1B's AP2192 FLAG pins; MK1C's short latches and fire preconditions
- **Ground test**: serial ARM/FIRE commands in PAD_IDLE (`src/ground_test.c`), and the switch procedure (`src/ground_test_seq.c`, `src/ground_test_switch.c`, DD-071)
- **WASM simulation**: Flight computer + physics engine compiled to WebAssembly

### Build
```bash
mkdir -p build && cd build && cmake -G Ninja .. && ninja      # MK1B
cmake -B build-mk1c -DPYRO_BOARD=mk1c && cmake --build build-mk1c
# Flash: ./support/flash_picotool.sh [build_dir]
# OTA: ./support/upload_fw.sh [bin] [host]
# Test: python3 support/test_network.py --all pyro.local
```
Dependencies: Pico SDK 2.2.0, installed separately; littlefs v2.11.2, Unity v2.6.0, Lua v5.4.6 (boards with Lua) and pico_fota_bootloader (master) via FetchContent.

### Key Design Decisions
1. Integers for what is stored and reported (Pa, cm, ms); the altitude formula and the pressure fit use single-precision float
2. Flash is written only in core0's flash window, between core1 work units; the littlefs driver refuses and counts a write outside it (`flash_refusals`, DD-035, `src/flash_window.h`)
3. Apogee is an event, not a state (twelve states; see docs/flight_states.md)
4. T+0 is backdated to the first reading above 50 cm, not the detection threshold (FLT-LAUNCH-03)
5. The exec loop is the only clock: nothing sleeps or busy-waits (DD-053)
6. All pyro modes available on both channels
7. The only file deletion is the flight log's erase (`POST /api/flight/erase`)
8. The state machine is a detector table indexed by state and a transition table; an out-of-range state returns PAD_IDLE
9. A pad has one owner, pyro or Lua, and the claim is what installs its operations (DD-020)
10. The API and USB stay live in flight; only the flight log touches the filesystem (DD-058)

### Hardware Pins (MK1B)
The other boards' maps are in `boards/<board>/board_pins.h` and their theories of operation.

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | UART0 TX | Telemetry, 115200 baud, TRRS jack |
| 1 | UART0 RX | Ground-test commands |
| 6 | I2C1 SDA | BMP280 pad, no pull-up; probed at 100 kHz |
| 7 | I2C1 SCL | Both sensor pads |
| 8 | User pad | J1, Lua |
| 10 | I2C1 SDA | MS5607 pad, 400 kHz |
| 15 | PYRO_COMMON_EN | Q1B gate, the shared low side |
| 16 | Buzzer | LS1, on/off |
| 17 | Pyro 1 fault | AP2192 FLG2, active low |
| 18 | Pyro 2 fault | AP2192 FLG1, active low |
| 21 | Pyro 1 enable | AP2192 EN2 |
| 22 | Pyro 2 enable | AP2192 EN1 |
| 25 | Onboard LED | |
| 26 | Pyro 1 continuity | ADC0, 100k pull-up |
| 27 | Pyro 2 continuity | ADC1, 100k pull-up |

### Memory
```
Flash, MK1B:       2 MB  = bootloader 36 KB + info 4 KB + 2 x 512 KB slots + 984 KB littlefs
Flash, MK1A/MK1C: 16 MB  = bootloader 36 KB + info 4 KB + 2 x 4076 KB slots + 8 MB littlefs
RAM: 264 KB. Fixed buffers include the flight log's 4 KB, its 128-row delay line (3 KB),
     the pressure trace (256 records, 6 KB), lwIP's 8,000-byte heap, and the flight ring (64 samples, 1 KB)
```

---

## Flight States

```
BOOT_SETTLE ──[2.5s]──→ BOOT_SENSOR ──[sensor+fs ok]──→ BOOT_CONTINUITY
BOOT_SENSOR ──[no sensor or no fs]──→ FAULT (terminal)
BOOT_SENSOR ──[power event in flight]──→ ASCENT or FALLING (brownout recovery)
BOOT_CONTINUITY ──→ BOOT_CALIBRATE ──[10 samples]──→ PAD_IDLE
BOOT_CONTINUITY ──[ground test switch held at power-up]──→ GROUND_TEST (terminal)
BOOT_CALIBRATE ──[10s, no samples]──→ FAULT (terminal)

PAD_IDLE ──[alt>100ft AND pad speed>5m/s, 100 ms]──→ ASCENT
ASCENT ──[arming gate]──→ ASCENT (self-loop, arms pyros)
ASCENT ──[armed AND fit shows apogee, no Mach lock]──→ FALLING
FALLING ──[rate settled in drogue band]──→ DROGUE_DESCENT
FALLING / DROGUE_DESCENT ──[rate settled in main band]──→ CHUTE_DESCENT
DROGUE_DESCENT ──[rate above drogue band, 1 s]──→ FALLING
FALLING / DROGUE_DESCENT / CHUTE_DESCENT ──[stable 1s, or descent timeout]──→ LANDED (terminal)
```

Twelve states. **`docs/flight_states.md` is the authority** — it carries the
complete transition table, the exact thresholds, and the dead end this machine
has (ASCENT does not exit unless the arming gate is met or the Mach lock's
fallback declares apogee). This summary is a sketch. BOOT_SENSOR, FAULT and
GROUND_TEST are numbered after LANDED, so recorded state numbers never move.

### Ground Test Mode (DD-071)
- Entry: the ground test switch (`pins.ini`: `ground_test=ground|pair`) closed for the last 0.5 s of BOOT_SETTLE; taken after the sensor and continuity checks (GND-TEST-05)
- Announced by three long beeps and a pause, repeating; nothing else takes the buzzer (GND-TEST-06)
- Switch opened, after 1 s closed in the mode: a 5 s countdown → fire pyro 1 → 3 s tone → a 5 s countdown → fire pyro 2 → three long beeps, once (GND-TEST-07)
- A channel with mode none, or released to Lua, is skipped (GND-TEST-08); the switch closed again during a countdown or the tone stops the procedure (GND-TEST-10)
- Terminal until the next power-up: no launch detection, no flight log (GND-TEST-11)
- A board recovering a flight after a power event keeps flying whatever the switch says

### PAD_IDLE
- A pressure every 20 ms loop, 50 a second (FLT-RATE-01)
- Ground reference: a 5 s rolling mean of the filtered pressure, excluding samples more than 50 Pa from it (GND-CAL-01..03)
- Launch: altitude above 100 ft and speed above 5 m/s, held 100 ms of sample time, and no USB host attached unless test mode is on (FLT-LAUNCH-01, FLT-LAUNCH-07, USB-01)
- On launch: T+0 is the first reading above 50 cm (FLT-LAUNCH-03); the reference freezes to the pad before T+0 (GND-CAL-04)
- Pad announcement repeats until launch (BUZ-02); serial ground-test commands are read here only

### ASCENT
- Thrust is reported while the fit's acceleration is upward (FLT-ASC-03)
- Arm pyros once the speed has passed 10 m/s and fallen back below it, above about 30 m (DD-017, FLT-MACH-06)
- Apogee: armed, no Mach lock, clean fits showing the pressure rising for 60 ms, and the fitted pressure at 1.0001 times its lowest (FLT-APO-01, FLT-MACH-05); or the Mach lock's fallback (FLT-MACH-04)
- Record peak altitude and apogee time

### DESCENT (FALLING, DROGUE_DESCENT, CHUTE_DESCENT)
- Check pyro conditions every sample; a channel whose trigger is met fires once (PYR-SAFE-03)
- The phase is the descent rate settling in a band: 10-35 m/s drogue, 10 m/s or less main (DD-023)
- Post-fire verification from the first continuity check completed after the pulse (PYR-VERIFY-01, DD-080); an emergency ladder re-fires a drogue that did not open and brings the main forward when the drogue is not slowing the rocket (DD-028)
- Landing: altitude change < 1 m between samples, speed < 2 m/s and altitude < 30 m, for 1 s (FLT-LAND-01..03); or `landing_timeout` (60 s) of descent and still for 1 s (FLT-LAND-07)

### LANDED
- The flight log closes; the ring keeps a row a second
- Beep max altitude in the configured units (cm, m or ft), repeating (BUZ-03, BUZ-04); not while a USB host is attached (USB-02)
- Telemetry continues at 1 Hz

## Pyro Firing Modes (both channels)

| Mode | Config | Parameter | Unit | Condition |
|------|--------|-----------|------|-----------|
| 0 | none | — | — | never fires |
| 1 | fallen | distance | configured units | peak height - fitted height >= value |
| 2 | agl | altitude | configured units | fitted height <= value |
| 3 | speed | speed | configured units/s | fitted downward speed >= value |
| 4 | delay | time | seconds | time since apogee >= value |

Fires only a channel with continuity (PYR-SAFE-01), only after apogee (PYR-SAFE-04), and never both at the same instant (PYR-DEPLOY-02). AGL, FALLEN and SPEED compare the pressure fit (PYR-MODE-05). MK1A and MK1B fire a non-blocking 500 ms pulse; MK1C runs its firing sequence a loop step at a time (DD-056). A refused fire is recorded as a refusal (PYR-FIRE-01).

## Vertical Speed
Taken from the pressure fit, through the slope of the altitude formula at the fitted pressure (FLT-ASC-02, DD-048). Short of a fit, from consecutive samples, each against its own time:
```c
vertical_speed_cms = (altitude_cm - prev_altitude_cm) * 1000 / dt_ms;
```

## Configuration (config.ini)
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

| Parameter | Values | Default |
|-----------|--------|---------|
| id | 8 chars | PYRO001 |
| name | 8 chars | MyRocket |
| pyro1_mode | none/fallen/agl/speed/delay | delay |
| pyro1_value | 0-65535 | 0 |
| pyro2_mode | none/fallen/agl/speed/delay | agl |
| pyro2_value | 0-65535 | 300 |
| units | cm/m/ft | m |
| telem_format | 0 NMEA, 1 JSON | 0 |
| telem_rate_hz | in flight; 0 takes 10, at most 50 | 10 |
| log_rate | 1hz/events/full | 1hz |
| landing_timeout | seconds | 60 |
| lua_enabled | true/false | false |
| lua_baud | a Lua serial role's baud | 9600 |
| lua_pixels | LED string length | 0 |

All internal calculations use cm. Pyro values and altitude reporting use the selected units. Speed mode uses units/second (cm/s, m/s, or ft/s). A pyro altitude setting beyond the sensor's 8000 m is a system failure on the pad. The hardware map is `pins.ini` and the beep personalities `beep.ini`.

## Flight Sample (16 bytes)
The flight context's ring, the last 64 samples, for `flight_save_csv()`:
```c
typedef struct {
    uint32_t time_ms;
    int32_t  pressure_pa;
    int32_t  altitude_cm;
    uint8_t  state;
    uint8_t  under_thrust;
    uint8_t  event;
    uint8_t  event_data;
} flight_sample_t;
```

The flight log on disk is `flight_log.bin` (`src/flight_log.h`): the magic `PYL1`, a header record, then 22-byte sample records (time, pressure, altitude, raw pressure, temperature in tenths of a degree, state, thrust, event) and text rows (DD-062).

## Telemetry Phase Codes
The `$PYRO` state field follows the ground-station contract (TEL-05, `docs/ground-station-interface-spec.md`):

| Code | State |
|------|-------|
| 0 | PAD_IDLE |
| 1 | ASCENT (thrust flag 1 under power, 0 coasting) |
| 2 | FALLING |
| 3 | DROGUE_DESCENT |
| 4 | CHUTE_DESCENT |
| 5 | LANDED |

Boot states and GROUND_TEST send no `$PYRO`; FAULT sends `!FAULT` every 5 s.

## Beep Codes
| Outcome | Default (Eggtimer) | Meaning |
|---------|--------------------|---------|
| ok_to_fly | rapid chirp | Sensor, filesystem and both pyro channels good |
| check_pyro_1 | 5 beeps | Pyro 1 open or shorted |
| check_pyro_2 | 4 beeps | Pyro 2 open or shorted |
| system_failure | 2 beeps | Sensor, filesystem or configuration failure |

Three personalities in `beep.ini`; each sets every outcome's sound, the cadence, and whether pyro faults are split per channel. The announcement repeats every 5 s until launch by default. The diagnosis is `faults[]` on `/api/status`.

Altitude beep-out: a 2 s pause, a 500 ms beep, then each digit as that many beeps, zero as ten, repeating.

## Continuity Thresholds (12-bit ADC)
MK1A and MK1B, with the shared low side on (DD-059):

| ADC | Condition |
|-----|-----------|
| > 3000 | Open circuit |
| < 500 | Good, unless the low-side-off reading shows a short |
| 500-3000 | Neither; the count is reported |

With the low side off, a channel still low is shorted. MK1B as built reads every channel shorted: its AP2192A discharges the sense nodes (task B-U5).

MK1C reads presence as a ratio: a channel is present when it reads at least half the biased bus in the same test, and the test says nothing when the bus stays under 200 counts (DD-054, DD-055).
