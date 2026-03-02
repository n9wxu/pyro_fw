# Pyro MK1B Firmware - Current Status

## ✅ Completed

### fat_mimic Library (`lib/fat_mimic/`)
- Writable FAT12 USB Mass Storage over littlefs
- No flash I/O in USB callbacks (50ms quiet gate)
- Three sync triggers: sector 32 batch, slot reuse, gap detection
- Orphan deletion on unmount only
- ~35KB RAM, ~400 lines
- Tested: edits persist, deletions work, no USB panics

### Pressure Sensors
- MS5607 and BMP280 with auto-detection
- Unified interface, I2C at 400kHz

### USB Integration
- TinyUSB MSC device, mounts on macOS/Windows/Linux
- config.ini editable via USB drive

### Build System
- Pico SDK 2.2.0, Ninja, littlefs v2.11.2 via FetchContent

## 🔨 In Progress - Flight State Machine Overhaul

### States: PAD_IDLE → ASCENT → DESCENT → LANDED
- [ ] Rename LAUNCH→ASCENT, FALLING→DESCENT, remove APOGEE state
- [ ] Test mode via GPIO 8 jumper
- [ ] Vertical speed calculation (integer math)
- [ ] ASCENT: thrust detection, pyro arming at <10 m/s, apogee event
- [ ] T=0 backdating to first motion
- [ ] Four pyro modes: fallen, agl, speed, delay (both channels)
- [ ] Post-fire diagnostics: BAD_PARACHUTE, BAD_PYRO, re-fire
- [ ] DESCENT: pyro firing, landing detection (<1m for 1s)
- [ ] LANDED: altitude beep-out (m/ft/ft100)
- [ ] Config parser: pyro modes, values, altitude_beep

## 🔧 Not Yet Implemented
- [ ] CSV flight data writer
- [ ] Buzzer/beep code driver
- [ ] Fault detection monitoring (GPIO 17/18)
- [ ] Unit tests for state machine
