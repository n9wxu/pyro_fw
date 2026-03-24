# Pyro MK1B Firmware - Current Status

_Last updated: 2026-03-23 after commit `6fb554a`_

## ✅ Completed

### Event-Driven State Machine
- Transition table: 8 rows define every possible state change
- Detectors (per-tick work) → Events → Actions (side effects)
- 4 boot states + 4 flight states, no implementation leakage
- See IMPLEMENTATION.md for transition table

### Hardware Abstraction Layer
- hal.h: 15 functions covering all hardware interaction
- Zero #ifdef in flight code (flight_states.c, telemetry_formatter.c, buzzer.c)
- Three implementations: hardware, test, simulation
- Same source compiles for Pico, host tests, and WASM

### Config System
- INI parser with 12 unit tests
- Web config editor (all fields: id, name, units, beep, pyro1, pyro2)
- Beep code 4-3 for out-of-range altitude settings
- 8,000m altitude clamp on all altitude-based settings

### Telemetry
- Format 0 (default): `$PYRO` NMEA sentences with XOR checksum, backwards-compatible
- Format 1: JSON newline-delimited objects (selectable via `telem_format=1` in config.ini)
- Event sentences: `$PYRO_APO`, `$PYRO_FIRE`, `$PYRO_LAND` on key flight events
- 10Hz ASCENT/DESCENT, 1Hz PAD_IDLE/LANDED
- `telemetry_formatter.c` — standalone protocol-independent module

### Buzzer
- GPIO on/off, startup chirps + status code × 2
- Altitude beep-out after landing in configured units

### Data Logging
- 4096-sample ring buffer, events tagged on samples
- Streaming CSV export to littlefs after landing
- /api/flight.csv serves actual flight data

### Web Interface
- 4-tab UI: Status, Config, Flight Data, Update
- Pending config warning, range validation, guided editor
- GitHub Pages demo + interactive WASM simulation

### Simulator
- Pyro black box (sim/main_sim.c) — no physics knowledge
- CLI physics driver (sim/sim_cli.c)
- Browser simulation (docs/sim.html) with Web Audio buzzer

### Pyro Fault Detection
- AP2192 FLAG pin monitoring (GPIO 17/18, active-low with pull-ups)
- Post-fire continuity verification (ADC re-check 500ms after fire)
- Fault events logged to flight buffer (EVT_PYRO1_FAULT, EVT_PYRO2_FAULT, EVT_PYRO1_NOPEN, EVT_PYRO2_NOPEN)
- Beep codes 2-3/2-4 (P1 fault/verify) and 3-3/3-4 (P2 fault/verify)

### Ground Test Commands (v2 Task 2) ✅ Done
- Serial commands via TRRS jack (UART0 RX): `BEEP STATUS`, `BEEP ALT <n>`, `ARM <1|2>`, `FIRE <1|2>`, `STATUS`
- 3-second ARM→FIRE window with automatic disarm timeout
- All commands rejected outside PAD_IDLE state
- NMEA-style `$GT,...` responses with XOR checksum (DD-011)
- `last_status_code` stored on each continuity check for BEEP replay

### HAL Config API (v2 Task 1 subset) ✅ Done
- `hal_config_load()` / `hal_config_save()` abstract all config I/O
- Flight software no longer calls `hal_fs_*()` for config
- All three HAL implementations updated (hardware, sim, test)

### CPU Sleep (v2 Task 3) ✅ Done
- `hal_sleep_until_event()` added to all HAL implementations
- Hardware: `__wfe()` — CPU sleeps between events (UART RX, timer, Core1 SEV)
- Main loop calls it at end of each iteration

### Autonomous Pressure Sampling (v2 Task 4) ✅ Done
- `hal_pressure_fifo_start()` / `hal_pressure_fifo_get()` / `hal_pressure_fifo_release()` in hal.h
- Hardware: BMP280 FIFO at 50Hz feeds async batches; MS5607 uses timer-driven polling
- `async_task.h` state machine drives hardware sampling without blocking the main loop
- Test/sim HAL stubs return `false` from `hal_pressure_fifo_get()` (polled mode fallback)

### Telemetry Formatter (v2 Task 5) ✅ Done
- `src/telemetry_formatter.h` / `.c` — protocol-independent event API
- `telemetry_init(cfg)`, `telemetry_state(snapshot)`, `telemetry_apogee()`, `telemetry_pyro_fire()`, `telemetry_landing()`
- Format 0 (NMEA): `$PYRO` + event sentences, field order identical to v1 (no parser breakage)
- Format 1 (JSON): `{"t":"state",...}` objects, selectable by `telem_format=1`
- `flight_states.c` wired: `telemetry_init()` at boot, `flight_update_outputs()` builds snapshot
- `src/telemetry.c` emptied and removed from all build targets

### Testing (76 C + 22 web)
- 39 unit, 22 integration (+4 GND-TEST-01..04, +2 TEL-03..04), 15 closed-loop (35+ flights)
- 4 safety-critical closed-loop tests (no-fire-without-continuity, no-simultaneous-fire, no-fire-during-ascent, overcurrent-fault-detection)
- 22 Playwright web UI tests (3 mock server modes)
- cppcheck/MISRA, clang-format, pmccabe in CI
- Requirements traced to integration tests (TRACEABILITY.md)

### Build & CI
- Firmware, tests, simulator, WASM all from same source
- GitHub Actions on every push
- Auto-versioning, A/B OTA images

## 🔨 Known Issues
- [ ] Playwright reboot cycle test skipped (needs CI log access)
- [ ] Parallel HTTP connections (6+) can drop

## 🚧 In Progress — v2 Architecture
v2 refactors the firmware to autonomous hardware I/O with CPU sleep between 100ms windows.

| Task | Description | Status |
|------|-------------|--------|
| v2-1 | X-macro config system (`config_fields.h`) | ✅ Done |
| v2-2 | HAL config API (`hal_config_load/save`) | ✅ Done |
| v2-2 | Ground test commands (`ground_test.c`) | ✅ Done |
| v2-3 | CPU sleep (`hal_sleep_until_event`) | ✅ Done |
| v2-4 | Serial readline HAL (`hal_serial_readline`) | ✅ Done |
| v2-5 | Autonomous pressure (batch buffers, Core1/DMA) | ✅ Done |
| v2-6 | Telemetry formatter module | ✅ Done |
| v2-7 | Buzzer pattern player (timer ISR) | ❌ Not started |
| v2-8 | Batch flight_process_samples() | ❌ Not started |
| v2-9 | Fire-and-forget hal_log_sample() | ❌ Not started |
| v2-10 | DMA UART TX, USB on Core1/timer ISR | ❌ Not started |

See ARCHITECTURE_V2.md for full task descriptions.

## ❌ Not Implemented
- [ ] Progressive in-flight CSV logging (superseded by v2-9 `hal_log_sample`)
- [ ] Buzzer pattern player (timer ISR) — v2 Task 7
- [ ] Batch `flight_process_samples()` — v2 Task 8
- [ ] `hal_log_sample()` fire-and-forget — v2 Task 9
- [ ] DMA UART TX + USB on Core1 — v2 Task 10
