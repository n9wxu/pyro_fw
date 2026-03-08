# Pyro MK1B Firmware - Current Status

## ✅ Completed

### Unified State Machine
- Non-blocking boot: BOOT_FILESYSTEM → I2C_SETTLE → SENSOR_DETECT → PYRO_INIT → CONTINUITY → STABILIZE → CALIBRATE → MDNS → PAD_IDLE
- Flight: PAD_IDLE → ASCENT → DESCENT → LANDED
- Pure functional: all state in flight_context_t, no globals in state/telemetry code
- Split into flight_states.c (states + helpers), telemetry.c, buzzer.c, flight_controller.c (main loop)

### Telemetry
- $PYRO NMEA format with XOR checksum
- Fields: seq, state, thrust, alt, vel, maxalt, press, time, flags, p1adc, p2adc, batt, temp
- 10Hz during ASCENT/DESCENT, 1Hz during PAD_IDLE/LANDED
- Skipped during boot states

### Buzzer
- GPIO 16 on/off (no PWM needed)
- Startup: 10 fast chirps → pause → status code × 2 → stop
- Status codes: 1-1 good, 2-1 P1 open, 2-2 P1 short, etc.
- Altitude beep-out after landing: long pause → long beep → digit sequence → repeat
- Digit encoding: 0 = 10 beeps, 1-9 = N beeps
- Uses config.units for altitude conversion

### Event Logging
- Events tagged on existing data samples (no separate records)
- EVT_LAUNCH, EVT_ARMED, EVT_APOGEE, EVT_PYRO1_FIRE, EVT_PYRO2_FIRE, EVT_LANDING
- Same 16-byte sample struct (event + event_data replace padding)

### Web Interface & Networking
- USB composite: ECM/RNDIS + vendor reset (picotool)
- HTTP server with CORS, mDNS (pyro.local), DNS-SD (_pyro._tcp)
- Live dashboard, config editor, firmware update
- GitHub release update checker in web UI

### OTA & Flashing
- A/B bootloader with automatic rollback
- picotool vendor reset interface (deferred to main loop)
- flash_picotool.sh, upload_fw.sh, upload_www.sh, install.py
- update_from_release.py for GitHub release updates

### Pressure Sensors
- MS5607 (GPIO 10/7) and BMP280 (GPIO 6/7) auto-detection
- I2C pin release between detect attempts

### Build & CI/CD
- GitHub Actions: build on push, release on tag, 47 tests in CI
- Auto-incrementing version (CI-aware)
- Beta/prerelease support

### Testing (47 tests)
- 27 unit tests: helpers, boot sequence, all flight states, telemetry formatting
- 11 integration tests: full flight simulation with OpenRocket data
- 9 closed-loop tests: 28 simulated flights with physics feedback (100ft to Karman line)
- Host-compiled with mocks (pressure, pyro, UART, buzzer, GPIO, LFS)
- Network test suite (support/test_network.py) with TUI

## 🔨 Known Issues

- [ ] Parallel HTTP connections (6+) can drop under heavy load

## 🔧 Not Yet Implemented

- [ ] Config parser (read pyro modes/values/units from config.ini)
- [ ] CSV flight data writer (save log to littlefs)
- [ ] Fault detection monitoring (GPIO 17/18 AP2192 FLAG pins)
- [ ] Post-fire ADC verification
- [ ] Test mode via GPIO 8 jumper
- [ ] Web app separate versioning
