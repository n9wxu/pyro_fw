# Pyro MK1B Firmware - Current Status

## ✅ Completed Features

### Flight Controller Core
- **State machine**: Enum-indexed function pointer array (5 states)
- **States**: PAD_IDLE, LAUNCH, APOGEE, FALLING, LANDED
- **Function signature**: `flight_state_t (*state_fn_t)(flight_context_t *, uint32_t)`
- **Pure functions**: All state passed via context, no globals
- **Sample rates**: 
  - PAD_IDLE: 100Hz
  - LAUNCH/APOGEE: 10Hz  
  - FALLING: 20Hz
  - LANDED: 1Hz

### Sensor & Data Processing
- **Pressure filtering**: Exponential moving average, 1-second time constant
- **Integer-only math**: No floating point operations
- **Altitude calculation**: Simplified barometric formula `(P0-P)*8.3 cm/Pa`
- **Dual sensor support**: MS5607 and BMP280 with unified interface
- **I2C**: GPIO8 (SDA), GPIO9 (SCL), 400kHz

### Pyrotechnic Control
- **Non-blocking fire**: 500ms duration tracked in main loop
- **Dual channels**: GPIO21 (PYRO1), GPIO22 (PYRO2)
- **Common enable**: GPIO15
- **Fault detection**: GPIO17/18 (hardware ready, not implemented)
- **Continuity check**: ADC0/1 with 256x oversampling (12→16 bit)

### Data Logging
- **Buffer**: 64KB RAM circular buffer
- **Apogee protection**: 50 samples before apogee preserved
- **Format**: Timestamp, pressure, altitude, state
- **Capacity**: ~8000 samples at 8 bytes each

### Telemetry
- **Format**: Eggtimer-compatible with trigger bytes
- **Example**: `{046>@5>#0018>~1B---->%04679>=PYRO001>}`
- **Output**: UART0 GPIO0 (TX), GPIO1 (RX), 115200 baud
- **Currently**: Disabled for USB MSC debugging

### USB Integration
- **CDC**: Working (stdio over USB)
- **MSC**: Enumerates as 1.9MB drive
- **Initialization order**: board_init() → tud_init() → stdio_init_all()
- **Main loop**: No sleep, tud_task() runs at full speed
- **LittleFS**: Mounts and formats correctly (1.8MB partition)

### Build System
- **Toolchain**: ARM GCC from Pico SDK
- **Generator**: Ninja (fast builds)
- **Structure**: src/, test/, build/
- **Output**: 182KB .uf2 flashable firmware
- **Tests**: Unity framework integrated

## ⚠️ Known Issues

### USB MSC Filesystem Not Recognized (macOS)
**Symptoms:**
- macOS reads boot sector 6 times
- Reads FAT sector 1 twice, FAT sector 2 once
- Never reads root directory (sector 23)
- Rejects filesystem and asks to initialize

**Investigation Done:**
- Boot sector values verified correct:
  - 3680 total sectors (1.9MB)
  - 11 FAT sectors per copy
  - 2 FAT copies (standard FAT12)
  - 16 root directory entries
  - 512 bytes per sector
  - 1 sector per cluster
- FAT header verified: F8 FF FF (media descriptor + cluster 1)
- Both FAT copies return same data
- Root directory calculation correct (sector 23)
- Integer-only math (no floating point)

**Possible Causes:**
- macOS-specific validation requirements
- Subtle FAT12 spec compliance issue
- Boot sector field mismatch
- FAT entry interpretation

**Next Steps:**
1. Test with Windows/Linux to isolate macOS-specific issue
2. Compare byte-by-byte with working FAT12 image
3. Use USB analyzer to see exact validation macOS performs
4. Review FAT12 specification for required vs optional fields

## 🔧 Not Yet Implemented

### Configuration
- [ ] config.ini parser
- [ ] Default configuration values
- [ ] Runtime configuration updates

### File Operations
- [ ] CSV file writer for flight data
- [ ] Metadata header in CSV
- [ ] File naming with flight number

### Safety Features
- [ ] Fault detection monitoring (GPIO17/18)
- [ ] Safe input handler for manual pyro test
- [ ] Post-fire ADC verification
- [ ] Arming sequence

### User Interface
- [ ] Beep code generator (GPIO16)
- [ ] Status LED patterns
- [ ] Button input handling

### Testing
- [ ] Unit tests for state machine
- [ ] Pressure simulation for state transitions
- [ ] Pyro fire sequence validation
- [ ] Buffer overflow protection tests
- [ ] Complete flight sequence simulation

## 📝 Technical Notes

### Hardware Configuration
```
I2C0:    GPIO8 (SDA), GPIO9 (SCL), 400kHz
UART0:   GPIO0 (TX), GPIO1 (RX), 115200 baud
Pyro:    GPIO15 (common), GPIO21/22 (channels)
Fault:   GPIO17/18 (detection inputs)
ADC:     GPIO26/27 (continuity check)
Buzzer:  GPIO16
```

### Memory Layout
```
Flash:   2MB total
  - Firmware: ~182KB
  - LittleFS: 1.8MB (at end of flash)
RAM:     264KB total
  - Flight data buffer: 64KB
  - Stack/heap: ~200KB
```

### Key Code Patterns
```c
// State function signature
typedef flight_state_t (*state_fn_t)(flight_context_t *, uint32_t);

// Main loop
while (1) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    tud_task();  // USB - must run fast
    update_pyro_fire(&ctx, now);
    ctx.current_state = state_functions[ctx.current_state](&ctx, now);
}

// Pressure filter (integer only)
int32_t alpha = (dt_ms * 1000) / (1000 + dt_ms);
ctx->filtered_pressure += ((raw - ctx->filtered_pressure) * alpha) / 1000;
```

## 🚀 Usage

### Build
```bash
cd pyro_fw
mkdir -p build && cd build
cmake -G Ninja ..
ninja
```

### Flash
```bash
# Hold BOOTSEL, connect USB, release BOOTSEL
cp build/pyro_fw_c.uf2 /Volumes/RPI-RP2/
```

### Monitor
```bash
# USB CDC (stdio)
screen /dev/tty.usbmodem* 115200

# UART telemetry
screen /dev/tty.usbserial* 115200
```

## 📚 References
- [Pico SDK Documentation](https://www.raspberrypi.com/documentation/pico-sdk/)
- [TinyUSB Documentation](https://docs.tinyusb.org/)
- [LittleFS](https://github.com/littlefs-project/littlefs)
- [Oyama pico-littlefs-usb](https://github.com/oyama/pico-littlefs-usb)
- [FAT12 Specification](https://en.wikipedia.org/wiki/File_Allocation_Table#FAT12)
