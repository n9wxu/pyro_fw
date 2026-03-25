# Session Notes — March 7-8, 2026

## March 7 (Saturday)

### Telemetry Tests
- Added 6 telemetry formatting tests (altitude, speed, thrust flag, ADC, all flags, boot state mapping)
- Total: 27 unit tests

### OpenRocket Integration Tests
- Added open_rocket_export.csv (228 data points, 16s flight, 165ft peak)
- 11 integration tests with 1ms interpolated pressure data
- Binary search interpolation, inverse barometric formula

### Closed-Loop Simulation Tests
- 9 tests, 28 simulated flights (7 pyro configs × 4 altitudes: 100ft to Karman line)
- Physics model: thrust, drag, atmospheric density, standard atmosphere
- Closed-loop: pyro fires → chute deployment → changed descent rate
- Flight summaries printed in build logs

### Firmware Bugs Found by Tests
- `pyro_fired` flags never set after `pyro_fire()` — pyros re-fired every tick
- Pressure filter integer stall — truncation to 0 when diff < 22 Pa
- Landing detection false trigger — needed speed + altitude + stability check

### Config System
- INI parser reads config.ini on boot
- Config POST writes to littlefs (was a stub)
- Beep code 4-3 for out-of-range altitude settings
- 8,000m altitude clamp in firmware and config values
- 12 config parser unit tests

### Web Interface
- 4-tab UI: Status, Config, Flight Data, Update
- Guided config editor with all fields (id, name, units, beep, pyro1, pyro2)
- Pending config warning, range warnings, tips
- GitHub Pages demo with service worker mock
- Rocket favicon

### Web Testing
- Mock server with 3 modes (new, configured, post-flight)
- 22 Playwright tests in CI
- Flight CSV with graph and pyro event details

### Documentation
- REQUIREMENTS.md: 123 requirements in L1-L4 hierarchy (user needs → implementation)
- TRACEABILITY.md: Requirements → integration/closed-loop tests
- All tests renamed with requirement prefixes
- Code annotated with requirement traceability tags

## March 8 (Sunday)

### HAL Refactor
- Created hal.h with ~15 functions (time, pressure, pyro, buzzer, telemetry, filesystem)
- flight_states.c, telemetry.c, buzzer.c: zero #ifdef, zero platform includes
- Three HAL implementations: hal_hardware.c, hal_test.c, hal_sim.c
- flight_controller.c → main_hardware.c
- flight_init() and flight_update_outputs() own all flight logic

### Simulator
- sim/main_sim.c: pyro black box (no physics)
- sim/hal_sim.c: simulation HAL with in-memory filesystem
- sim/sim_cli.c: separate physics driver
- ninja sim builds and runs

### Streaming CSV Writer
- Added hal_fs_open/write/close for streaming file writes
- flight_save_csv() streams line-by-line, flight buffer stays intact
- /api/flight.csv serves actual file from littlefs

### Event-Driven State Machine
- Replaced switch/case dispatch with transition table (8 rows)
- Detectors return events, actions handle side effects
- Every transition visible in one place

### Boot State Cleanup
- Collapsed 8 boot states to 4 (BOOT_INIT, BOOT_SETTLE, BOOT_CONTINUITY, BOOT_CALIBRATE)
- Removed I2C, filesystem, mDNS, sensor detect from state names
- HAL handles hardware init internally

### Code Quality
- cppcheck with MISRA addon in CI
- clang-format check in CI
- pmccabe complexity check (threshold 15)
- Refactored: parse_config_ini, state_pad_idle, state_descent, buzzer_update
- Removed apogee buffer protection (streaming CSV makes it unnecessary)
- Clean comments: traceability tags + WHY comments + doc references only

### Requirements Engineering
- 123 requirements in L1-L4 hierarchy across 12 categories
- Traceability restructured: requirements → integration tests (not unit tests)
- Gap analysis: 7 critical safety gaps, 3 unimplemented features

### Interactive WASM Simulation
- scripts/build_wasm.sh for Emscripten compilation
- docs/sim.html: interactive UI with config, launch, live graph
- docs/physics.js: JS physics engine (separate from pyro code)
- docs/buzzer.js: Web Audio driver for buzzer tones
- CI builds WASM on every push

### Hardware CI Plan
- HARDWARE_CI_PLAN.md: 4 Proxmox VMs, 2× MS5607 + 2× BMP280
- Self-hosted GitHub Actions runners
- Flash → test → OTA → config cycle
- Execute next week

## March 24 (v2-7 through v2-9 session)

### Buzzer Parallel State Machine (v2-7) ✅
- `async_task_t`-based pattern player in `buzzer.c`
- `BZ_IDLE → BZ_ENCODE_CODE/ALT → BZ_PLAYING` states
- Encodes `buzzer_pattern_t[]` at request time; `hal_tasks_tick()` fires each step
- `buzzer_play_code(code, repeat)` / `buzzer_play_altitude(value)` non-blocking API
- `buzzer_update()` becomes a no-op shim (no main-loop involvement)
- `hal_buzzer_task_register()` added to all 3 HAL implementations
- 8 new unit tests: BUZ-PAT-01..05, BUZ-ACT-01..03 — all pass

### Batch Pressure Processing (v2-8) ✅
- `flight_process_samples(ctx, batch)` — processes 5-sample 50Hz batch in one call
- `detect_ascent()` / `detect_descent()` gates at 20ms; `detect_pad_idle()` at 10ms
- `main_hardware.c` uses `hal_pressure_fifo_get()` → `flight_process_samples()` loop
- `hal_pressure_push_sample()` + `hal_pressure_batch_t` added to HAL

### Fire-and-Forget Flight Log (v2-9) ✅
- `hal_log_start()` / `hal_log_sample()` / `hal_log_stop()` / `hal_log_active()` in hal.h
- Three implementations: hardware (LittleFS streaming), sim (host FS), test (in-memory)
- `hal_log_sample()` called from `buf_add()` (ASCENT+) and `buf_tag_event()` for events
- LAUNCH event emitted explicitly (PAD_IDLE sample is below the >= ASCENT guard)
- Incremental ring-buffer logger (`csv_flush_safe/step/track`) retired from all call sites
- `mock_reset_all()` now resets streaming handle state between tests
- `test_DAT_04_events` updated to read `flight_log.csv`
- `test_PYR_FAULT_02` fixed: breaks early on fault flags, calls `hal_log_stop()` before `flight_save_csv()`
- All 5 test suites: **99 Tests, 0 Failures**
- Commits: `6cab391` (hal_log implementations), `6224112` (CSV logger retirement)

## March 9-17 (v1.5.0 work, separate session)

### Safety Tests Added
- test_PYR_SAFE_01_no_fire_without_continuity
- test_PYR_SAFE_02_no_simultaneous_fire
- test_SYS_DEPLOY_03_no_fire_during_ascent
- test_PYR_FAULT_02_overcurrent_detection

### Pyro Fault Detection Implemented
- hal_pyro_fault() reads AP2192 FLAG pins (GPIO 17/18)
- Post-fire continuity verification (ADC re-check 500ms after fire)
- EVT_PYRO1_FAULT, EVT_PYRO2_FAULT, EVT_PYRO1_NOPEN, EVT_PYRO2_NOPEN events
- Beep codes 2-3/2-4/3-3/3-4

### Simulation Library
- Physics engine extracted to sim/physics.c (shared between CLI and WASM)
- sim/README.md and sim/INTEGRATION.md documentation
- WASM rebuilt with 500ms filter time constant
- Buzzer majority-vote fix for 10x speed
- Flight time display fix (counts from launch, not boot)
- Synchronous WASM loading fix
- Physics-only fallback mode + 10s post-landing dwell

### Test Count: 64 C + 22 web
- 39 unit, 12 integration, 13 closed-loop

### Released v1.5.0
