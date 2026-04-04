# Pyro MK1B Firmware - Current Status

_Last updated: 2026-04-04 — v2.1.28 DMA UART TX, all blocking UART writes eliminated_

## 🔬 Hardware Serial Log Analysis (2026-04-04)

### Summary of Boot Cycles Captured

| FW Version | Boots | Behavior | False Launch? | Network? |
|------------|-------|----------|---------------|----------|
| 2.1.17 | ~8 | False launches on every boot | ❌ YES | N/A (no diag) |
| 2.1.24 | 2 | Stable PAD_IDLE, no false launch | ✅ FIXED | No diag |
| 2.1.26 | 2 | Stable PAD_IDLE, no false launch | ✅ FIXED | ❌ HTTP broken |

### FW 2.1.17 — False Launch Bug (now fixed)

Every boot cycle followed the same failure pattern:
1. BOOT_SETTLE → BOOT_CONT → BOOT_CAL → PAD_IDLE (normal, ~3.5s)
2. **PAD_IDLE → ASCENT in 0.5–6s** (false trigger)
3. ASCENT → DESCENT in **50ms** (impossible physics)
4. DESCENT → LANDED in ~1s
5. LANDED shows pressure slowly converging from millions of Pa to ~102,280 Pa (correct)
6. Device reboots after ~120–520s in LANDED (watchdog or power cycle)

Raw pressure during PAD_IDLE was wildly wrong: `2,689,215 Pa`, `-43,966 Pa`, `3,733,478 Pa` — 
values orders of magnitude outside the 30,000–110,000 Pa barometric range. The IIR filter's 
`dt = now - last_sample` overflowed when batch timestamps predated `last_sample`, producing 
`alpha` values that amplified 7 Pa of real noise into 1.8M Pa filter jumps.

**Buzzer impact during 2.1.17**: The rapid false-launch cycle meant:
- 10 startup chirps begin playing
- `buzzer_stop()` fires almost immediately (ASCENT triggered in <2s)
- `buzzer_set_altitude(800000cm)` starts altitude beep-out for 8000m (8,0,0,0 → 8 + 10 + 10 + 10 beeps)
- Extremely long garbled altitude pattern plays until watchdog reboot
- Cycle repeats: truncated chirps → cut off → garbled altitude beeps → reboot

### FW 2.1.24 — Pressure Processing Fix Verified ✅

Both boot cycles showed:
- Stable PAD_IDLE for 900+ seconds (entire captured duration)
- Pressure readings: 102,260–102,300 Pa (realistic, normal atmospheric drift)
- Altitude readings: 0–255 cm (sensor noise at ground level, normal)
- No false state transitions
- $PYRO telemetry: consistent 2Hz output with valid sensor data
- Pyro ADC values: 26–30 range on both channels (continuity readings)

**Buzzer behavior (2.1.24)**: Working correctly:
- 10 chirps (30ms on / 30ms off each) at startup
- Status code plays TWICE (BUZ-02 compliant), then stops
- Buzzer goes silent after ~3–4 seconds total
- Code depends on pyro continuity status (likely 2-1 or 3-1 if no ematches connected)

### FW 2.1.26 — Network Diagnostics Reveal HTTP Failure ❌

Both boot cycles showed stable flight software (no false launches), but HTTP is non-functional:

```
[10003] NET: tud=1 rx=67  drop=0 tx=8  txf=3   http=0
[50003] NET: tud=1 rx=90  drop=0 tx=9  txf=7   http=0
[70003] NET: tud=1 rx=95  drop=0 tx=9  txf=10  http=0
```

Second boot (worse):
```
[10005] NET: tud=1 rx=61  drop=0 tx=11 txf=2   http=0
[60005] NET: tud=1 rx=95  drop=0 tx=12 txf=120 http=0
[100005] NET: tud=1 rx=104 drop=0 tx=12 txf=270 http=0
[120005] NET: tud=1 rx=121 drop=0 tx=12 txf=308 http=0
```

Key observations:
- **tud=1**: USB device ready, host has enumerated the NCM device
- **rx growing**: Frames being received from host (ARP, DHCP, pings)
- **http=0**: Zero HTTP connections accepted — TCP listener not serving
- **txf growing rapidly**: USB TX failures (tud_network_xmit returning false)
- **tx stuck at 9–12**: Only a handful of frames ever transmitted successfully
- **drop=0**: No frames dropped at the receive stage

**Diagnosis**: The device receives network frames but cannot transmit responses. The
growing `txf` count indicates USB NCM TX buffer saturation or a protocol handshake issue.
The host keeps retrying (rx grows), but since responses fail, TCP connections never complete.
This affects ARP replies, DHCP, and TCP SYN-ACK — meaning HTTP can never be established.

**Next steps for network fix**:
1. Check if lwIP is properly calling `tud_network_xmit()` at the right time
2. Verify NCM TX is not being called before USB enumeration completes
3. Add timing guard: don't attempt TX until tud_ready() has been true for >1s
4. Check if the `__wfe()` sleep is preventing USB interrupt servicing

## ✅ Completed

### Event-Driven State Machine
- Transition table: 8 rows define every possible state change
- Detectors (per-tick work) → Events → Actions (side effects)
- 4 boot states + 4 flight states, no implementation leakage
- See IMPLEMENTATION.md for transition table

### Hardware Abstraction Layer
- hal.h: 15 functions covering all hardware interactionded
- Zero #ifdef in flight code (flight_states.c, telemetry_formatter.c, buzzer.c)
- Three implementations: hardware, test, simulation
- Same source compiles for Pico, host tests, and WASM

### Config System
- INI parser with 12 unit tests
- Web config editor (all fields: id, name, units, beep, pyro1, pyro2)
- Beep code 4-3 for out-of-range altitude settings
- 8,000m altitude clamp on all altitude-based settings

### Telemetry
- Format 0 (default): `$PYRO` NMEA sentences with XOR checksum, backwards-compatible
- Format 1: JSON newline-delimited objects (selectable via `telem_format=1` in config.ini)
- Event sentences: `$PYRO_APO`, `$PYRO_FIRE`, `$PYRO_LAND` on key flight events
- 10Hz ASCENT/DESCENT, 1Hz PAD_IDLE/LANDED
- `telemetry_formatter.c` — standalone protocol-independent module

### Buzzer (v2 Task 7) ✅ Done — Hardware Verified
- `async_task_t`-based pattern player — runs as a parallel state machine alongside pressure sampling
- Three states: BZ_IDLE → BZ_ENCODE_CODE/ALTITUDE → BZ_PLAYING
- Step table built at request time; `hal_tasks_tick()` fires each step at its deadline
- `buzzer_play_code(code, uint8_t repeat_count)` — v2 API with strict repeat semantics:
  `0`=infinite, `1`=play once, `N`=play N times (strict BUZ-02 compliance)
- `loop_start` field: chirps (×10) play only on the FIRST pass; subsequent loops restart at digits
- `flight_states.c` calls `buzzer_play_code(code, 2)` — plays status code twice then stops [BUZ-02]
- `buzzer_set_code()` / `buzzer_set_altitude()` kept as inline shims for existing callers
- `hal_buzzer_task_register()` added to all three HALs; hardware HAL registers into `hw_tasks[]`
- Test and sim HALs drive the task via `hal_tasks_tick()` for full pattern testing
- **12 unit tests**: BUZ-PAT-01..07 (tone counts, BUZ-02 repeat, chirp-once), BUZ-ACT-01..03 (lifecycle, stop, repeat)
- **Hardware observation**: With 2.1.17, buzzer was garbled due to rapid false-launch state cycling.
  With 2.1.24+, buzzer correctly plays startup chirps + status code × 2 then goes silent.

### Pressure Processing Module ✅ Done — Hardware Verified
- `pressure_processing.h/.c` — standalone IIR filter + hypsometric altitude module
- `pp_init()`, `pp_feed(raw_pa, now_ms)`, `pp_read(&sample)` — ring-buffered producer/consumer
- `pp_start_cal()` / `pp_cal_sample()` / `pp_end_cal()` — boot calibration pipeline
- `pp_test_prime(ground_pa)` — test-only helper to skip calibration
- `pp_filter_pressure()` / `pp_pressure_to_altitude_cm()` — public utility functions
- All detectors consume altitude via `pp_read()` — single data path from sensor to state machine
- `hal_tasks_tick()` feeds `pp_feed()` at ~50Hz (matching real BMP280/MS5607 sample rate)
- **Hardware verified**: v2.1.24 shows stable 102,260–102,300 Pa readings with no overflow

### Batch Pressure Processing (v2 Task 8) ✅ Done
- `flight_process_samples(ctx, batch)` — processes a 5-sample 50Hz batch in one call
- `detect_ascent()` / `detect_descent()` gate at 20ms (50Hz); `detect_pad_idle()` gates at 10ms
- `main_hardware.c` calls `dispatch_state()` via `pp_read()` ring (v2.1.24+)
- Polled fallback (`dispatch_state()`) retained for test/sim compatibility

### Fire-and-Forget Flight Log (v2 Task 9) ✅ Done
- `hal_log_start(cfg, ground_pa)` opens `flight_log.csv` at launch; `hal_log_stop()` closes at landing
- `hal_log_sample(time_ms, pa, alt, state, thrust, event)` appends one CSV line — non-blocking
- Three implementations: hardware (LittleFS streaming), sim (host filesystem), test (in-memory)
- Incremental ring-buffer CSV logger (`csv_flush_safe/step/track`) retired from all call sites

### Data Logging
- 4096-sample ring buffer, events tagged on samples
- Streaming CSV export to littlefs after landing
- /api/flight.csv serves actual flight data

### Web Interface
- 4-tab UI: Status, Config, Flight Data, Update
- Pending config warning, range validation, guided editor
- GitHub Pages demo + interactive WASM simulation

### Simulator
- Pyro black box (sim/main_sim.c) — no physics knowledge
- CLI physics driver (sim/sim_cli.c)
- Browser simulation (docs/sim.html) with Web Audio buzzer

### Pyro Fault Detection
- AP2192 FLAG pin monitoring (GPIO 17/18, active-low with pull-ups)
- Post-fire continuity verification (ADC re-check 500ms after fire)
- Fault events logged to flight buffer
- Beep codes 2-3/2-4 (P1 fault/verify) and 3-3/3-4 (P2 fault/verify)

### Ground Test Commands (v2 Task 2) ✅ Done
- Serial commands via TRRS jack (UART0 RX)
- 3-second ARM→FIRE window with automatic disarm timeout
- All commands rejected outside PAD_IDLE state

### Network Diagnostics (v2.1.25–26) ✅ Done
- `net_glue.c`: rx/tx/drop counters on every frame, `tud_network_init_cb()` logged
- `http_server.c`: **Fixed `conn_alloc()` bug** — was scanning 4 of 8 pool slots; now uses all 8
- `http_server.c`: HTTP accept counter for connection tracking
- `main_hardware.c`: 10-second `NET:` health heartbeat
- Periodic `$PYRO` state sentences suppressed on UART (events still sent)

### Testing (107 C + 22 web)
- 43 unit, 22 integration, 15 closed-loop, 12 buzzer, 15 config
- 4 safety-critical closed-loop tests
- 22 Playwright web UI tests
- cppcheck/MISRA, clang-format, pmccabe in CI
- Requirements traced to integration tests (TRACEABILITY.md)

### Build & CI
- Firmware, tests, simulator, WASM all from same source
- GitHub Actions on every push
- Auto-versioning, A/B OTA images

## 🔨 Known Issues

### RESOLVED: HTTP Server Not Accepting Connections (v2.1.26) ✅
- **Symptom**: `http=0` in all NET diagnostics; `txf` grows rapidly; ARP incomplete
- **Root cause**: **MAC address mismatch** between USB descriptor string (`STRID_MAC = "020284006A00"`)
  and `tud_network_mac_address` array (`{0x02,0x02,0x84,0x6A,0x96,0x00}`). The host's ECM driver
  expected device MAC `02:02:84:00:6A:00` (from descriptor), but lwIP sent ARP replies with source
  MAC `02:02:84:6A:96:01` (from array + XOR). Host rejected responses as invalid source.
- **Fix**: Aligned `tud_network_mac_address` in `net_glue.c` to match `STRID_MAC`:
  `{0x02, 0x02, 0x84, 0x00, 0x6A, 0x00}`
- **Verified**: HTTP 200 in 24ms, `/api/status` returns valid JSON, device up in 3s after flash

### RESOLVED: False Launch on Boot ✅
- Fixed in v2.1.24 via `pressure_processing.c` dt_ms clamp
- **Hardware verified**: 8 boot cycles on 2.1.17 showed false launches; 4 boot cycles on 2.1.24+ showed none

### RESOLVED: conn_alloc() Pool Bug ✅  
- Fixed in v2.1.25 — was scanning 4 of 8 pool slots
- May not have been the primary HTTP issue (see HTTP failure above)

### Minor: Playwright Reboot Cycle Test
- Skipped in CI (needs log access)

## 🚧 v2 Architecture Task Status

| Task | Description | Status |
|------|-------------|--------|
| v2-1 | X-macro config system (`config_fields.h`) | ✅ Done |
| v2-2 | HAL config API + Ground test commands | ✅ Done |
| v2-3 | CPU sleep (`hal_sleep_until_event`) | ⏸️ Disabled v2.1.27 |
| v2-4 | Serial readline HAL | ✅ Done |
| v2-5 | Autonomous pressure sampling | ✅ Done |
| v2-6 | Telemetry formatter module | ✅ Done |
| v2-7 | Buzzer pattern player (async task) | ✅ Done + HW verified |
| v2-8 | Batch flight_process_samples() | ✅ Done |
| v2-9 | Fire-and-forget hal_log_sample() | ✅ Done |
| v2-10 | ISR UART TX ring buffer | 🔄 Replaced by v2-13 |
| v2-11 | Network diagnostics + conn_alloc fix | ✅ Done |
| v2-12 | Fix HTTP TX failures (MAC mismatch) | ✅ Done v2.1.27 |
| v2-13 | DMA UART TX (replaces v2-10 ring buffer) | ✅ Done v2.1.28 |

## 🔨 Next Priority
1. **Flash v2.1.28** — device needs manual BOOTSEL recovery (unplug, hold BOOTSEL, replug)
2. **Re-enable WFE sleep** — MAC mismatch was the real root cause, not `__wfe()`; re-enable for power savings
3. Run Playwright web tests against live hardware to verify full stack
4. Long-duration soak test (network + UART stability over hours)
