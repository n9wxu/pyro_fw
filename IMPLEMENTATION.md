# Flight Controller Implementation

## Architecture

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
| `src/flight_controller.c` | Main loop, state machine, `main()` |
| `src/http_server.c` | HTTP server, API endpoints, OTA handler |
| `src/net_glue.c` | TinyUSB RNDIS ↔ lwIP bridge, DHCP/DNS |
| `src/pyro.c` | Pyro channel control, continuity, fire |
| `src/pressure_sensor.c` | Unified sensor interface |
| `src/littlefs_driver.c` | Flash driver for littlefs |
| `src/device_status.h` | Shared status struct for HTTP dashboard |
| `www/` | Web interface files (uploaded to littlefs) |

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
- **mDNS:** Advertises as `pyro.local` (RFC 6762) with conflict resolution
- **DNS-SD:** Registers `_pyro._tcp` service for automatic discovery
- **CORS:** API endpoints include `Access-Control-Allow-Origin: *`
- **Single device:** http://pyro.local/ just works
- **Multiple devices:** Data collection server browses `_pyro._tcp` to find all trackers

### Versioning
- `VERSION` file contains semantic version (major.minor.patch)
- Patch auto-increments on every build via `scripts/gen_version.sh`
- `src/version.h` generated at build time with `FW_VERSION` and `FW_BUILD_DATE`
- Version reported in UART boot message, HTTP API, and web dashboard

### Debugging
The VS Code launch config loads both bootloader and app ELFs:
1. `pico_fota_bootloader.elf` → flash at 0x10000000
2. `pyro_fw_c.elf` → flash at 0x1000A000 (Slot A)

### Build
```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```

### Scripts
| Script | Purpose |
|--------|---------|
| `upload_fw.sh` | OTA firmware upload via curl |
| `upload_www.sh` | Upload web files to device |
| `test_network.py` | Comprehensive network/API test suite |
| `scripts/gen_version.sh` | Auto-generate version.h at build time |
