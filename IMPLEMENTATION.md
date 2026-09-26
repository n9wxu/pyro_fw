# Flight Controller Implementation

## Architecture

### Event-Driven State Machine
The flight computer uses a transition table that defines every possible state change:

```
{ from_state, event, to_state, action }
```

Three components:
- **Detectors**: One per state. Read sensors, do per-tick work, return an event or SEVT_NONE.
- **Actions**: One-time side effects at transitions (logging, buzzer, pyro firing).
- **Engine**: Calls detector → looks up (from, event) in table → calls action → returns new state.

### Transition Table
The table is `transitions[]` in `src/flight_states.c`; `docs/flight_states.md` has the diagram and every state in detail.
```
BOOT_SETTLE     → BOOT_SENSOR      on SEVT_TIMER            (2.5 s settle)
BOOT_SENSOR     → BOOT_CONTINUITY  on SEVT_DONE             (sensor answered, filesystem mounted)
BOOT_SENSOR     → ASCENT / FALLING on SEVT_RECOVER_*        (brownout recovery, FLT-BROWN-02)
BOOT_SENSOR     → FAULT            on SEVT_FAULT
BOOT_CONTINUITY → BOOT_CALIBRATE   on SEVT_DONE
BOOT_CALIBRATE  → PAD_IDLE         on SEVT_CAL_DONE         (10 readings averaged)
BOOT_CALIBRATE  → FAULT            on SEVT_FAULT            (no samples in 10 s)
PAD_IDLE        → ASCENT           on SEVT_LAUNCH           (100 ft and 5 m/s)
ASCENT          → ASCENT           on SEVT_ARMED            (arming gate, DD-017)
ASCENT          → FALLING          on SEVT_APOGEE           (speed ≤ 0 while armed)
FALLING         → DROGUE_DESCENT / CHUTE_DESCENT  on a settled descent rate
DROGUE_DESCENT  → CHUTE_DESCENT on SEVT_CHUTE; → FALLING on SEVT_FREEFALL
FALLING / DROGUE_DESCENT / CHUTE_DESCENT → LANDED on SEVT_LANDING
```

### State Transition Criteria

**PAD_IDLE → ASCENT:** Filtered altitude above 100 ft with vertical speed above 5 m/s, held together for 100 ms (FLT-LAUNCH-01, FLT-LAUNCH-07). T+0 is backdated to the first sample above 50 cm (FLT-LAUNCH-03).

**ASCENT → FALLING:** Vertical speed ≤ 0 for 60 ms while the pyros are armed and the Mach gate is clear (FLT-APO-01, FLT-MACH-01). The pyros arm once the peak speed has passed 10 m/s filtered (about 20 m/s true) and the speed has fallen back below it (DD-017).

**Descent:** the phase is read from the descent rate settling in a band, never from a firing command (DD-023).

**→ LANDED:** All three conditions hold for 1 second, from any descent state:
- Altitude change < 1m between samples
- Vertical speed < 2 m/s
- Altitude < 30m AGL

Or the landing timeout: 60 s after apogee and slower than 5 m/s (FLT-LAND-07).

### Hardware Abstraction Layer
Flight logic files (`flight_states.c`, `telemetry_formatter.c`, `buzzer.c`) contain zero platform-specific code. All hardware interaction goes through `hal.h`:

| HAL Function | Purpose |
|---|---|
| `hal_time_ms()` | Current time |
| `hal_pressure_init/read()` | Pressure sensor |
| `hal_pyro_init/check/fire/update/fault()` | Pyro channels |
| `hal_buzzer_init/tone_on/tone_off()` | Buzzer |
| `hal_telemetry_send()` | UART output |
| `hal_fs_open/read/write/close()` | Filesystem |

Three implementations: `src/hal_common/hal_common.c` with each board's files in `boards/<name>/` (Pico), `test/hal_test.c` (mocks), `boards/sim/hal_sim.c` (simulation).

### Pyro Fault Detection
- `hal_pyro_fault(channel)` reads AP2192 FLAG pins (GPIO 17/18, active-low with pull-ups)
- Post-fire continuity verification: ADC re-check 500ms after fire
- Fault events: EVT_PYRO1_FAULT, EVT_PYRO2_FAULT (overcurrent during fire)
- Verify events: EVT_PYRO1_NOPEN, EVT_PYRO2_NOPEN (pyro didn't open after fire)
- Beep codes: 2-3/3-3 (fault), 2-4/3-4 (verify fail)

### Key Source Files
| File | Purpose |
|---|---|
| `src/flight_states.c` | State machine, detectors, actions, config parser, CSV export |
| `src/flight_states.h` | Types, context struct, transition table types |
| `src/telemetry_formatter.c` | $PYRO NMEA formatting |
| `src/buzzer.c` | Non-blocking beep sequencer |
| `src/hal.h` | Hardware abstraction interface |
| `src/hal_common/hal_common.c` | Pico SDK HAL implementation, shared by every board |
| `src/pressure_processing.c` | Pressure filter, altitude, ground reference |
| `src/main_hardware.c` | Hardware main loop |
| `sim/main_sim.c` | Simulation black box (WASM target) |
| `boards/sim/hal_sim.c` | Simulation HAL |
| `sim/physics.c` | Shared physics engine |
| `sim/sim_cli.c` | CLI physics driver |

### Flash Layout (2MB)
```
0x000000  Bootloader           36 KB
0x009000  Info block             4 KB
0x00A000  App Slot A           512 KB
0x08A000  App Slot B           512 KB
0x10A000  LittleFS             984 KB
```

### HTTP API
| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | JSON device state |
| GET | `/api/config` | Config INI file |
| POST | `/api/config` | Write config |
| POST | `/api/reboot` | Restart device |
| POST | `/api/ota` | Firmware update |
| GET | `/api/flight.csv` | Flight data CSV from littlefs |

### Telemetry ($PYRO NMEA)
```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags,p1adc,p2adc,batt,temp*XX\r\n
```
10Hz during ASCENT/DESCENT, 1Hz otherwise. XOR checksum.

### Buzzer
- **Startup:** 10 chirps → status code × 2 → stop
- **Status codes:** 1-1 good, 2-1/2-2 P1 open/short, 2-3/2-4 P1 fault/verify, 3-1/3-2 P2 open/short, 3-3/3-4 P2 fault/verify, 4-3 config range
- **Landing:** Altitude beep-out in configured units, repeats forever

### Flight Log
The flight log (`flight_log.csv` in littlefs) opens at launch and closes at LANDED. Samples go to a 4 KB RAM buffer.

**Flash stalls the CPU.** The RP2040 executes from flash via XIP, and a sector erase (40-73 ms measured, DD-035) stops both cores fetching from it. So:
- nothing is written until the RAM buffer has filled once, which carries the log through the launch-shock window (FLT-LOG-05, DD-027);
- after that the buffer is written, and the file synced once a second, only in core0's flash window between core1 work units (FLT-LOG-06, DD-035), so a flight that never lands keeps its record.

The flight context also keeps a 4096-entry ring of samples and events (DAT-01); `flight_save_csv()` exports it for the simulator.

### Pressure Filter
Each reading first passes a median of three, stamped with the middle reading's time, so no single outlier reaches the filter (SNS-PRES-07, DD-040). `pp_filter_pressure()` in `src/pressure_processing.c` is then a first-order IIR with a 500 ms time constant (SNS-PRES-02), initialised to the first raw reading (SNS-PRES-03). The state is whole pascals. At 20 ms the step is 3.8 % of the difference, which rounds to zero for any difference under about 26 Pa, so SNS-PRES-04 forces a 1 Pa step instead. Near steady state that makes the filter a rate limiter that passes noise through; T4 in `docs/outstanding_tasks.md` replaces it with a fractional state.

Altitude is the hypsometric formula against the ground reference (SNS-ALT-01), clamped to 0-8000 m (SNS-ALT-02, SNS-ALT-03). The ground reference is a 5 s mean of the filtered pressure, frozen at launch (GND-CAL-01..05).

### Altitude Limitations
Altitude is clamped at 8000 m (SNS-ALT-02) and at 0 (SNS-ALT-03) where it is reported. Speed is taken from the unclamped height (SNS-ALT-04), so neither clamp reads as a stopped rocket.

### Simulation
The `sim/` directory contains a WASM-compilable flight computer black box and a shared physics engine. See `sim/README.md` for architecture and integration guide.

### Testing
- Host suites: see `test/README.md`; every one runs in CI
- Playwright web UI tests in 3 mock server modes
- 4 safety-critical tests (no-fire-without-continuity, no-simultaneous-fire, no-fire-during-ascent, overcurrent)
- Requirements traced to integration/closed-loop tests (TRACEABILITY.md)
- cppcheck with MISRA addon, clang-format, pmccabe complexity in CI
- See test/README.md for complete test plan
