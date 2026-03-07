# Pyro MK1B Firmware - Current Status (v1.2.0)

## ✅ Completed

### Unified State Machine
- Non-blocking boot sequence: filesystem → I2C → sensor detect → pyro → calibration → mDNS → PAD_IDLE
- Flight states: PAD_IDLE → ASCENT → DESCENT → LANDED
- No sleep_ms in startup — USB enumeration works immediately
- Single dispatch_state() handles all boot + flight states

### Web Interface & Networking
- USB composite device: ECM/RNDIS network + vendor reset interface
- HTTP server at 192.168.7.1 with DHCP
- mDNS: pyro.local (RFC 6762) with conflict resolution
- DNS-SD: _pyro._tcp service for automatic discovery
- CORS headers on API endpoints
- Live dashboard with 1s polling, 3-miss tolerance
- Config download/upload, file upload, firmware update via web
- One-click firmware update from GitHub releases in browser

### OTA Firmware Updates
- A/B bootloader via pico_fota_bootloader
- Incremental sector erase during upload (USB-safe)
- Automatic rollback if new firmware doesn't commit
- Upload via web UI, support/upload_fw.sh, or support/update_from_release.py

### picotool Support
- Vendor reset interface (VID 0x2E8A, PID 0x4002)
- Deferred reset to main loop (protects I2C bus)
- support/flash_picotool.sh for full flash cycle

### HTTP Server
- Reliable file serving (fills TCP send buffer, handles ERR_MEM)
- Connection pool (8 slots) with tcp_err cleanup
- TCP PCB pool: 16, heap: 8KB
- Streaming file reads from littlefs

### Pressure Sensors
- MS5607 and BMP280 with auto-detection
- I2C pin release between detect attempts (no bus contention)
- Unified interface, I2C1 at 400kHz

### Pyro Module
- Dual channel with AP2192 power switches
- Raw 12-bit ADC continuity sensing
- 500ms fire duration with auto-shutoff

### Build & CI/CD
- Pico SDK 2.2.0, Ninja, littlefs v2.11.2 via FetchContent
- pico_fota_bootloader via FetchContent
- GitHub Actions: build on push, release on tag
- Auto-incrementing version (VERSION file + gen_version.sh)
- CI-aware version generation (no increment in CI)

### Testing
- Comprehensive Python test suite (support/test_network.py)
- Live TUI with split screen (tests + UART)
- Interactive mode, log analyzer, pre-test diagnostics
- 16/17 tests passing (1 parallel connection drop under heavy load)

### Support Tools
- support/install.py — interactive installer for build artifacts
- support/flash_picotool.sh — picotool flash cycle
- support/upload_fw.sh — OTA firmware upload
- support/upload_www.sh — web file upload
- support/update_from_release.py — update from GitHub releases
- support/test_network.py — network test suite

## 🔨 In Progress

- [ ] Parallel connection handling under heavy load (6+ simultaneous)

## 🔧 Not Yet Implemented

- [ ] Telemetry UART (Eggtimer format — code exists, disabled)
- [ ] Event logging system (RAM buffer + flash on error)
- [ ] Buzzer/beep code driver (GPIO 16, 3kHz PWM)
- [ ] Config parser for pyro modes/values from config.ini
- [ ] CSV flight data writer
- [ ] Fault detection monitoring (GPIO 17/18)
- [ ] Post-fire ADC verification
- [ ] Altitude beep-out after landing
- [ ] Test mode via GPIO 8 jumper
- [ ] Unit tests for state machine
- [ ] Web app separate versioning + self-update
