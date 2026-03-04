# Session Notes - March 3, 2026

## What We Did Tonight

### 1. OTA Firmware Updates (COMPLETE)
- Added POST `/api/ota` endpoint for firmware upload
- Initial attempt: direct flash write — bricked device (overwrites running code)
- Evaluated A/B bootloader options:
  - RP2350 has native partition table support in ROM — not available on RP2040
  - Found [pico_fota_bootloader](https://github.com/JZimnol/pico_fota_bootloader) (MIT, RP2040 support)
- Integrated pico_fota_bootloader via FetchContent
- Hit HardFault: `pfb_initialize_download_slot()` erases 512KB with interrupts disabled (~6s), kills USB
- Fix: incremental sector erase (4KB at a time) — keeps interrupt-disabled window to ~50ms
- A/B swap + automatic rollback working

### 2. Flash Layout Redesign
- Old: firmware at 0x0, littlefs 660KB at top
- New: bootloader 36KB, info 4KB, Slot A 512KB, Slot B 512KB, littlefs 984KB
- Updated littlefs driver FS_SIZE from (1323*512) to (984*1024)

### 3. Web Interface Improvements
- Added firmware update button with file picker and confirmation dialog
- Fixed config download: was returning hardcoded `{}`, now reads config.ini from littlefs
- Fixed app.js: was parsing config as JSON, changed to plain text
- Added firmware version (FW_VERSION "1.0.0") to status API and dashboard

### 4. Debugger Configuration
- App now linked to Slot A (0x1000A000) — debugger couldn't find main
- Updated launch.json to load bootloader ELF before app ELF

### 5. Build System
- Added pico_fota_bootloader via FetchContent with options:
  - PFB_WITH_IMAGE_ENCRYPTION=OFF
  - PFB_WITH_SHA256_HASHING=OFF
  - PFB_REDIRECT_BOOTLOADER_LOGS_TO_UART=ON
  - PFB_RESERVED_FILESYSTEM_SIZE_KB=984
- Suppressed -Warray-bounds warning from third-party flash_utils.c
- Fixed cmake generator: project uses Ninja, not Make

### Key Files Modified
- `CMakeLists.txt` — pico_fota_bootloader dep, flash layout config
- `src/http_server.c` — OTA endpoint, config download fix, version API
- `src/flight_controller.c` — pfb_firmware_commit() on boot
- `src/littlefs_driver.c` — FS_SIZE updated to 984KB
- `www/index.html` — firmware update UI section
- `www/app.js` — firmware upload function, config as text, version display
- `.vscode/launch.json` — dual ELF loading for debugger
- `upload_fw.sh` — OTA upload script (uses fota_image.bin)

---

# Session Notes - March 2, 2026

## What We Did Tonight

### 1. fat_mimic Library (COMPLETE)
- Rewrote FAT12-over-littlefs as `lib/fat_mimic/` (~400 lines, ~35KB RAM)
- Write-back dirty cache, 50ms USB quiet gate, three sync triggers
- Tested: edits persist, deletions work, no USB panics
- **Status: Working but has edge cases with filenames and temp files**

### 2. Pivoted to HTTP Interface (IN PROGRESS)
- Replaced USB Mass Storage with USB network device (ECM+RNDIS)
- Added lwIP TCP/IP stack, DHCP server, DNS server
- Built custom HTTP server with:
  - File listing page at `http://192.168.7.1/`
  - File download links
  - Live JSON status API at `/api/status`
  - Auto-refreshing dashboard (500ms polling)
- **Ping works, page loads, dashboard updates live**
- Pressure sensor reading live on dashboard (BMP280 on I2C1, GPIO 6 SDA / GPIO 7 SCL)

### 3. Flight State Machine Overhaul (PARTIAL)
- Renamed: LAUNCH→ASCENT, FALLING→DESCENT, removed APOGEE state
- 4 states: PAD_IDLE, ASCENT, DESCENT, LANDED
- Switch dispatch with default→PAD_IDLE
- Four pyro modes: fallen, agl, speed, delay
- Post-fire diagnostics: BAD_PARACHUTE, BAD_PYRO
- Vertical speed calculation, apogee arming at <10 m/s
- **Not yet tested in flight - only PAD_IDLE verified**

### 4. Pyro Module Refactored
- New `src/pyro.c` and `src/pyro.h` with clean API
- Raw 12-bit ADC reads (no oversampling)
- Live continuity check every 1s in PAD_IDLE
- Raw ADC values shown on dashboard
- **ADC readings: short=343, open=400 - very close, thresholds need tuning**

### 5. Filesystem Analysis (docs/)
- `docs/fat12_image_proposal.md` - FAT12 image on littlefs
- `docs/pure_fat12_remap_proposal.md` - Custom FAT12 + cluster remap
- `docs/fatfs_review.md` - FatFs + cluster remap (recommended for FAT approach)
- `docs/petit_fatfs_review.md` - Rejected, too restricted
- `docs/filesystem_analysis.md` - Full comparison matrix
- `docs/web_interface_onepager.md` - Marketing one-pager for HTTP approach
- `docs/http_implementation_notes.md` - Implementation notes

## Tomorrow's Priorities

### 1. Fix Pyro ADC Readings
- 343 vs 400 counts is too close - investigate circuit
- May need different ADC channel, pull-up value, or measurement technique
- Consider: is GPIO 15 (common enable) actually toggling? Scope it.
- The 100kΩ pull-up with ~0.5Ω pyro should give near-zero volts (ADC ~0)
- Open circuit should give 3.3V (ADC ~4095)
- Getting ~340-400 for both suggests the pull-up isn't working or wrong pin

### 2. HTTP File Upload
- Add POST handler for file upload
- HTML form with file input
- Write uploaded data to littlefs

### 3. Config Editor
- In-browser textarea for editing config.ini
- Save button that POSTs back to device

### 4. DNS Resolution
- `http://pyro.local/` via mDNS (currently only IP works)
- DNS server is initialized but hostname may need adjustment

### 5. Test Mode (GPIO 8)
- Jumper detection at boot before I2C init
- Beep pattern, countdown, pyro fire sequence
- Needs buzzer driver (GPIO 16, 3kHz PWM)

## Key Files Modified
- `src/flight_controller.c` - state machine, main loop
- `src/tusb_config.h` - ECM+RNDIS network class
- `src/usb_descriptors.c` - network device descriptors
- `src/net_glue.c` - TinyUSB ↔ lwIP bridge + DHCP
- `src/http_server.c` - HTTP server with dashboard
- `src/device_status.h` - shared status struct
- `src/pyro.c` / `src/pyro.h` - new pyro module
- `src/pressure_sensor.c` - I2C1 on GPIO 6/7, auto-detect
- `src/bmp280_driver.c` - changed to I2C1
- `src/ms5607_driver.c` - changed to I2C1
- `src/lwipopts.h` - lwIP configuration
- `CMakeLists.txt` - lwIP sources, networking helpers

## Files to Clean Up
- `src/usb_msc_driver.c` - dead code (replaced by net_glue.c)
- `src/mimic_fat.c` / `.h` / `.backup` / `.bak2` - old implementation
- `src/fat_mimic.c` / `.h` - redirect stubs
- `src/unicode.c` / `.h` - unused
- `lib/fat_mimic/` - keep for reference but not in build

## Commit Message
```
feat: HTTP web interface over USB, live dashboard

Replace USB Mass Storage with USB network device (ECM+RNDIS).
Device appears as network adapter, serves web interface at 192.168.7.1.

- lwIP TCP/IP stack with DHCP server
- HTTP server with file listing, download, live status API
- Auto-refreshing dashboard: state, altitude, pressure, pyro continuity
- Refactored pyro module (pyro.c) with clean API and raw ADC
- Pressure sensor on I2C1 (BMP280 GPIO 6/7, MS5607 GPIO 10/7)
- Flight state machine: PAD_IDLE, ASCENT, DESCENT, LANDED
- Four pyro modes: fallen, agl, speed, delay
- fat_mimic library preserved in lib/ for reference

Known issues:
- Pyro ADC readings need calibration (343 vs 400 too close)
- File upload not yet implemented
- Config editor not yet implemented
- Test mode not yet implemented
```
