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
