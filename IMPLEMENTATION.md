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
```
BOOT_INIT       → BOOT_SETTLE      on SEVT_DONE
BOOT_SETTLE     → BOOT_CONTINUITY  on SEVT_TIMER     (2.5s wait)
BOOT_CONTINUITY → BOOT_CALIBRATE   on SEVT_DONE
BOOT_CALIBRATE  → PAD_IDLE         on SEVT_CAL_DONE  (10 readings averaged)
PAD_IDLE        → ASCENT           on SEVT_LAUNCH    (altitude > 10m)
ASCENT          → ASCENT           on SEVT_ARMED     (speed < 10 m/s)
ASCENT          → DESCENT          on SEVT_APOGEE    (speed ≤ 0 while armed)
DESCENT         → LANDED           on SEVT_LANDING   (stable + slow + low for 1s)
```

### State Transition Criteria

**PAD_IDLE → ASCENT:** Filtered altitude exceeds 10 meters. Launch time backdated to first sample above 50cm.

**ASCENT → DESCENT:** Vertical speed ≤ 0 while pyros are armed. Pyros arm when speed drops below 10 m/s.

**DESCENT → LANDED:** All three conditions hold for 1 second:
- Altitude change < 1m between samples
- Vertical speed < 2 m/s
- Altitude < 30m AGL

### Hardware Abstraction Layer
Flight logic files (`flight_states.c`, `telemetry.c`, `buzzer.c`) contain zero platform-specific code. All hardware interaction goes through `hal.h`:

| HAL Function | Purpose |
|---|---|
| `hal_time_ms()` | Current time |
| `hal_pressure_init/read()` | Pressure sensor |
| `hal_pyro_init/check/fire/update/fault()` | Pyro channels |
| `hal_buzzer_init/tone_on/tone_off()` | Buzzer |
| `hal_telemetry_send()` | UART output |
| `hal_fs_open/read/write/close()` | Filesystem |

Three implementations: `hal_hardware.c` (Pico), `hal_test.c` (mocks), `hal_sim.c` (simulation).

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
| `src/telemetry.c` | $PYRO NMEA formatting |
| `src/buzzer.c` | Non-blocking beep sequencer |
| `src/hal.h` | Hardware abstraction interface |
| `src/hal_hardware.c` | Pico SDK HAL implementation |
| `src/main_hardware.c` | Hardware main loop |
| `sim/main_sim.c` | Simulation black box (WASM target) |
| `sim/hal_sim.c` | Simulation HAL |
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

### Altitude Limitations
Linear barometric formula clamped at 8,000m. Above that only AGL pyro mode works correctly. See REQUIREMENTS.md for full analysis.

### Simulation
The `sim/` directory contains a WASM-compilable flight computer black box and a shared physics engine. See `sim/README.md` for architecture and integration guide.

### Testing
- 39 unit, 12 integration, 13 closed-loop = 64 C tests
- 22 Playwright web UI tests (3 mock server modes)
- 4 safety-critical tests (no-fire-without-continuity, no-simultaneous-fire, no-fire-during-ascent, overcurrent)
- Requirements traced to integration/closed-loop tests (TRACEABILITY.md)
- cppcheck with MISRA addon, clang-format, pmccabe complexity in CI
- See test/README.md for complete test plan
