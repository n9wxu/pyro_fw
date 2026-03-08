# Flight Controller Implementation (v1.2.0)

## Architecture

### Unified State Machine
All system behavior runs through a single `dispatch_state()` function:
```
BOOT_FILESYSTEM → BOOT_I2C_SETTLE → BOOT_SENSOR_DETECT → BOOT_PYRO_INIT →
BOOT_CONTINUITY → BOOT_STABILIZE → BOOT_CALIBRATE → BOOT_MDNS →
PAD_IDLE → ASCENT → DESCENT → LANDED
```

Boot states are non-blocking (timer-based delays). `tud_task()` and `net_service()` run every main loop iteration, ensuring USB and networking work during boot.

### Flash Layout (2MB)
```
0x000000  Bootloader           36 KB   pico_fota_bootloader
0x009000  Info block             4 KB   swap flags, rollback state
0x00A000  App Slot A           512 KB   active firmware
0x08A000  App Slot B           512 KB   OTA download target
0x10A000  LittleFS             984 KB   config, web files, flight data
0x200000  End
```

### Key Source Files
| File | Purpose |
|------|---------|
| `src/flight_controller.c` | Main loop only (~80 lines) |
| `src/flight_states.c` | All boot + flight state functions, helpers |
| `src/flight_states.h` | Types, context struct, declarations |
| `src/telemetry.c` | $PYRO NMEA telemetry formatting |
| `src/buzzer.c` | Buzzer: status codes + altitude beep-out |
| `src/buzzer.h` | Buzzer API and beep code definitions |
| `src/http_server.c` | HTTP server, API endpoints, OTA handler |
| `src/net_glue.c` | TinyUSB RNDIS ↔ lwIP bridge, DHCP/DNS, mDNS |
| `src/reset_interface.c` | Vendor reset interface for picotool |
| `src/usb_descriptors.c` | USB composite: ECM/RNDIS + vendor reset |
| `src/pyro.c` | Pyro channel control, continuity, fire |
| `src/pressure_sensor.c` | Unified sensor interface (MS5607/BMP280) |
| `src/littlefs_driver.c` | Flash driver for littlefs |
| `src/device_status.h` | Shared status struct for HTTP dashboard |
| `src/arch/cc.h` | lwIP platform hooks |
| `www/` | Web interface files (uploaded to littlefs) |

### Testing
- 27 unit tests + 11 integration tests = 38 total
- Host-compiled with mocks (no ARM target needed)
- Integration tests use OpenRocket simulation data at 1ms resolution
- See [test/README.md](test/README.md) for comprehensive test plan

### OTA Update Flow
1. HTTP POST to `/api/ota` with firmware .bin body
2. `pfb_firmware_commit()` — prevent rollback of current firmware
3. Incoming data buffered to 4KB sector boundaries
4. Each sector: erase + program to Slot B (incremental, USB-safe)
5. `pfb_mark_download_slot_as_valid()` — mark Slot B ready
6. `pfb_perform_update()` — watchdog reboot, bootloader swaps A↔B
7. New firmware calls `pfb_firmware_commit()` on boot to confirm

If step 7 doesn't happen before next reboot, bootloader rolls back.

### HTTP API
| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | JSON: state, altitude, pyro, version, uptime |
| GET | `/api/config` | Plain text config.ini from littlefs |
| POST | `/api/config` | Upload new config.ini |
| POST | `/api/ota` | Firmware update (raw .bin body) |
| GET | `/api/flight.csv` | Download flight data |
| GET | `/` | Web dashboard (served from littlefs /www/) |
| POST | `/www/<file>` | Upload web file to littlefs |

All API responses include CORS headers for cross-origin access.

### Networking
- **Addressing:** 192.168.7.1/24 (DHCP assigns PC 192.168.7.2)
- **MAC:** Unique per board, derived from `pico_unique_board_id`
- **mDNS:** Advertises as `pyro.local` (RFC 6762) with conflict resolution
- **DNS-SD:** Registers `_pyro._tcp` service for automatic discovery
- **CORS:** API endpoints include `Access-Control-Allow-Origin: *`
- **Single device:** http://pyro.local/ just works
- **Multiple devices:** Data collection server browses `_pyro._tcp` to find all trackers

### USB Composite Device
- **VID:** 0x2E8A (Raspberry Pi), **PID:** 0x4002
- **Config 1 (RNDIS):** Network (Windows) + vendor reset
- **Config 2 (ECM):** Network (macOS/Linux) + vendor reset
- **Vendor reset:** picotool can reboot to BOOTSEL or application
- **Deferred reset:** Reset request sets flag, main loop executes (protects I2C)

### Versioning
- `VERSION` file contains semantic version (major.minor.patch)
- Patch auto-increments on local builds via `scripts/gen_version.sh`
- CI builds use VERSION as-is (no increment when `CI_BUILD=1`)
- Release tags set VERSION from tag name (v2.0.0 → VERSION=2.0.0)
- `src/version.h` generated at build time with `FW_VERSION` and `FW_BUILD_DATE`
- Version reported in UART boot message, HTTP API, and web dashboard

### CI/CD (GitHub Actions)
- **build.yml:** Builds on push to main, uploads artifacts
- **release.yml:** Creates GitHub Release with firmware binaries on `v*` tags
- Release assets: `pyro_fw_c.uf2`, `pyro_fw_c_fota_image.bin`, `pico_fota_bootloader.uf2`, `pyro-mk1b-support.zip`

### Debugging
- VS Code launch config loads both bootloader + app ELFs
- lwIP debug infrastructure in `src/arch/cc.h` (UART printf)
- Debug flags: `LWIP_DEBUG`, `MEM_DEBUG`, `MEMP_DEBUG`, `PBUF_DEBUG`
- UART heartbeat: 1Hz `[uptime] alive pa=pressure`

### Build
```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```

### Scripts
| Script | Purpose |
|--------|---------|
| `support/install.py` | Interactive installer for build artifacts |
| `support/flash_picotool.sh` | Flash via picotool (no BOOTSEL button) |
| `support/upload_fw.sh` | OTA firmware upload via curl |
| `support/upload_www.sh` | Upload web files to device |
| `support/update_from_release.py` | Update firmware from GitHub releases |
| `support/test_network.py` | Comprehensive network/API test suite |
| `scripts/gen_version.sh` | Auto-generate version.h at build time |

### Telemetry ($PYRO NMEA)
```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags_hex,p1adc,p2adc,batt,temp*XX\r\n
```
- State: 0=PAD_IDLE, 1=ASCENT, 2=DESCENT, 3=LANDED
- Flags: bit0=P1_CONT, bit1=P2_CONT, bit2=P1_FIRED, bit3=P2_FIRED, bit4=ARMED, bit5=APOGEE
- XOR checksum between $ and *
- 10Hz during ASCENT/DESCENT, 1Hz otherwise

### Buzzer
- **Startup:** 10 chirps (30ms) → 500ms pause → status code × 2 → stop
- **Status codes:** two-digit (1-5 beeps each), 100ms beep, 200ms gap, 300ms digit gap
- **Altitude beep-out (LANDED):** 2s pause → 500ms long beep → digits (0=10 beeps) → repeat
