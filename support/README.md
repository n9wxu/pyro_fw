# Pyro MK1B Support Tools

Scripts for flashing, testing, and managing the Pyro MK1B flight computer.

## Scripts

| Script | Purpose |
|--------|---------|
| `test_network.py` | Comprehensive network/API test suite with TUI |
| `flash_picotool.sh` | Flash bootloader + app via picotool |
| `upload_fw.sh` | OTA firmware update via HTTP |
| `upload_www.sh` | Upload web files to device |

## Test Suite (test_network.py)

### Quick Start

```bash
# Interactive mode — guided setup
python3 support/test_network.py

# Direct mode
python3 support/test_network.py 192.168.7.1

# Full test suite, don't stop on failure
python3 support/test_network.py --all 192.168.7.1
```

### Features

- **Live TUI** — split screen with test checklist (left) and UART log (right)
- **Color coded** — current test bold, passed green, failed red, pending grey
- **UART monitoring** — live serial output via pyserial
- **Timestamped logging** — complete timeline of commands, responses, and UART
- **Pre-test diagnostics** — mDNS, ping, interfaces, DNS-SD logged automatically
- **Log analyzer** — diagnose failures from a log file without device access
- **Interactive mode** — guided setup for new users, shows repeat command
- **picotool integration** — reset device before testing

### Options

```
  --all           Run all tests even if one fails (default: stop on first failure)
  --uart PORT     Monitor UART during tests (e.g. /dev/tty.usbmodem201202)
  --reset         Reset device via picotool before testing
  --repeat N      Repeat test suite N times (stress testing)
  --plain         Plain text output (no TUI, for CI/scripts)
  --log FILE      Write timestamped diagnostic log
  --analyze FILE  Analyze a previous log file
```

### Test Coverage

| Section | Tests |
|---------|-------|
| Connectivity | ping |
| HTTP API | status JSON, config INI, CORS headers |
| File Serving | index.html, app.js, style.css with size validation |
| File Consistency | 5x repeated downloads, size comparison |
| Error Handling | 404 response |
| Parallel Connections | 6 simultaneous status + 6 mixed requests |
| Sequential Requests | 20 status + 10 file downloads |
| mDNS/DNS-SD | _pyro._tcp service discovery |
| picotool | vendor reset interface visibility |

### UART Monitoring

Requires [pyserial](https://pypi.org/project/pyserial/):

```bash
pip install pyserial
```

If the port is busy (e.g. open in another terminal), the test continues without UART and shows a clear error message.

### Log Analysis

After a test run with `--log`, analyze the results:

```bash
python3 support/test_network.py --analyze pyro_test_20260305.log
```

The analyzer shows:
- System info (OS, Python version)
- Pre-test diagnostics (network state, mDNS, interfaces)
- Test results with pass/fail counts
- Failure context (±2 seconds of log entries around each failure)
- Auto-diagnosis of common issues (TCP PCB exhaustion, memory failures, mDNS problems)

### Remote Debugging

When a user reports a problem:

1. Ask them to run: `python3 support/test_network.py --all --uart /dev/tty.usbmodem* --log test.log`
2. Have them send the `.log` file
3. Analyze with: `python3 support/test_network.py --analyze test.log`

The log contains everything needed for diagnosis without device access.

## Flashing

### Via picotool (recommended)

```bash
./support/flash_picotool.sh
```

Requires the device to be running firmware with the vendor reset interface. Forces BOOTSEL, loads bootloader + app, reboots, and waits for network.

### Via OTA

```bash
./support/upload_fw.sh
```

Uploads firmware over HTTP to the A/B bootloader's download slot. Device reboots automatically.

### Via BOOTSEL (first time only)

1. Hold BOOTSEL, plug in USB
2. Copy `build/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2`
3. Hold BOOTSEL again
4. Copy `build/pyro_fw_c.uf2`

## Web Files

```bash
./support/upload_www.sh
```

Uploads `www/` directory contents to the device's littlefs filesystem.
