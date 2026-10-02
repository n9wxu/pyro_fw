# Pyro Support Tools

Scripts for flashing, testing, and managing the Pyro flight computers (MK1A,
MK1B, MK1C).

Each board is a point-to-point USB network at `192.168.<n>.1`, the third octet
derived from its flash chip's id; `register_board.py` lists them. `pyro.local`
finds one board. Scripts that default to `192.168.7.1` reach only a board
whose octet happens to be 7: pass the address.

`flash_picotool.sh` and `test_network.py` run picotool from
`~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool`, where the Pico VS Code
extension installs it.

## Scripts

| Script | Purpose |
|--------|---------|
| `test_network.py` | Comprehensive network/API test suite with TUI |
| `http_stream_check.py` | The HTTP server over raw sockets: requests split byte by byte, bodies in later writes, coalesced requests, odd-chunk uploads round-tripped, parallel fetches past a stalled connection (rewrites /www/app.js and index.html with the local copies) |
| `api_check.py` | Bench check of every HTTP route, the config round trip, the pad diagnosis, silence on USB, and test mode (erases the flight log; about 15 s in test mode) |
| `pressure_trace.py` | Every pressure conversion a board makes, judged for the faults a sample rate hides (DD-063) |
| `noise_baseline.py` | A still board's pressure noise and pad speed noise, from `/api/status` |
| `register_board.py` | Record the attached boards in `boards/BOARD_REGISTRY.json` and report subnet collisions |
| `bench_flight.py` | Fly a profile on a board through its flight software, channels mocked (DD-078) |
| `hr_log.py` | Decode a high-rate log from the SD card into CSV (DD-077) |
| `prove_core0.py` | Prove from the linked ELF that no flight-critical root can reach a wait core1 can hold |
| `trace_check.py` | Check the requirements, decisions, traceability and living documents against each other and the code |
| `wait_check.py` | Fail if any source in `src/` or `boards/` sleeps or busy-waits (DD-053) |
| `flash_picotool.sh` | Flash bootloader + app via picotool |
| `upload_fw.sh` | OTA firmware update via HTTP |
| `upload_www.sh` | Upload web files to device |
| `install.py` | Interactive installer: BOOTSEL, picotool or OTA, then the web files |
| `update_from_release.py` | Update firmware from GitHub releases |

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

With options and no host it tests `pyro.local`.

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
| HTTP API | status JSON, config INI, same-origin headers |
| File Serving | index.html, app.js, style.css with size validation |
| File Consistency | 5x repeated downloads, size comparison |
| Error Handling | 404 response |
| Parallel Connections | 6 simultaneous status + 6 mixed requests |
| Sequential Requests | 20 status + 10 file downloads |
| mDNS/DNS-SD | _pyro._tcp service discovery (macOS `dns-sd`) |
| picotool | reboots the board through the vendor reset interface |

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

## Bench Checks

Each capture takes the board's address. `api_check.py`, `http_stream_check.py`
and `noise_baseline.py` want the board in PAD_IDLE.

```bash
python3 support/api_check.py <board-ip>           # every route; erases the flight log
python3 support/http_stream_check.py <board-ip>   # the server as a byte stream
python3 support/noise_baseline.py <board-ip> [seconds]
python3 support/pressure_trace.py <board-ip> [seconds [save.json]]
python3 support/pressure_trace.py --analyze save.json...
python3 support/pressure_trace.py --selftest      # the judge, on made-up traces
```

`api_check.py` restores the pyro settings it changes before it exits.
`http_stream_check.py` turns test mode on and off.

`noise_baseline.py` polls for 60 s by default and reports the RMS of `raw_pa`
about a straight-line fit, so weather drift is not counted, and of
`pad_speed_cms`, the speed the launch detector reads. The host tests assume
the MS5607's 1.2 Pa at OSR 4096. Nobody may touch the board while it runs.

`pressure_trace.py` polls `/api/pressure/trace` for 60 s by default and
reports per board: the rate and the spread of the intervals, repeated codes
against the count chance gives, rejects and missed slots, conversions a flash
operation ran beside (DD-068, counted, not a fault), the lag from the driver's
stamp to the loop, and the noise. A record lost to the ring fails the capture,
not the sensor.

`test/web/hw_ui_check.js <board-ip>` runs the web UI's read-only checks on a
board.

### A flight on the bench

```bash
python3 support/bench_flight.py <board-ip>                       # 3 km, the board's defaults
python3 support/bench_flight.py <board-ip> --apogee 30000 --boost 4
python3 support/hr_log.py --fetch <board-ip> hr0001              # the high-rate log after it
```

`bench_flight.py` turns test mode on, starts the profile (`POST
/api/sim/flight`), and follows `/api/sim` and `/api/status` until the board
lands. It fails a flight whose drogue is not within 4 s of the profile's
apogee, whose main did not fire, or during which the loop overran or a flash
operation was refused. From the start every fire on the board is mocked until
it reboots, and the board must be rebooted to fly again. `/api/sim` also
reports the newest pressure fit and the Mach lock, for a flight that goes
wrong.

## Board Registry

```bash
python3 support/register_board.py              # discover, record, report
python3 support/register_board.py --dry-run    # report without writing
python3 support/register_board.py --host ADDR  # probe this address instead of discovering
```

Discovery reads the host's own interfaces: a board hands the host
`192.168.<n>.2`, so each such address is a board at `192.168.<n>.1`. The
registry is a record, not an allocator; deleting it loses nothing that running
this again with the boards attached does not rebuild. Two boards on one octet
cannot share a host: POST 12 hex digits to `/api/serial` on one of them and
reboot it. The same fixes two boards whose flash chips report one id, as two
MK1Cs' do: a record is keyed by hw_id and MAC, and the report lists shared ids.

## Static Checks (CI)

```bash
python3 support/prove_core0.py --core1 core1_main build/pyro_fw_mk1b.elf
python3 support/trace_check.py [--counts]
python3 support/wait_check.py
```

`prove_core0.py` rebuilds the call graph from `arm-none-eabi-objdump` (on
`PATH` or under `~/.pico-sdk/toolchain/`) and exits 1 if a flight-critical root
reaches an unbounded wait, or, with `--core1`, if core1's entry acquires
anything. `--root` adds a root. It follows an indirect call only through a
table whose symbol ends in `_vt`. A pass means no root calls such a wait: spin
lock acquires are inlined, so they are reported by address, never failed. CI
runs it on MK1B's, MK1C's and MK1A's ELF.

`trace_check.py` holds every requirement ID the code, tests and living
documents cite to `REQUIREMENTS.md`, every DD to `DECISIONS.md`, every live
requirement to a row in `TRACEABILITY.md`, and every test a row names to the
code. A function a living document names in backticks with its parentheses,
in this file too, must exist in the tree.

`wait_check.py` keeps a per-file ratchet of calls not yet converted, which only
shrinks; it is empty. Bounded waits on a bus or a hardware handshake are
outside it.

## Flashing

### Via picotool (recommended)

```bash
./support/flash_picotool.sh [build_dir]
```

Loads the bootloader and the first `pyro_fw_mk1*.uf2` in `build_dir` (default
`build`). A board already in BOOTSEL is loaded as it is; one running firmware
with the vendor reset interface is forced into BOOTSEL first. It reboots the
board and waits up to 15 s for `192.168.7.1` to answer a ping, so on any other
octet the warning at the end means nothing.

### Via OTA

```bash
./support/upload_fw.sh [path_to_bin] [host]
```

Uploads `build/pyro_fw_c_fota_image.bin` (by default) to `http://pyro.local/api/ota`,
the A/B bootloader's download slot, with the `X-Pyro: 1` header the board asks
of a POST (DD-083). Device reboots automatically. The image name does not
carry the board: send each board its own build's. An image for another board
refuses to arm or fire and declines to commit, so the bootloader puts the
previous one back (DD-081).

### Via BOOTSEL (first time only)

1. Hold BOOTSEL, plug in USB
2. Copy `build/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2`
3. Hold BOOTSEL again
4. Copy `build/pyro_fw_<board>.uf2`

### Installer

```bash
python3 support/install.py
```

Run from a checkout, or from a release's `pyro-support.zip` with the board's
images beside it. The board is the one the device reports on `/api/status`
(`--host`, default `192.168.7.1`); with no device answering, `--board` names
it, and a `--board` that disagrees with the device is refused. With a board
answering it offers OTA with the web files, the web files alone, or a full
picotool flash; without one, a BOOTSEL copy or picotool. It takes the newest
`fw_<board>.uf2`, `fw_<board>_fota.bin` and `fw_<board>_bootloader.uf2`
anywhere under the directory above `support/`, or else a local build's
`pyro_fw_<board>.uf2` and the `pyro_fw_c_fota_image.bin` beside it. It exits
non-zero when anything did not install.

## Web Files

```bash
./support/upload_www.sh [host]
```

Uploads `www/` directory contents to the device's littlefs filesystem, one
`POST /www/<name>` each (default host `pyro.local`), then `VERSION` as
`/www/version.txt`, which the page shows as the web files' version. The
installer uploads the same files.

## Self-Update from GitHub Releases

```bash
# Check if update is available
python3 support/update_from_release.py --check

# Update to latest release
python3 support/update_from_release.py

# Update to specific version
python3 support/update_from_release.py --version 2.0.0

# Force update even if same version
python3 support/update_from_release.py --force
```

`--host` names the board (default `192.168.7.1`), and `--beta` includes
prereleases.

The tool:
1. Queries the device's current version **and board** via `/api/status`
2. Checks GitHub releases API for the latest (or specified) version
3. Downloads `fw_<board>_fota.bin` from the release -- the image for
   the board the device reported, since the boards do not take each other's
   firmware (mk1b is a 2 MB part, mk1a and mk1c are 16 MB). If the device
   does not report a board, pass `--board`. Releases up to v2.2.0 carried a
   single unqualified `pyro_fw_c_fota_image.bin`, which is still accepted as
   a fallback.
4. Pushes it to the device via `/api/ota`
5. Waits for reboot and verifies the new version

A release carries `fw_<board>.uf2`, `fw_<board>_fota.bin` and
`fw_<board>_bootloader.uf2` for MK1A, MK1B and MK1C
(`.github/workflows/release.yml`).
Configure the GitHub repo by editing `REPO` at the top of the script.
