# Pyro MK1B Firmware - Current Status

## ✅ Completed

### Web Interface & Networking
- USB network device (ECM+RNDIS) via TinyUSB + lwIP
- HTTP server at 192.168.7.1 with DHCP and DNS
- Live dashboard with 500ms polling (state, altitude, pressure, pyro status)
- Config download/upload via web
- File upload to littlefs via POST
- Firmware version displayed in dashboard

### OTA Firmware Updates
- A/B bootloader via [pico_fota_bootloader](https://github.com/JZimnol/pico_fota_bootloader)
- Incremental sector erase during upload (USB-safe)
- Automatic rollback if new firmware doesn't commit
- Upload via `./upload_fw.sh` or web UI button
- Debugger loads both bootloader + app automatically

### Flash Layout
- Bootloader: 36KB, Info: 4KB
- App Slot A: 512KB, App Slot B: 512KB
- LittleFS: 984KB (config, web files, flight data)

### Pressure Sensors
- MS5607 and BMP280 with auto-detection
- Unified interface, I2C at 400kHz

### Pyro Module
- Dual channel with AP2192 power switches
- Raw 12-bit ADC continuity sensing
- 500ms fire duration with auto-shutoff

### Flight State Machine
- 4 states: PAD_IDLE → ASCENT → DESCENT → LANDED
- Four pyro modes: fallen, agl, speed, delay
- Vertical speed calculation, apogee arming at <10 m/s

### Build System
- Pico SDK 2.2.0, Ninja, littlefs v2.11.2 via FetchContent
- pico_fota_bootloader via FetchContent
- Unity test framework

## 🔨 In Progress

- [ ] Test mode via GPIO 8 jumper
- [ ] Config parser for pyro modes/values
- [ ] CSV flight data writer
- [ ] Buzzer/beep code driver

## 🔧 Not Yet Implemented

- [ ] Fault detection monitoring (GPIO 17/18)
- [ ] Post-fire ADC verification
- [ ] Altitude beep-out after landing
- [ ] Unit tests for state machine
- [ ] mDNS for pyro.local hostname
