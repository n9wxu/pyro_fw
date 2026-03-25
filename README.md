# pyro_fw
Firmware for the Pyro MK1B Rocket Flight Computer

**[🚀 Try the web interface — no hardware needed](https://n9wxu.github.io/pyro_fw/)**

## Features
- **Dual Pyrotechnic Control** - AP2192 power switches with overcurrent detection
- **Four Firing Modes** - Fallen distance, AGL altitude, descent speed, timed delay
- **Fault Detection** - Overcurrent monitoring and post-fire verification
- **Fire-and-Forget Flight Log** - 50Hz streaming CSV to LittleFS via `hal_log_sample()`; ISR-driven async flush; non-blocking
- **Real-time Telemetry** - `$PYRO` NMEA or JSON format via UART (non-blocking ISR TX ring), 10Hz in flight; event sentences at apogee, fire, landing
- **Pressure Sensing** - MS5607-02BA03 or BMP280 (auto-detected)
- **Altitude Calculation** - Integer-only barometric formula
- **Continuity Checking** - ADC oversampling for pre-flight verification
- **Web Interface** - Live dashboard via USB network (192.168.7.1)
- **OTA Firmware Updates** - A/B bootloader with automatic rollback
- **WASM Simulation** - Full flight computer runs in browser with physics engine
- **Status Beep Codes** - Field-diagnosable error reporting
- **Altitude Beep-out** - Max altitude announced after landing (m/ft/ft100)
- **Configurable Deployment** - INI file configuration via USB or web
- **Ground Test Interface** - Safe pre-launch pyro test via serial jack

## Hardware
- **MCU:** Raspberry Pi Pico (RP2040)
- **Pressure Sensor:** MS5607-02BA03 or BMP280 (auto-detected)
- **Pyro Control:** AP2192 dual channel power switch (1.5A per channel)
- **Storage:** littlefs on Pico flash (984KB)
- **Interfaces:** USB, UART0, I2C0

## Flight States
1. **PAD_IDLE** - Ground calibration, ascent detection (+10m in <1s)
2. **ASCENT** - Boost/coast phase, 50Hz logging, apogee detection
3. **DESCENT** - Pyro deployment, 50Hz logging, landing detection
4. **LANDED** - Post-flight altitude beep-out; flight log finalized

Apogee is an event within ASCENT, not a separate state. Pyros arm only after vertical speed drops below 10 m/s.

## Pyro Modes
| Mode | Description |
|------|-------------|
| fallen | Distance fallen from apogee (in configured units) |
| agl | Altitude above ground level (in configured units) |
| speed | Downward vertical speed threshold (configured units/second) |
| delay | Seconds after apogee event |

## Configuration
Edit `config.ini` on the USB drive (appears when connected to PC):

```ini
[pyro]
id=PYRO001
name=My Rocket
pyro1_mode=delay
pyro1_value=0
pyro2_mode=agl
pyro2_value=300
units=m
beep_mode=digits
```

**Pyro Modes:**
- **fallen:** Distance fallen from apogee (in configured units)
- **agl:** Altitude above ground level (in configured units)
- **speed:** Downward vertical speed threshold (configured units/second)
- **delay:** Seconds after apogee event

**Units:** `cm` (centimeters), `m` (meters), `ft` (feet) - applies to pyro values and altitude reporting

**Beep Mode:** `digits` (each digit beeped) or `hundreds` (value / 100, then each digit)

**Defaults:** If `config.ini` is missing, defaults are created automatically.

## Status Beep Codes
Two-digit codes (1-5 beeps per digit):
- 100ms beep, 200ms pause within digit
- 300ms pause between digits
- 500ms pause between codes
- Repeats after 1 second

| Code | Meaning |
|------|---------|
| 1-1  | All systems good ✓ |
| 2-1  | Pyro 1 open circuit |
| 2-2  | Pyro 1 short circuit |
| 2-3  | Pyro 1 fault during fire |
| 2-4  | Pyro 1 failed to open after fire |
| 3-1  | Pyro 2 open circuit |
| 3-2  | Pyro 2 short circuit |
| 3-3  | Pyro 2 fault during fire |
| 3-4  | Pyro 2 failed to open after fire |
| 4-1  | Pressure sensor failure |
| 4-2  | Filesystem failure |
| 4-3  | Pyro altitude setting exceeds sensor limit |
| 5-5  | Critical system failure |

## Pin Assignments
### Communication
- **GPIO 0:** UART0 TX (telemetry output)
- **GPIO 1:** UART0 RX
- **GPIO 8:** I2C0 SDA (pressure sensor) / Test input (active low, internal pull-up)
- **GPIO 9:** I2C0 SCL (pressure sensor)

### Pressure Sensor I2C
- **MS5607:** GPIO 10 (SDA), GPIO 7 (SCL)
- **BMP280:** GPIO 6 (SDA), GPIO 7 (SCL)
- Auto-detected at boot with I2C pin release between attempts

### Pyrotechnic Control
- **GPIO 15:** Common enable (master switch)
- **GPIO 21:** Pyro 1 enable (AP2192 EN1)
- **GPIO 22:** Pyro 2 enable (AP2192 EN2)
- **GPIO 17:** Pyro 1 fault (AP2192 FLAG1, active low)
- **GPIO 18:** Pyro 2 fault (AP2192 FLAG2, active low)
- **GPIO 26 (ADC0):** Pyro 1 continuity sense
- **GPIO 27 (ADC1):** Pyro 2 continuity sense

### User Interface
- **GPIO 25:** Onboard LED (status)
- **GPIO 16:** Buzzer (GPIO on/off, external circuit produces tone)

## Data Logging
The primary in-flight record is written by `hal_log_sample()` (v2-9) to `flight_log.csv` in LittleFS — one CSV line per pressure sample from launch to landing.

- **Primary log:** `flight_log.csv` in LittleFS, 50Hz during ASCENT/DESCENT, streaming append (ISR-flushed every 200ms)
- **Events:** LAUNCH, ARMED, APOGEE, PYRO1_FIRE, PYRO2_FIRE, LANDING tagged inline as an extra CSV column
- **Fields:** `time_ms, pressure_pa, altitude_cm, state, thrust, event`
- **RAM ring buffer:** 64 entries × 16 bytes = 1KB (retained for launch-backdate and `/api/flight.csv` HTTP endpoint)
- **CSV export:** `/api/flight.csv` serves `flight_log.csv` directly from LittleFS

## Telemetry (UART0)
**Format:** `$PYRO` NMEA sentences (default) or JSON (set `telem_format=1` in config.ini), 115200 baud

```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags_hex,p1adc,p2adc,batt,temp*XX\r\n
```

- **Rate:** 10Hz during ASCENT/DESCENT, 1Hz during PAD_IDLE/LANDED
- **State:** 0=PAD_IDLE, 1=ASCENT, 2=DESCENT, 3=LANDED
- **Flags:** bit0=P1_CONT, bit1=P2_CONT, bit2=P1_FIRED, bit3=P2_FIRED, bit4=ARMED, bit5=APOGEE
- **Checksum:** XOR of all bytes between `$` and `*`

**Event sentences (NMEA format):**
```
$PYRO_APO,flight_time_ms,max_alt_cm*XX\r\n
$PYRO_FIRE,channel,flight_time_ms,alt_cm*XX\r\n
$PYRO_LAND,flight_time_ms,max_alt_cm*XX\r\n
```

**JSON format** (set `telem_format=1` in config.ini):
```json
{"t":"state","seq":42,"state":1,"alt":1500,"vel":-200,"press":95000}
{"t":"apogee","alt":15240,"time":8500}
{"t":"fire","ch":1,"alt":15240,"time":8501}
```

**Example (NMEA):**
```
$PYRO,42,1,1,150000,-200,150000,95000,8500,13,48,52,0,0*4A
```

## Ground Test Interface

From a PC or handset connected to UART0 RX (TRRS jack, 115200 baud), send plain-text commands while the device is in PAD_IDLE:

| Command | Description |
|---------|-------------|
| `STATUS` | Report continuity state for both channels |
| `BEEP STATUS` | Replay last continuity beep code from buzzer |
| `BEEP ALT <n>` | Beep a specific altitude value (cm, for field calibration) |
| `ARM 1` or `ARM 2` | Arm the selected pyro channel for 3 seconds |
| `FIRE 1` or `FIRE 2` | Fire the armed channel (must ARM first within 3s) |

Responses are NMEA-style `$GT,...*XX` sentences with XOR checksum:
```
$GT,ARMED,1*3A
$GT,FIRED,1*38
$GT,ERR,not_pad_idle*XX
```

All commands are silently ignored if the device is not in PAD_IDLE (ASCENT/DESCENT/LANDED). The ARM→FIRE sequence has a 3-second auto-disarm timeout for safety.

## Safety Features
1. **Pre-flight Continuity Check** - ADC oversampling (256 samples, 16-bit effective)
2. **Two-part Pyro Safety** - Common enable + channel enable
3. **AP2192 Protection** - Hardware current limiting, thermal shutdown, short circuit protection
4. **Fault Monitoring** - FLAG pins monitored during fire
5. **Post-fire Verification** - ADC confirms pyro opened successfully
6. **Ground Test Safety** - 3-second ARM→FIRE window, commands rejected in flight states

## Continuity Detection
- **Circuit:** 100kΩ pull-up to 3.3V on each pyro output
- **Method:** ADC oversampling (256 samples) for 16-bit effective resolution
- **Thresholds:**
  - Open: ADC > 60000 (3.3V)
  - Good: ADC 1-100 (very low but non-zero)
  - Short: ADC = 0 (exactly 0V)

## WASM Simulation — Use in Other Projects

The Pyro MK1B flight computer and physics engine compile to **WebAssembly**, enabling any web project to run closed-loop rocket flight simulations entirely in the browser. The WASM module contains the real firmware state machine, pyro logic, and telemetry — identical to hardware.

### Quick Start

```bash
# Build the WASM module (requires Emscripten SDK)
./scripts/build_wasm.sh
```

Copy 3 files into your project:
```
docs/wasm/pyro.js       ← Emscripten glue (generated)
docs/wasm/pyro.wasm     ← WASM binary (generated)
docs/wasm/pyro-sim.js   ← ES module API wrapper
```

Then in your JavaScript:
```javascript
import { createPyroSim } from './wasm/pyro-sim.js';

const sim = await createPyroSim();
sim.init("pyro1_mode=delay\npyro1_value=0\nunits=ft\n");
sim.setContinuity(1, 50, true, false);
sim.setContinuity(2, 50, true, false);
sim.physics.init(1524);  // 5000 ft target apogee

// Closed-loop: physics feeds pressure → flight computer fires pyros → physics deploys chutes
for (let t = 0; t <= 120000; t++) {
    if (sim.pyroFireCount > 0 && sim.lastFireChannel === 1) sim.physics.deployDrogue();
    if (t >= 2000) sim.physics.step((t - 2000) / 1000);
    sim.setPressure(sim.physics.pressurePa);
    sim.clearPyroFiring();
    sim.tick(t);
    if (sim.state === 7) break;  // LANDED
}
console.log("Apogee:", sim.physics.apogeeM.toFixed(0), "m");
```

### Integration Documentation

| Document | Audience | Contents |
|----------|----------|----------|
| **[sim/INTEGRATION.md](sim/INTEGRATION.md)** | AI assistants & developers | Step-by-step integration guide, full API reference, troubleshooting, code examples |
| **[sim/README.md](sim/README.md)** | Developers | Architecture diagram, API tables, build instructions, C library usage |
| **[docs/sim.html](https://n9wxu.github.io/pyro_fw/sim.html)** | Anyone | Interactive browser demo — working example of WASM integration |

### For C/C++ Projects (No WASM)

The simulation also works as a plain C library:
```bash
cc -I sim/ -I src/ sim/physics.c sim/main_sim.c sim/hal_sim.c \
   src/flight_states.c src/telemetry_formatter.c src/buzzer.c src/config.c \
   my_test.c -lm -o my_test
```

See `sim/pyro_sim.h` and `sim/physics.h` for the C API.

## Building
Requires Pico SDK 2.2.0 or later:
```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```

This produces:
- `build/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2` — A/B bootloader
- `build/pyro_fw_c.uf2` — application firmware
- `build/pyro_fw_c_fota_image.bin` — OTA update image

## Flash Layout
```
0x000000  Bootloader           36 KB   (pico_fota_bootloader)
0x009000  Info block             4 KB   (swap flags, rollback state)
0x00A000  App Slot A           512 KB   (active firmware)
0x08A000  App Slot B           512 KB   (OTA download target)
0x10A000  LittleFS             984 KB   (config, web files, flight data)
0x200000  End of flash
```

## Initial Flash (one-time via BOOTSEL)
1. Hold BOOTSEL, plug in USB
2. Copy `pico_fota_bootloader.uf2` to the Pico drive
3. Hold BOOTSEL again
4. Copy `pyro_fw_c.uf2` to the Pico drive
5. Upload web files: `./support/upload_www.sh`

## Flashing via picotool
After the initial flash, use picotool for all subsequent flashing (no BOOTSEL button needed):
```bash
./support/flash_picotool.sh
```
This forces BOOTSEL via the vendor reset interface, loads both bootloader and app, reboots, and waits for the network to come up.

## OTA Firmware Updates
For routine updates without reflashing the bootloader:
```bash
./support/upload_fw.sh
```
Or use the "Firmware Update" button in the web interface at http://pyro.local/.

The A/B bootloader ([pico_fota_bootloader](https://github.com/JZimnol/pico_fota_bootloader)) provides:
- **Safe updates** — new firmware is written to the inactive slot while the device keeps running
- **Automatic rollback** — if new firmware doesn't call `pfb_firmware_commit()`, the bootloader reverts on next reboot
- **No bricking** — a failed or interrupted OTA leaves the current firmware intact

## Web Interface
Connect the Pico via USB. It appears as a network adapter with DHCP.

- **Address:** http://pyro.local/ (or http://192.168.7.1/)
- **Status tab:** Live state, altitude, speed, pressure in configured units
- **Config tab:** Guided pyro editor with range warnings and tips
- **Flight Data tab:** Summary, CSV download, altitude graph
- **Update tab:** Firmware/web upload, GitHub release checker
- **APIs:** /api/status, /api/config, /api/reboot, /api/ota, /api/flight.csv (all CORS enabled)

### Network Architecture
Each tracker advertises a `_pyro._tcp` DNS-SD service via mDNS.

**Single device:** Browse to http://pyro.local/ — just works.

**Multiple devices:** A data collection server browses for `_pyro._tcp` to discover all attached trackers automatically.

## Testing

A comprehensive test suite validates the device's network stack, HTTP server, mDNS, and picotool integration.

```bash
# Interactive mode — guided setup for new users
python3 support/test_network.py

# Quick test against direct IP
python3 support/test_network.py 192.168.7.1

# Full suite with UART monitoring and log file
python3 support/test_network.py --all --uart /dev/tty.usbmodem201202 --log test.log

# Analyze a log file from a remote user
python3 support/test_network.py --analyze test.log
```

The test suite features a live TUI with colored results, UART monitoring via pyserial, timestamped diagnostic logging, and automated failure analysis. See [support/README.md](support/README.md) for full documentation.

## Support Tools

All development and deployment scripts are in the `support/` directory:

| Script | Purpose |
|--------|---------|
| `support/test_network.py` | Network/API test suite with TUI |
| `support/flash_picotool.sh` | Flash via picotool (no BOOTSEL button) |
| `support/upload_fw.sh` | OTA firmware update (local build) |
| `support/upload_www.sh` | Upload web files |
| `support/update_from_release.py` | Update firmware from GitHub releases |

## Releasing

Releases are automated via GitHub Actions:

- **Every push to main** builds firmware and uploads artifacts
- **Git tags** (`v*`) create a GitHub Release with downloadable binaries

To create a release:
```bash
# Set version in VERSION file
echo "2.0.0" > VERSION
git add VERSION && git commit -m "release: v2.0.0"
git tag v2.0.0
git push && git push --tags
```

GitHub Actions will build and publish `pyro_fw_c.uf2`, `pyro_fw_c_fota_image.bin`, and `pico_fota_bootloader.uf2` as release assets.

## Self-Update from GitHub

Update a device to the latest release directly from GitHub:
```bash
# Check for updates
python3 support/update_from_release.py --check

# Update to latest
python3 support/update_from_release.py

# Update to specific version
python3 support/update_from_release.py --version 2.0.0

# Update device at specific address
python3 support/update_from_release.py --host 192.168.7.1
```

The tool checks the device's current version, compares with GitHub releases, downloads the OTA binary, pushes it to the device, and verifies the update.

## Usage
1. **Power on** - System initializes
2. **Wait 3 seconds** - Continuity check performed
3. **Listen for beeps** - 1-1 = good, others = fault
4. **Connect USB** - Open http://pyro.local/ to view status or edit config
5. **Arm system** - Place in rocket
6. **Launch** - Automatic detection and logging
7. **Retrieve** - Download flight data from web interface

## Development Status

**99 C tests, 22 web UI tests — all passing.**

The firmware is feature-complete and fully implements the v2 autonomous I/O architecture. All v2 milestones are complete:

- **v2-1/2:** X-macro config system; `hal_config_load/save`
- **v2-2b:** Ground test serial command interface (`ground_test.c`)
- **v2-3:** CPU sleep (`hal_sleep_until_event` / `__wfe`)
- **v2-4:** Autonomous 50Hz pressure sampling (`async_task.h` + `hal_pressure_fifo_*`)
- **v2-5:** Telemetry formatter module (dual NMEA/JSON, event sentences)
- **v2-7:** Buzzer parallel state machine (`buzzer_play_code/altitude`, `async_task_t`-driven, no main-loop involvement)
- **v2-8:** Batch `flight_process_samples()` at 50Hz
- **v2-9:** Fire-and-forget `hal_log_sample()` with ISR-flushed streaming LittleFS writer
- **v2-10:** Non-blocking UART TX ring buffer (UART0 ISR, 512-byte SPSC queue)

See [ARCHITECTURE_V2.md](ARCHITECTURE_V2.md) and [STATUS.md](STATUS.md) for implementation details.

See [SPECIFICATION.md](SPECIFICATION.md) for detailed requirements and implementation notes.

## License
See LICENSE file for details.
