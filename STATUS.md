# Pyro MK1B Firmware - Current Status

## ✅ Completed Features

### Flight Controller Core
- **State machine**: Enum-indexed function pointer array (5 states)
- **States**: PAD_IDLE, LAUNCH, APOGEE, FALLING, LANDED
- **Pure functions**: All state passed via context, no globals
- **Variable sample rates**: 100Hz (pad), 10Hz (launch/apogee), 20Hz (falling), 1Hz (landed)

### Sensor & Data Processing
- **Dual sensor support**: MS5607 and BMP280 with auto-detection
- **Integer-only math**: No floating point operations
- **Pressure filtering**: Exponential moving average, 1-second time constant
- **Altitude calculation**: Simplified barometric formula

### Pyrotechnic Control
- **Non-blocking fire**: 500ms duration tracked in main loop
- **Dual channels**: GPIO21 (PYRO1), GPIO22 (PYRO2), GPIO15 (common enable)
- **Continuity check**: ADC0/1 with 256x oversampling (12→16 bit)

### Data Logging
- **Buffer**: 64KB RAM circular buffer with apogee protection
- **Format**: Timestamp, pressure, altitude, state

### Telemetry
- **Format**: Eggtimer-compatible with trigger bytes
- **Output**: UART0 at 115200 baud

### USB Mass Storage (fat_mimic library)
- **Read/write FAT12 filesystem** over littlefs on RP2040 flash
- **macOS, Windows, Linux compatible** USB Mass Storage device
- **File edits persist** across reboots via littlefs
- **File deletion** works via eject/unmount
- **No USB panics**: Flash writes gated on 50ms USB quiet period
- **~35KB RAM**: FAT cache (2KB) + root dir (16KB) + dirty cache (16KB) + file table (512B)
- **Three-layer architecture**:
  - USB callbacks: RAM-only, no flash I/O
  - Poll (main loop): One file flush per call to littlefs
  - Init (mount): Build FAT12 image from littlefs
- **Three sync triggers**: Sector 32 batch complete, slot reuse detection, gap detection
- **Orphan deletion**: Only on unmount/eject (prevents premature deletion)
- **Library structure**: `lib/fat_mimic/` with separate CMakeLists.txt for future FetchContent use

### Build System
- **Toolchain**: ARM GCC from Pico SDK 2.2.0
- **Generator**: Ninja
- **Dependencies**: littlefs v2.11.2 (FetchContent), Unity v2.6.0 (tests)
- **Output**: ~90KB text, ~40KB BSS

## ⚠️ Known Limitations

### USB Mass Storage
- FAT12 only (max ~680KB usable space)
- 16 files maximum in root directory
- 8.3 filenames (LFN entries preserved in cache but not decoded for littlefs storage)
- No subdirectory support
- 32 dirty sector cache slots (backpressure eviction on overflow)
- Hard unplug may lose in-progress edits (littlefs metadata remains safe)
- Temp files from editors accumulate until eject

## 🔧 Not Yet Implemented

### Configuration
- [ ] config.ini parser
- [ ] Runtime configuration updates

### File Operations
- [ ] CSV file writer for flight data
- [ ] File naming with flight number

### Safety Features
- [ ] Fault detection monitoring (GPIO17/18)
- [ ] Post-fire ADC verification
- [ ] Arming sequence

### User Interface
- [ ] Beep code generator
- [ ] Status LED patterns

## 📝 Technical Notes

### Memory Layout
```
Flash:   2MB total
  - Firmware: ~90KB
  - LittleFS: ~680KB (at end of flash)
RAM:     264KB total
  - Flight data buffer: 64KB
  - fat_mimic caches: ~35KB
  - Stack/heap: ~165KB
```

### Key Architecture Decision: fat_mimic
The FAT12-over-littlefs translation was rewritten from a 2200-line monolithic implementation (~57KB RAM) to a 400-line library (~35KB RAM). Key design choices:
- **No flash I/O in USB callbacks** prevents RP2040 XIP/DMA conflicts
- **Write-back dirty cache** buffers data sector writes in RAM
- **Deferred sync** waits for USB quiet period before flash operations
- **Cluster-based orphan detection** avoids FAT 8.3 name comparison issues
- **One file per poll cycle** keeps flash operations short

## 🚀 Usage

### Build
```bash
mkdir -p build && cd build
cmake -G Ninja ..
ninja
```

### Flash
Hold BOOTSEL, connect USB, copy `build/pyro_fw_c.uf2` to RPI-RP2 drive.

### Edit Configuration
Connect USB → edit `config.ini` on the mounted drive → eject before unplugging.
