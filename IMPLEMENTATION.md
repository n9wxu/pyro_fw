# Flight Controller Implementation

## Architecture

### Event-Driven State Machine
The flight computer uses a transition table that defines every possible state change:

```
{ from_state, event, to_state, action }
```

Three components:
- **Detectors**: One per state, in a table indexed by state. Read sensors, do per-tick work, return an event or SEVT_NONE.
- **Actions**: One-time side effects at transitions (logging, buzzer, pyro firing).
- **Engine**: `dispatch_state()` calls the detector → looks up (from, event) in the table → calls the action → returns the new state. A state out of range returns PAD_IDLE.

### Transition Table
The table is `transitions[]` in `src/flight_states.c`; `docs/flight_states.md` has the diagram and every state in detail. BOOT_SENSOR, FAULT and GROUND_TEST are numbered after LANDED, so no recorded state number moves.
```
BOOT_SETTLE     → BOOT_SENSOR      on SEVT_TIMER            (2.5 s settle)
BOOT_SENSOR     → BOOT_CONTINUITY  on SEVT_DONE             (sensor answered, filesystem mounted)
BOOT_SENSOR     → ASCENT / FALLING on SEVT_RECOVER_*        (brownout recovery, FLT-BROWN-02)
BOOT_SENSOR     → FAULT            on SEVT_FAULT
BOOT_CONTINUITY → BOOT_CALIBRATE   on SEVT_DONE
BOOT_CONTINUITY → GROUND_TEST      on SEVT_GROUND_TEST      (switch held at power-up, GND-TEST-05)
BOOT_CALIBRATE  → PAD_IDLE         on SEVT_CAL_DONE         (10 readings averaged)
BOOT_CALIBRATE  → FAULT            on SEVT_FAULT            (no samples in 10 s)
PAD_IDLE        → ASCENT           on SEVT_LAUNCH           (100 ft and 5 m/s)
ASCENT          → ASCENT           on SEVT_ARMED            (arming gate, DD-017)
ASCENT          → FALLING          on SEVT_APOGEE           (the fit shows apogee while armed)
FALLING         → DROGUE_DESCENT / CHUTE_DESCENT  on SEVT_DROGUE / SEVT_CHUTE (a settled descent rate)
DROGUE_DESCENT  → CHUTE_DESCENT on SEVT_CHUTE; → FALLING on SEVT_FREEFALL
FALLING / DROGUE_DESCENT / CHUTE_DESCENT → LANDED on SEVT_LANDING
```

### State Transition Criteria

**PAD_IDLE → ASCENT:** Filtered altitude above 100 ft with vertical speed above 5 m/s, held together for 100 ms (FLT-LAUNCH-01, FLT-LAUNCH-07), and no USB host attached unless test mode is on (USB-01, USB-08). T+0 is backdated to the first sample above 50 cm (FLT-LAUNCH-03).

**ASCENT → FALLING:** Clean fits show the pressure rising for 60 ms, and the fitted pressure has risen to 1.0001 times the lowest a clean fit showed, while the pyros are armed and no Mach lock stands (FLT-APO-01, FLT-MACH-05, DD-048); or the lock's fallback, once clean fits show the rocket falling back past where the lock went up (FLT-MACH-04). The pyros arm once the peak speed has passed 10 m/s and the speed has fallen back below it, above about 30 m (DD-017, FLT-MACH-06). The Mach lockout is described in `docs/mach_lockout.md` (DD-049).

**Descent:** the phase is read from the descent rate settling in a band, never from a firing command (DD-023).

**→ LANDED:** All three conditions hold for 1 second, from any descent state:
- Altitude change < 1m between samples
- Vertical speed < 2 m/s
- Altitude < 30m AGL

Or the landing timeout: `landing_timeout` (60 s) after apogee, and still -- under 2 m/s for 1 s on a sensor that has not failed (FLT-LAND-07).

**BOOT_CONTINUITY → GROUND_TEST:** `hal_ground_test_asserted()` held through the last 500 ms of BOOT_SETTLE (GND-TEST-05, DD-071). A board recovering a flight never takes it.

### Hardware Abstraction Layer
Flight logic files (`flight_states.c`, `telemetry_formatter.c`, `buzzer.c`) contain zero platform-specific code. All hardware interaction goes through `hal.h`:

| HAL Function | Purpose |
|---|---|
| `hal_time_ms()` | Current time |
| `hal_pressure_init()`, `hal_pressure_sensor()` | Sensor bring-up and its result; samples reach the pressure layer from `hal_tasks_tick()` |
| `hal_pyro_init()`, `hal_pyro_sample()`, `hal_pyro_get()`, `hal_pyro_fire()`, `hal_pyro_update()`, `hal_pyro_fault()` | Pyro channels |
| `hal_buzzer_init()`, `hal_buzzer_tone_on()`, `hal_buzzer_tone_off()` | Buzzer |
| `hal_telemetry_send()` | UART output |
| `hal_fs_open()`, `hal_fs_write()`, `hal_fs_close()`, `hal_fs_read_file()`, `hal_fs_write_file()` | Filesystem |
| `hal_config_load()`, `hal_config_save()` | `config.ini` |
| `hal_log_start()`, `hal_log_sample()`, `hal_log_stop()` | Flight log |
| `hal_ground_test_asserted()` | Ground test switch |
| `hal_tasks_tick()` | The autonomous tasks: pressure sensor, buzzer, ground test switch |

Three implementations: `src/hal_common/hal_common.c` with each board's files in `boards/<name>/` (Pico), `test/hal_test.c` (mocks), `boards/sim/hal_sim.c` (simulation).

### Main Loop
`src/main_hardware.c` runs one iteration every 20 ms (`src/loop_period.h`, DD-065). In order: `hal_tasks_tick()` (the pressure sensor first, so the MS5607 is commanded at a steady offset); `hal_platform_service()` (TinyUSB, lwIP and the HTTP transport, which only moves bytes); `dispatch_state()`; `flight_update_outputs()`; Lua's service; then the flash window, where `hal_flash_service()` and `flight_flash_service()` do every flash write. The rest of the period is slack: `net_service()` and HTTP work units (`http_server_work()`), each started only with its budget left (DD-061). `lua_app_dispatch()` then grants core1 its slice, which may begin with HTTP units that touch only their own connection (WEB-HTTP-07). Nothing sleeps (DD-053). The watchdog is twice `PYRO_LOOP_WORST_MS`.

Pads are owned once: `pin_store_claim_pads()` gives each pad one owner, and `pyro_release_claim()` and `lua_iface_publish()` install operations only for pads they could claim (DD-020).

### Pyro Fault Detection
- `hal_pyro_fault(channel)`: MK1B reads its AP2192 FLAG pins (GPIO 17/18, active-low with pull-ups); MK1C reports its latched bus faults; MK1A has no fault output
- Post-fire continuity verification: ADC re-check 500-600 ms after each fire (PYR-VERIFY-01)
- Fault events: EVT_PYRO1_FAULT, EVT_PYRO2_FAULT (overcurrent during fire)
- Verify events: EVT_PYRO1_NOPEN, EVT_PYRO2_NOPEN (pyro didn't open after fire)
- Refusals: EVT_PYRO1_REFUSED, EVT_PYRO2_REFUSED (the board energised nothing, PYR-FIRE-01)

### Key Source Files
| File | Purpose |
|---|---|
| `src/flight_states.c` | State machine, detectors, actions, the ring's CSV export |
| `src/flight_states.h` | Types, context struct, transition table types |
| `src/config.c`, `src/config_fields.h` | `config.ini`: X-macro table, parser, serializer |
| `src/telemetry_formatter.c` | $PYRO NMEA and JSON formatting |
| `src/buzzer.c`, `src/beep_codes.c`, `src/beep_store.c` | Non-blocking beep sequencer; outcomes and personalities |
| `src/hal.h` | Hardware abstraction interface |
| `src/hal_common/hal_common.c` | Pico SDK HAL implementation, shared by every board |
| `src/pressure_processing.c` | Pressure filter, altitude, ground reference |
| `src/pressure_fit.c` | The quadratic fit the detectors read |
| `src/ms5607_oneshot.c`, `src/bmp280_driver.c` | The sensors' per-loop conversions |
| `src/flight_log.c`, `src/log_plan.c` | Binary flight log, CSV rendering, logging plans |
| `src/flash_window.c` | The window every flash write runs in |
| `src/http_server.c`, `src/http_conn.c`, `src/http_work.c` | HTTP routes, connections, work units |
| `src/net_glue.c`, `src/net_txq.c` | USB network, lwIP, mDNS, held frames |
| `src/ground_test.c`, `src/ground_test_seq.c`, `src/ground_test_switch.c` | Serial ground test; the switch procedure |
| `src/pin_assign.c`, `src/pin_store.c`, `src/pad_claim.c` | `pins.ini` and pad ownership |
| `src/lua/` | Lua on core1 |
| `src/main_hardware.c` | Hardware main loop |
| `sim/main_sim.c` | Simulation black box (WASM target) |
| `boards/sim/hal_sim.c` | Simulation HAL |
| `sim/physics.c` | Shared physics engine |
| `sim/sim_cli.c` | CLI physics driver |

### Flash Layout
MK1B (2 MB):
```
0x000000  Bootloader           36 KB
0x009000  Info block             4 KB
0x00A000  App Slot A           512 KB
0x08A000  App Slot B           512 KB
0x10A000  LittleFS             984 KB
```
MK1A and MK1C (16 MB): two 4076 KB slots from 0x00A000 and 0x405000, and 8 MB of littlefs from 0x800000. Each board's `board.cmake` sets the geometry.

### HTTP API
| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | JSON device state |
| GET | `/api/net` | lwIP pools, TCP connections, transport refusals (WEB-API-13) |
| GET | `/api/pressure/trace` | The last 256 conversions, binary (SNS-PRES-13) |
| GET | `/api/log/space` | Room for the next flight's log (WEB-API-12) |
| GET | `/api/flight.csv` | The flight log, rendered as CSV |
| POST | `/api/flight/erase` | Erase the flight log |
| GET/POST | `/api/config` | Config INI file; a POST is merged |
| GET/POST | `/api/pins` | `pins.ini` |
| GET | `/api/pins/caps` | The board's capability table and its vocabulary |
| GET/POST | `/api/beeps` | Beep personalities; `POST /api/beeps/play` auditions one |
| GET/POST | `/api/lua/script` | The Lua program; `POST /api/lua/check` validates one against the configured resources, `GET /api/lua/console` reads core1's output |
| POST | `/api/test_mode/on`, `/api/test_mode/off` | Test mode (USB-08) |
| POST | `/api/serial` | The board's MAC, into `serial.txt` |
| POST | `/api/reboot` | Restart device |
| POST | `/api/ota` | Firmware update |
| POST | `/www/<file>` | Upload a web file |
| GET | `/<path>` | Any littlefs file |

The API is live in flight; a request that needs a file gets 423 while the flight log holds the filesystem (DD-058, WEB-API-10).

### Telemetry ($PYRO NMEA)
```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags,p1adc,p2adc,0,0*XX\r\n
```
`telem_rate_hz` (default 10) in ASCENT and the descent states, 1Hz in PAD_IDLE and LANDED. State codes 0-5: PAD_IDLE, ASCENT, FALLING, DROGUE_DESCENT, CHUTE_DESCENT, LANDED. XOR checksum. `telem_format=1` gives JSON.

### Buzzer
- **Pad:** one of four outcomes (ok_to_fly, check_pyro_1, check_pyro_2, system_failure) under the active personality, repeating every 5 s until launch by default; the diagnosis is on `/api/status`
- **USB:** one double chirp on attach, and no announcement while a host is attached (USB-02, USB-03)
- **Landing:** Altitude beep-out in configured units, repeats forever
- **Ground test:** its own alert, countdowns, tone and all-clear (GND-TEST-06, GND-TEST-07)

### Flight Log
The flight log (`flight_log.bin` in littlefs) opens at launch and closes at LANDED. It holds binary records (`src/flight_log.h`); `/api/flight.csv` renders them as CSV with `flog_csv_read()` (DD-062). The flight code hands every sample to `hal_log_sample()`, and `log_plan_take()` keeps what `log_rate` asks for: a row a second, that plus every sample within 1 s of an event, or every sample (FLT-LOG-07, DD-064). Records go to a 4 KB RAM buffer.

**Flash stalls the CPU.** The RP2040 executes from flash via XIP, and a sector erase (46-73 ms measured, DD-035) stops both cores fetching from it. So:
- nothing is written until the RAM buffer has filled once or 2 s have passed, which carries the log through the launch-shock window (FLT-LOG-05, DD-027);
- after that the buffer is written, and the file synced once a second, only in core0's flash window between core1 work units (FLT-LOG-06, DD-035), so a flight that never lands keeps its record;
- from launch until the log lets go, every other file request is refused (`HAL_FS_LOCKED`, HTTP 423, DD-058).

The flight context also keeps a ring of the last 64 samples and events; `flight_save_csv()` exports it for the simulator.

### Pressure Filter
Each reading is stamped by its driver at the moment it describes, not when the loop reads it (SNS-PRES-08, DD-046): the MS5607's alarm handler, running from RAM, commands a pressure and a temperature each loop and stamps each conversion's middle (DD-051, DD-066); the BMP280 takes one forced conversion a loop, stamped from its command (DD-067). A conversion a flash erase or program ran beside is discarded (DD-068), and every sensor bus transfer gives up within a bound (DD-069). A reading then passes a median of three, stamped with the middle reading's time, so no single outlier reaches the filter (SNS-PRES-07, DD-040). `pp_filter_pressure()` in `src/pressure_processing.c` is then a first-order IIR with a 500 ms time constant (SNS-PRES-02), its state in Q8 fixed point so it has no dead band (DD-044), initialised to the first reading (SNS-PRES-03). Heights come from the fractional pressure. T+0 is read from the median's own reading, which the filter would delay by its time constant (FLT-LAUNCH-03).

Altitude is the hypsometric formula against the ground reference (SNS-ALT-01), clamped to 0-8000 m (SNS-ALT-02, SNS-ALT-03). The ground reference is a 5 s mean of the filtered pressure. Launch freezes it to the part of that mean from before T+0 (GND-CAL-01..05, GND-CAL-07).

The filter gives what is reported and logged. The detectors read a least-squares quadratic fitted at every sample through the median's output over the last second, against each sample's own time (`src/pressure_fit.c`, SNS-PRES-09, DD-048). Evaluated at the newest sample, it has no lag on a constant acceleration. Its pressure, rate and acceleration become a height, speed and acceleration through the altitude formula's slope at the fitted pressure. A fit is clean when its residuals are what the sensor's noise explains, σ measured on the pad.

### Altitude Limitations
Altitude is clamped at 8000 m (SNS-ALT-02) and at 0 (SNS-ALT-03) where it is reported. Speed and the trigger heights come from the fit, unclamped (SNS-ALT-04), so neither clamp reads as a stopped rocket.

### Ground Test
Serial commands (`STATUS`, `BEEP`, `ARM`, `FIRE`) are read in PAD_IDLE only (GND-TEST-01..04). The switch procedure is `gt_seq_step()` in `src/ground_test_seq.c`, which owns the schedule and fires on it with or without a buzzer; `gt_switch_step()` reads a switch to ground, or across two pads by driving one and requiring the other to follow both ways (GND-TEST-05..12, DD-071).

### Simulation
The `sim/` directory contains a WASM-compilable flight computer black box and a shared physics engine. See `sim/README.md` for architecture and integration guide.

### Testing
- Host suites: see `test/README.md`; every one runs in CI
- Playwright web UI tests in 3 mock server modes, and the browser demo flown from power-on to LANDED
- 4 safety-critical closed-loop tests (no fire without continuity, both channels on one event for a low flight, no fire during ascent, overcurrent)
- Requirements traced to integration/closed-loop tests (TRACEABILITY.md)
- cppcheck with MISRA addon, clang-format, pmccabe complexity in CI
- `support/trace_check.py`, `support/wait_check.py` and `support/prove_core0.py` in CI
- See test/README.md for complete test plan
