# Pyro MK1B Firmware - Current Status

## ✅ Completed

### Event-Driven State Machine
- Transition table: 8 rows define every possible state change
- Detectors (per-tick work) → Events → Actions (side effects)
- 4 boot states + 4 flight states, no implementation leakage
- See IMPLEMENTATION.md for transition table

### Hardware Abstraction Layer
- hal.h: 15 functions covering all hardware interaction
- Zero #ifdef in flight code (flight_states.c, telemetry.c, buzzer.c)
- Three implementations: hardware, test, simulation
- Same source compiles for Pico, host tests, and WASM

### Config System
- INI parser with 12 unit tests
- Web config editor (all fields: id, name, units, beep, pyro1, pyro2)
- Beep code 4-3 for out-of-range altitude settings
- 8,000m altitude clamp on all altitude-based settings

### Telemetry
- $PYRO NMEA format with XOR checksum
- 10Hz ASCENT/DESCENT, 1Hz PAD_IDLE/LANDED

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

### Testing (64 C + 22 web)
- 39 unit, 12 integration, 13 closed-loop (32+ flights)
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

## ❌ Not Implemented
- [ ] Progressive in-flight CSV logging
- [ ] Test mode (GPIO 8 jumper ground test sequence)
