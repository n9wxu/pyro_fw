# Pyro MK1 Flight Computer Specification

> **Requirements changed 2026-10-02.** `REQUIREMENTS.md` governs, and its review is recorded in `docs/requirements_review_2026-10-02.md` and DD-080 to DD-090. This file describes the firmware as built; where it differs from the requirements, the requirements are right.

## Summary

### Project Overview
Two-channel rocket flight computer on the RP2040, for three boards: MK1A, MK1B (the default build) and MK1C. Logs flight data to littlefs flash, serves a web dashboard via USB network (RNDIS/ECM), fires two pyrotechnic channels for recovery deployment, outputs $PYRO NMEA telemetry via UART (see docs/ground-station-interface-spec.md), and runs user Lua scripts. OTA firmware updates via A/B bootloader. Open work is in `docs/outstanding_tasks.md`; decisions in `DECISIONS.md` (to DD-090).

### Current Implementation Status
- **Event-driven state machine**: `src/flight_states.c` — a detector per state, a transition table, actions; twelve states (`docs/flight_states.md`). The flight software is the files of `src/flight_sources.txt`, one responsibility each
- **Main loop**: `src/main_hardware.c` — one iteration every 20 ms (`src/loop_period.h`, DD-065); the slack runs USB, lwIP and HTTP work units
- **HTTP web interface**: `src/http_server.c`, `src/http_conn.c`, `src/http_work.c` + `src/net_glue.c` — handlers run as work units in the slack or on core1 (DD-061); dashboard at pyro.local / 192.168.N.1
- **USB composite device**: ECM/RNDIS network + vendor reset (picotool support); a frame the endpoint cannot take yet is held (`src/net_txq.c`, DD-070)
- **mDNS/DNS-SD**: pyro.local hostname, _pyro._tcp service discovery
- **OTA updates**: A/B bootloader (pico_fota_bootloader), web UI + CLI + GitHub release update
- **Pressure sensors**: `src/pressure_collector.c` collects from either sensor, free-running from an alarm handler in RAM (DD-093); `src/ms5607_driver.c` (MK1B, MK1C) and `src/bmp280_driver.c` (MK1A) hold each part's detection, description and arithmetic. Bring-up in `src/pressure_single_sensor.c`, or `boards/mk1b/pressure_board.c` for MK1B's two pads
- **Pressure processing**: `src/pressure_estimator.c` (one Kalman filter on the raw readings: pressure, its rate, its acceleration, DD-085), `src/pressure_processing.c` (samples, gaps, a stuck sensor), `src/ground_reference.c` and `src/atmosphere.c` (the 1976 standard atmosphere)
- **Fire rules**: `src/fire_control.c` and `src/fire_plan.c` — each channel's trigger, re-fire, emergency fire, and the gap between pulses (DD-082, DD-084)
- **Resume**: `src/flight_resume.c` — the pad record `pad.mkr` and the decision to rejoin a flight after any restart (DD-086)
- **Pyro backends**: `boards/<board>/pyro_board.c` behind `src/pyro.h`; MK1C's split into measure, faults and sequence files, with its PIO arm pump
- **littlefs driver**: `src/littlefs_driver.c` — 984 KB on MK1B, 8 MB on MK1A and MK1C
- **Flight log**: `src/flight_log.c` (binary records, CSV on read) and `src/log_plan.c` (the `log_rate` plans), written by `src/hal_common/hal_common.c` through `src/flash_op.c`
- **Pin assignment**: `pins.ini` (`src/pin_assign.c`, `src/pin_store.c`), each board's `pin_caps.h`, one owner per pad (`src/pad_claim.c`, DD-020)
- **Lua**: `src/lua/` — Lua 5.4, on MK1A, MK1B and MK1C (`PYRO_HAS_LUA` in `boards/<board>/board.cmake`)
- **CI/CD**: GitHub Actions build + release pipeline
- **Test suite**: 38 host suites and the Playwright web UI suite, all in CI (`test/README.md`, `scripts/run_host_tests.sh`); `TRACEABILITY.md` is written from the tests' citations by `support/trace_matrix.py`
- **Config parser**: X-macro INI parser (`src/config_fields.h`, `src/config.c`), web config editor
- **Beep codes**: four outcomes, three personalities in `beep.ini` (`src/beep_codes.c`, `src/beep_store.c`); altitude beep-out after landing
- **Telemetry UART**: `src/telemetry.c` — $PYRO NMEA, one message a second; events queued into the next message (DD-088)
- **Event logging**: LAUNCH, ARMED, APOGEE, PYRO1, PYRO2, LANDING; PEAK, re-fire, emergency fire, resume, fault, verify and sensor events
- **Pyro health**: read on the pad for the announcement, and recorded for every pulse; MK1B's AP2192 FLAG pins; MK1C's bus faults. No reading withholds a fire (DD-081)
- **Ground test**: the switch procedure (`src/ground_test_seq.c`, `src/ground_test_switch.c`, DD-087)
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
1. Integers for what is stored and reported (Pa, cm, ms); the estimator and the atmosphere use single-precision float. Every flight comparison is made in pressure (SNS-EST-05)
2. Every flash program or erase runs through `flash_op()`, which parks the other core (DD-074)
3. Apogee is an event, not a state (twelve states; see docs/flight_states.md)
4. T+0 is backdated to the start of the rise, not the detection threshold (FLT-LAUNCH-03)
5. The exec loop is the only clock: nothing sleeps or busy-waits (DD-053)
6. All pyro modes available on both channels
7. The only file deletion is the flight log's erase (`POST /api/flight/erase`)
8. The state machine is a detector table indexed by state and a transition table; an out-of-range state returns PAD_IDLE
9. A pad has one owner, pyro or Lua, and the claim is what installs its operations (DD-020)
10. The API and USB stay live in flight; only the flight log touches the filesystem (DD-058)
11. Never fire early; data is believed; once a fire is decided nothing withholds it (DD-081)
12. Configuration and pin changes take effect at the next start (CFG-10)

### Hardware Pins (MK1B)
The other boards' maps are in `boards/<board>/board_pins.h` and their theories of operation.

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | UART0 TX | Telemetry, 115200 baud, TRRS jack |
| 1 | UART0 RX | Not read: the port takes no commands (TEL-12) |
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
BOOT_SENSOR ──[a flight in progress, any reset cause]──→ ASCENT or FALLING (resume)
BOOT_CONTINUITY ──→ BOOT_CALIBRATE ──[10 samples]──→ PAD_IDLE
BOOT_CONTINUITY ──[ground test switch held at power-up]──→ GROUND_TEST (terminal)
BOOT_CALIBRATE ──[10s, no samples]──→ FAULT (terminal)

PAD_IDLE ──[alt>100ft AND pad speed>5m/s, 100 ms]──→ ASCENT
ASCENT ──[arming gate]──→ ASCENT (self-loop, arms pyros)
ASCENT ──[seen climbing, then seen falling, the estimator explaining the readings]──→ FALLING
FALLING ──[rate settled in drogue band]──→ DROGUE_DESCENT
FALLING / DROGUE_DESCENT ──[rate settled in main band]──→ CHUTE_DESCENT
DROGUE_DESCENT ──[rate above drogue band, 1 s]──→ FALLING
FALLING / DROGUE_DESCENT / CHUTE_DESCENT ──[stable 1s, or descent timeout]──→ LANDED (terminal)
```

Twelve states. **`docs/flight_states.md` is the authority** — it carries the
complete transition table and the thresholds. This summary is a sketch. BOOT_SENSOR, FAULT and
GROUND_TEST are numbered after LANDED, so recorded state numbers never move.

### Ground Test Mode (DD-087)
- Entry: the ground test switch (`pins.ini`: `ground_test=ground|pair`) closed for the last 0.5 s of BOOT_SETTLE; taken after the sensor and pyro health checks (GND-TEST-05). The driven pad of a two-pad switch may be the buzzer's (GND-TEST-12)
- Announced by three long beeps and a pause, repeating; nothing else takes the buzzer (GND-TEST-06)
- Switch opened, after 1 s closed in the mode: a 5 s countdown → fire pyro 1 → 3 s tone → a 5 s countdown → fire pyro 2 → three long beeps, once (GND-TEST-07)
- A channel with mode none, or released to Lua, is skipped (GND-TEST-08); the switch closed again during a countdown or the tone stops the procedure (GND-TEST-10)
- A fire is delivered on command: no health reading withholds it (GND-TEST-13)
- Terminal until the next power-up: no launch detection, no flight log (GND-TEST-11)
- A board resuming a flight keeps flying whatever the switch says

### PAD_IDLE
- A pressure every 20 ms loop, 50 a second (FLT-RATE-01)
- Ground reference: follows the weather, excluding samples more than 50 Pa from it, and moves to a new pad after 5 s still (GND-CAL-01, GND-CAL-03, GND-CAL-06)
- Launch: altitude above 100 ft and speed above 5 m/s on the filtered state, with no hold time, and no USB host attached unless test mode is on (FLT-LAUNCH-07, USB-01)
- On launch: T+0 is the start of the rise (FLT-LAUNCH-03); the reference freezes to the pad before T+0 (GND-CAL-04)
- Pyro health is read every second and the announcement re-derived: general fault, pyro 1, pyro 2 or OK to fly, in that priority (BUZ-CODE-02); it repeats until launch (BUZ-02)
- After 10 s the pad record is stored for a resume (FLT-BROWN-01)

### ASCENT
- Thrust is reported while the filtered acceleration is upward (FLT-ASC-03)
- Arm pyros once the speed has passed 10 m/s and fallen back below it, above about 30 m (DD-017, FLT-ASC-08)
- Apogee: the obeyed estimator seen climbing and then seen falling, each by more than three times its rate's uncertainty, with its model explaining the readings throughout; a fall whose climb was not seen that way must last 2 s (FLT-APO-07, DD-092)
- Record peak altitude and apogee time

### DESCENT (FALLING, DROGUE_DESCENT, CHUTE_DESCENT)
- The fire rules run every sample (`fire_control_step()`): a channel fires when its trigger is met, fires again every `refire_interval` while the descent is faster than its re-fire speed (PYR-REFIRE-01), and every enabled channel fires while it is faster than `emergency_fire_speed` (FLT-EMRG-01). Speeds are judged as the pad's air would give them (FLT-AIR-01)
- Pulses never overlap; `fire_gap` of quiet time separates them; a first fire goes before a re-fire and pyro 1 before pyro 2 (PYR-DEPLOY-02)
- The phase is the descent rate settling in a band: 10-35 m/s DROGUE_DESCENT, 10 m/s or less CHUTE_DESCENT (FLT-DESC-01)
- Every pulse is recorded with what the board observed: energised or not, a fault, and whether the channel then read open (PYR-FIRE-01, PYR-VERIFY-01)
- Landing: speed < 2 m/s and height < 30 m, for 1 s (FLT-LAND-02, FLT-LAND-03); or `landing_timeout` (60 s) of descent and still for 1 s (FLT-LAND-07)

### LANDED
- The flight log closes and the pad record is cleared (FLT-BROWN-04)
- Beep max altitude in the configured units (cm, m or ft), repeating (BUZ-03, BUZ-04); not while a USB host is attached (USB-02)
- Telemetry continues at 1 Hz (TEL-03)

## Pyro Firing Modes (both channels)

| Mode | Config | Parameter | Unit | Condition |
|------|--------|-----------|------|-----------|
| 0 | none | — | — | never fires |
| 1 | fallen | distance | configured units | this far below the peak |
| 2 | agl | altitude | configured units | descending through this height above the pad |
| 3 | speed | speed | configured units/s | descent faster than this |
| 4 | delay | time | seconds | time since apogee >= value |

Fires only after apogee (PYR-SAFE-04), and never both at the same instant (PYR-DEPLOY-02). Health withholds no fire (PYR-HEALTH-01). Each trigger is converted to a pressure once, against the pad, and compared with the filtered state (SNS-EST-05). MK1A and MK1B fire a non-blocking 500 ms pulse; MK1C runs its firing sequence a loop step at a time and closes its gate at the deadline whatever the bus has (DD-056, PYR-ARM-03). No fire is refused (PYR-FIRE-01).

## Vertical Speed
The filtered rate of ln(p), times the atmosphere's scale height at that pressure (FLT-ASC-02, DD-085).

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
pyro1_refire_speed=0
pyro2_refire_speed=0
emergency_fire_speed=0
refire_interval=0
fire_gap=0
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
| pyro1_refire_speed, pyro2_refire_speed | units/s; 0 off | 0 |
| emergency_fire_speed | units/s; 0 off | 0 |
| refire_interval | ms; 0 takes the board's default (1000) | 0 |
| fire_gap | ms of quiet time between pulses; 0 takes the board's default (3000) | 0 |
| log_rate | 1hz/events/full | 1hz |
| landing_timeout | seconds | 60 |
| lua_enabled | true/false | false |
| lua_baud | a Lua serial role's baud | 9600 |
| lua_pixels | LED string length | 0 |

Pyro values and altitude reporting use the selected units. Speeds use units/second (cm/s, m/s, or ft/s). `refire_interval` and `fire_gap` are held to the range the board declares; a value outside it is brought in and reported, and `GET /api/limits` serves the ranges (PYR-BOARD-01..03). A change takes effect at the next start (CFG-10). The hardware map is `pins.ini` and the beep personalities `beep.ini`.

## Flight Sample
The flight context's ring, the last 64 samples, for `flight_save_csv()`:
```c
typedef struct {
    uint32_t time_ms;
    int32_t  pressure_pa;
    int32_t  altitude_cm;
    uint8_t  state;
    uint8_t  under_thrust;
    uint8_t  event;
} flight_sample_t;
```

The flight log on disk is `flight_log.bin` (`src/flight_log.h`): the magic `PYL1`, a header record, then 22-byte sample records (time, pressure, altitude, raw pressure, temperature in tenths of a degree, state, thrust, event) and text rows (DD-062).

## Telemetry Phase Codes
The `$PYRO` state field follows the ground-station contract (TEL-07, `docs/ground-station-interface-spec.md`):

| Code | State |
|------|-------|
| 0 | PAD_IDLE |
| 1 | ASCENT (thrust flag 1 under power, 0 coasting) |
| 2 | FALLING |
| 3 | DROGUE_DESCENT |
| 4 | CHUTE_DESCENT |
| 5 | LANDED |

Boot states and GROUND_TEST send no `$PYRO`; FAULT sends `!FAULT` every 5 s (TEL-05). The port accepts no commands (TEL-12).

## Beep Codes
| Outcome | Default (Eggtimer) | Meaning |
|---------|--------------------|---------|
| ok_to_fly | rapid chirp | Sensor, storage and every enabled pyro channel good |
| check_pyro_1 | 5 beeps | Pyro 1 open or shorted |
| check_pyro_2 | 4 beeps | Pyro 2 open or shorted |
| general_fault | 2 beeps | Sensor or storage failure: not fixable at the rocket |

Three personalities in `beep.ini`; each sets every outcome's sound and the cadence. One outcome is announced at a time: general fault, then pyro 1, then pyro 2 (BUZ-CODE-02). The announcement repeats every 5 s until launch by default. The diagnosis is `faults[]` on `/api/status`.

Altitude beep-out: a 2 s pause, a 500 ms beep, then each digit as that many beeps, zero as ten, repeating.

## Continuity Thresholds (12-bit ADC)
MK1A and MK1B, with the shared low side on (DD-059):

| ADC | Condition |
|-----|-----------|
| > 3000 | Open circuit |
| < 500 | Good, unless the low-side-off reading shows a short |
| 500-3000 | Neither; the count is reported |

With the low side off, a channel still low is shorted. MK1B as built cannot judge a channel: its AP2192A discharges the sense nodes (task B-U5), so it reports each ready (PYR-HEALTH-01).

MK1C reads presence as a ratio: a channel is present when it reads at least half the biased bus in the same test, and the test says nothing when the bus stays under 200 counts (DD-054, DD-055).
