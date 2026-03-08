# Pyro MK1B Firmware - Current Status

## ✅ Completed

### Unified State Machine
- Non-blocking boot: BOOT_FILESYSTEM → I2C_SETTLE → SENSOR_DETECT → PYRO_INIT → CONTINUITY → STABILIZE → CALIBRATE → MDNS → PAD_IDLE
- Flight: PAD_IDLE → ASCENT → DESCENT → LANDED
- Pure functional: all state in flight_context_t, no globals in state/telemetry code
- Split into flight_states.c (states + helpers), telemetry.c, buzzer.c, flight_controller.c (main loop)

### Config System
- INI file parser reads config.ini from littlefs on boot
- Web config editor with guided UI, range warnings, tips
- Beep code 4-3 warns when pyro altitude settings exceed 8,000m sensor limit
- Config POST API writes to littlefs, reboot applies
- Altitude settings clamped to 8,000m barometric ceiling

### Telemetry
- $PYRO NMEA format with XOR checksum
- Fields: seq, state, thrust, alt, vel, maxalt, press, time, flags, p1adc, p2adc, batt, temp
- 10Hz during ASCENT/DESCENT, 1Hz during PAD_IDLE/LANDED

### Buzzer
- GPIO 16 on/off (no PWM needed)
- Startup: 10 fast chirps → pause → status code × 2 → stop
- Status codes: 1-1 good, 2-1 P1 open, 2-2 P1 short, 4-3 config range, etc.
- Altitude beep-out after landing: long pause → long beep → digit sequence → repeat

### Event Logging
- Events tagged on existing data samples (no separate records)
- EVT_LAUNCH, EVT_ARMED, EVT_APOGEE, EVT_PYRO1_FIRE, EVT_PYRO2_FIRE, EVT_LANDING

### Web Interface & Networking
- USB composite: ECM/RNDIS + vendor reset (picotool)
- HTTP server with CORS, mDNS (pyro.local), DNS-SD (_pyro._tcp)
- Tabbed UI: Status, Config, Flight Data, Update
- Config editor with Default/Current/Save/Upload/Reboot buttons
- Pending config warning across tabs
- GitHub release checker with "Show All Versions" for downgrades
- GitHub Pages interactive demo (no hardware needed)

### OTA & Flashing
- A/B bootloader with automatic rollback
- picotool vendor reset interface (deferred to main loop)
- /api/reboot endpoint for web-triggered reboot
- flash_picotool.sh, upload_fw.sh, upload_www.sh, install.py
- update_from_release.py for GitHub release updates

### Pressure Sensors
- MS5607 (GPIO 10/7) and BMP280 (GPIO 6/7) auto-detection
- I2C pin release between detect attempts

### Build & CI/CD
- GitHub Actions: build on push, release on tag
- 59 C tests + 22 Playwright web tests in CI
- Auto-incrementing version (CI-aware)
- Beta/prerelease support

### Testing
- 39 unit tests: helpers, boot, flight states, telemetry, config parser (12 tests)
- 11 integration tests: full flight simulation with OpenRocket data
- 9 closed-loop tests: 28 simulated flights with physics feedback (100ft to Karman line)
- 22 Playwright web UI tests against mock server (3 modes)
- JS syntax validation in CI
- Host-compiled with mocks (pressure, pyro, UART, buzzer, GPIO, LFS)
- Network test suite (support/test_network.py) with TUI

## 🔨 Known Issues

- [ ] Parallel HTTP connections (6+) can drop under heavy load

## 🔧 Not Yet Implemented

- [ ] CSV flight data writer (save log to littlefs)
- [ ] Fault detection monitoring (GPIO 17/18 AP2192 FLAG pins)
- [ ] Post-fire ADC verification
- [ ] Test mode via GPIO 8 jumper
