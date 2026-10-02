# Unit & Integration Tests

Host-compiled tests for the Pyro flight computers. Every suite builds with the
build machine's C compiler, not the ARM toolchain, from any configured build
directory, pico board or host board. What stands in for the hardware depends on
the suite:
- `hal_test.c` implements `hal.h` on mock state for the flight software suites;
- `board_harness.c` boots that flight software as the hardware boots, with a
  noisy sensor;
- `test/fake_sdk/` stands in for the Pico SDK under a board file;
- `sim/hw/` and `sim/plant/` run MK1C's real backend against a model of its
  board.

A module with no platform links alone, so a module that reached for live state
would not link.

## Running

```bash
cd build

ninja host_tests          # 84 unit tests
ninja integration_tests   # 51 integration tests (OpenRocket data)
ninja closedloop_tests    # 31 closed-loop tests (7 configurations x 4 rockets, and profiles to 30 km)
```

Each target builds its binary into the build directory and runs it. A suite
that compiles with `-I boards/<PYRO_BOARD>` sees the selected board's
`board_pins.h`, so a board-specific assumption shows only when it runs in more
than one build.

| Target | Source | Covers |
|--------|--------|--------|
| `host_tests` | `test_flight_states.c` | the state machine a step at a time, telemetry, the config parser, USB |
| `integration_tests` | `test_integration.c` | an OpenRocket flight at 1 ms, logging, release, beeps, the serial ground test, brownout markers |
| `closedloop_tests` | `test_closedloop.c` | physics with deployment feedback, every pyro mode |
| `pressure_chain_tests` | `test_pressure_chain.c` | the pressure chain under noise (below) |
| `mach_tests` | `test_mach.c` | the Mach lockout (below) |
| `ground_test_tests` | `test_ground_test.c` | the ground test procedure through the flight software, booted as the hardware boots (DD-071) |
| `ground_test_seq_tests` | `test_ground_test_seq.c` | its schedule and switch (`ground_test_seq.c`, `ground_test_switch.c`), a 20 ms loop at a time on a fake clock |
| `brownout_tests` | `test_brownout.c` | the pad marker and the recovery decision matrix |
| `buzzer_tests` | `test_buzzer.c` | the pattern player driven through `hal_tasks_tick()`, and the beep store |
| `beep_tests` | `test_beep_codes.c` | the beep vocabulary, the personalities and their file format |
| `config_tests` | `test_config.c` | round trip, defaults, parser, serializer, merge |
| `config_persistence_tests` | `test_config_persistence.c` | save and load across a power cycle; reload only in PAD_IDLE |
| `pin_caps_tests` | `test_pin_caps.c` | MK1A's pin capability table, whatever board is selected |
| `pin_assign_tests` | `test_pin_assign.c` | pin assignment rules on MK1A's table: the release matrix, the common, the buzzer and ground test pads |
| `lua_tests` | `test_lua.c` | the Lua sandbox and its limits, on `boards/sim/lua_platform_sim.c`; only for a board with Lua |
| `http_tests` | `test_http.c` | the HTTP engine as a byte stream (below) |
| `http_work_tests` | `test_http_work.c` | who runs each HTTP work unit: core0's turn and budget, the worker's hold (DD-061) |
| `status_json_tests` | `test_status_json.c` | `/api/status` rendered from a snapshot, `status_json.c` linked alone |
| `net_stats_tests` | `test_net_stats.c` | `/api/net`'s rendering |
| `net_txq_tests` | `test_net_txq.c` | the USB network's transmit queue against a fake endpoint (DD-070) |
| `mac_random_tests` | `test_mac_random.c` | the MAC drawn from the RNG, and `/serial.txt` (DD-072) |
| `hr_log_tests` | `test_hr_log.c` | the high-rate log on a fake card and IMU, its tasks run by hand (`test/fake_rtos`, DD-077) |
| `flight_sim_tests` | `test_flight_sim.c` | the bench flight's atmosphere and profile, and its hold on the channels (DD-078) |
| `flight_log_tests` | `test_flight_log.c` | the flight log's binary records and the CSV they render as (DD-062) |
| `log_plan_tests` | `test_log_plan.c` | the three logging plans, in time order (DD-064) |
| `pressure_trace_tests` | `test_pressure_trace.c` | the ring `/api/pressure/trace` reads (DD-063) |
| `ms5607_tests` | `test_ms5607.c` | the MS5607 one-shot (below); only for a board with an MS5607 |
| `bmp280_tests` | `test_bmp280.c` | the BMP280 at the loop's rate (below) |
| `sensor_bringup_tests` | `test_sensor_bringup.c` | every board's sensor bring-up (below) |
| `board_pyro_tests` | `test_board_pyro_mk1b.c` | MK1B's pyro backend on the fake SDK (below) |
| `plant_tests` | `test_plant.c` | the three board plant models (below) |
| `board_pyro_mk1c_tests` | `test_board_pyro_mk1c.c` | MK1C's pyro backend on its modelled board (below); builds `plant_tests` first |
| `board_pyro_mk1a_tests` | `test_board_pyro_mk1a.c` | MK1A's pyro backend on the fake SDK: one channel at a time, the pulse on one clock, no verdict before a fresh check; `board_pyro_tests` runs it first |
| `board_selftest_tests` | `test_board_selftest.c` | the board-image stamp and what each verdict does at boot (DD-081) |
| `beep_out_tests` | `test_beep_out.c` | the altitude beep-out heard from the buzzer pin: `buzzer.c` alone, its digits read back |
| `physics_tests` | `test_physics.c` | the simulators' 1976 US Standard Atmosphere and the flight profile (DD-086) |
| `replay_tests` | `test_replay.c` | `pyro_sim --replay` on a CRLF log and on a log with no state column (DAT-08) |

`sim` is the simulator, not a test: see `sim/README.md`.

`test/web/run_web_tests.sh` runs the Playwright suite against the mock server
in all three modes, then `test_sim.spec.js` against `docs/` served as GitHub
Pages serves it (`static_server.js`): the browser demo's WASM build, flown from
power-on to LANDED. Run it from `test/web` after `npm install` and
`npx playwright install chromium`.

## The pressure chain under noise (T0)

`test_pressure_chain.c` (target `pressure_chain_tests`) flies the board through
`board_harness.c` with a sensor that is not perfect; `mach_tests` and
`ground_test_tests` boot through the same harness. The test HAL's sensor model
(`mocks.h`) adds seeded Gaussian noise (the MS5607's 1.2 Pa at OSR 4096),
truncates to whole pascals and range-checks as `hal_common.c` does, can inject
glitches, and can reproduce the MS5607's conversion timing with core0's flash
stalls. Every model is off unless a test turns it on, so the other suites see a
smooth signal. The board boots from BOOT_SETTLE with nothing primed, and each
flight's truth is integrated beside the firmware so detections can be timed
against it. The `BASELINE` lines it prints measure the rows of
`docs/outstanding_tasks.md`'s baseline table.

`support/noise_baseline.py <board-ip>` measures a board's real noise from
`raw_pa` and `pad_speed_cms` on `/api/status`.

The `test_T5_*` tests fly the detectors on the pressure fit (DD-048): apogee
over 1000 flights from 100 m to 9 km, SPEED and DELAY channels, two bad
readings under the drogue, the thrust flag, and σ through a brownout.
`mach_tests` adds `test_T5_ejection` (a bay charge of up to 5 kPa at the
drogue) and `test_T5_canopy_swing`, flown on a standard-atmosphere pad, where
the firmware's pressure altitude is the true height.

`test_T9_same_outcomes` flies the key scenarios with the sensor sampled every
11 ms, the MS5607's rate with its temperature read once in ten, and holds them
to the 50 Hz outcomes. `mock_one_shot` models the one-shot's schedule (DD-051,
DD-065, DD-066): a pressure every loop, stamped at the middle of its
conversion and taken at the next loop's top; `test_T9_one_shot_cadence` holds
the HAL to it. `test_T9_temperature_at_the_pressures_time` compensates each
pressure with the temperature line at its own time, for a die warming at
1 °C/s, and `test_T9_datasheet_example` holds the compensation to the
datasheet's worked example.

`ms5607_tests` runs the MS5607 one-shot's interrupt state machine
(`src/ms5607_oneshot.c`) on a fake bus and clock at the selected board's bus
rate: `test/ms5607_bus.h` stands in for `src/hal_common/ms5607_bus.h`, so the
suite puts `test/` first on the include path, and refuses to build for a board
with no MS5607 (MK1A). The stamp is the handler's, through a read held 60 ms and
a loop 50 ms late, and each pair is ready before the next loop.

`test_T8_replay` flies a flight, reads back its log, and replays the log's
readings through `sim/replay.c`; every event must land on the sample the
flight decided it on. The same code is `pyro_sim --replay <flight_log.csv>`
(build `sim` with `PYRO_BOARD=sim`), for real flights logged with
`log_rate=full` (DAT-08).

## Board files on the host (DD-053)

`test/fake_sdk/` stands in for the Pico SDK calls a board file makes: pins,
the ADC, I2C with fake devices (`fake_i2c.c`), and a clock that moves only
when the test moves it. Its sleeps and busy-waits fail the test that reaches
them, and so does a blocking I2C transfer to a device holding SCL low, which
on the SDK never returns (DD-069). The `_timeout_us` forms time out on a held
bus, or when the bound is shorter than the transfer at the bus's rate.
- `board_pyro_tests` runs MK1B's `pyro_board.c` against it: continuity read
  as MK1A reads it (DD-059), its settle waited out across loops, never inside
  one.
- `sensor_bringup_tests` runs every board's sensor bring-up —
  `src/pressure_single_sensor.c`, or MK1B's own `pressure_board.c` — each
  built against its own package (mk1a, mk1b, mk1c, reference), with fake
  sensors that refuse a transfer during their reset.
- `bmp280_tests` runs the BMP280 driver on MK1A's pins: one forced conversion
  a loop, taken at the next (DD-067), on a fake part timed from the
  datasheet's Table 13. A conversion a flash operation ran beside is marked
  (DD-068), and a held bus costs a bounded wait.

## MK1C's firing bus as measured (DD-054, DD-055, DD-056)

`plant_tests` holds `sim/plant/plant_mk1c.c` to the bench MK1C: U9's reverse
path and the bias sources' Schottky, fitted to the board's ADC and a scope,
from `boards/mk1c/pyro_sense.h`. `board_pyro_mk1c_tests` runs MK1C's real
backend — `boards/mk1c/pyro_board.c`, `pyro_measure.c`, `pyro_faults.c`,
`pyro_sequence.c` and `arm_pump.c` — against that model through `sim/hw/`,
whose PIO stand-in runs the pump's FIFO and bursts:
- between fires, a fitted match reads present and an absent one open, a
  shorted bus or high side latches, one bad tracking reading does not, and
  the only stimulus is the tracking test's bus bias;
- a fire arms the bus, fires on the measured bus, stops the pump at the
  gate, and verifies with the next tracking test, on 2S and 1S; each
  refusal, an aborted precharge, a stopped loop, a misfire, both channels
  in turn and a high side stuck on; flash waits out a fire;
- the test watches the plant every 25 us between loops, so a gate, a pump
  edge or U9 conducting is seen when it happens, not only at a loop.

## The Mach lockout's test ground (M0)

`test_mach.c` (target `mach_tests`) flies supersonic rockets through the board
harness (`board_harness.c`, shared with `pressure_chain_tests` and
`ground_test_tests`) against a plant, `sim/mach_plant.c`:
- an atmosphere set by the pad's temperature and elevation, through the
  tropopause;
- the speed of sound, and a rocket whose drag rises through Mach 1;
- static ports whose error is a Mach-dependent fraction of the dynamic
  pressure, stepping at Mach 1, of either sign, including one that makes a
  boost read as a descent;
- an ejection charge's bay pressure, and a sensor that drops out or sticks
  (`mock_sensor_stuck`).

The `test_M1_*` tests fly the Mach lockout (DD-049) on those profiles: the
flag before Mach 0.85, the release on every seed of the low-drag flight to
10 km, the fallback under ports too noisy to release, the peak from outside
the lock, and a sweep of the port error from 0.1 to 4 times either way.
`test_M1_design_note` recomputes `docs/mach_lockout.md`'s tables and fails if
the document's rows differ. `test_T9_mach_at_90hz` flies the fast profiles
again with a sample every 11 ms.

The `test_M2_*` tests fail the sensor in flight: stuck in coast, a 0.5 s
dropout across the apogee, a 2 s loss under the drogue, half a second of
impossible readings, and an hour of real noise that must never read as stuck.

`sim/mach_plant.c` is separate from `sim/physics.c`, the WASM module's rocket.
The report `test_M0_report` prints, for every profile at a 10 °C sea-level pad
and a 45 °C pad at 2000 m, when the drogue fired against the true apogee and
where the Mach lock let go.

## Code review 2026-09-24 regressions

Each finding the review proved, or that was found while fixing it, has a test
that fails on the code it was reported against. See
`docs/code_review_2026-09-24_resolution.md` for the mapping.

| Suite | Tests |
|-------|-------|
| config | `test_config_mode_none_round_trips`, `test_config_unknown_mode_serialises_as_none`, `test_config_default_name_is_not_truncated`, `test_config_writes_no_inert_keys` |
| unit | `test_REV03_*`, `test_REV04_*`, `test_REV07_*`, `test_REV08_*`, `test_REV09_*`, `test_REV12_*`, `test_REV_NEW_disabled_channel_is_not_a_fault` |
| integration | `test_FLT_LAUNCH_03_backdate`, `test_REV06_*`, `test_REV11_*`, `test_FLT_LOG_07_the_three_logging_plans`, `test_REV_NEW_log_header_*`, `test_BRN_INT_05_*` |
| closed-loop | `test_REV01_working_drogue_main_at_its_trigger`, `test_REV01_failed_drogue_brings_the_main_forward`, `test_REV05_*`, `test_REV16_*`; every mode suite asserts no forced main and AGL channels within 8 m |
| buzzer | driven through `buzzer_play_spec()`; `test_BUZ_ACT_04_*`, `test_BEEP_STORE_01/02` |
| web | flight data refresh, naming, erase and column parsing; unit conversion; name limit; one Save; no beep mode |

## The HTTP server as a byte stream (WEB-HTTP-01..05)

`test_http.c` (target `http_tests`) drives `http_conn.c` through a fake
transport: each request whole, split at every byte position, byte by byte and
in random pieces, drained through windows of 1 to 9 bytes. It covers framing,
long header blocks, HEAD, refusals (400/405/411/413/414/431), a peer that
closes early, a streamed body five times the ring into a sink that refuses,
a streamed response, a response larger than tx, `Expect: 100-continue`, which
connections want a service pass, and a unit that answers away from the service
call (DD-061). `support/http_stream_check.py <board-ip>` does the same over raw
sockets on a board.

## On USB (USB-01..07)

| Suite | Tests |
|-------|-------|
| unit | `test_USB_01..06`: no launch, no announcement, the beep-out and fault stop and resume, ignored in flight. `test_USB_07..10`: test mode flies and announces on USB, chirps on leaving, is off at boot, does not change in flight |
| integration | `test_USB_INT_01` (no marker on USB, dwell restarts on detach, one chirp per attach), `test_USB_INT_02` (no flight recovery on USB), `test_USB_INT_03` (test mode writes the marker on USB) |
| buzzer | `test_BUZ_PAT_10_usb_ok_is_one_double_chirp` |
| web | the USB row says grounded; test mode asks first, warns while on and turns off; declining leaves it off |

`test/web/hw_ui_check.js <board-ip>` runs the read-only UI checks against a real
board, and `support/api_check.py <board-ip>` the HTTP ones. Both need a board,
so CI does not run them.

## In CI

`.github/workflows/build.yml` runs on every push to `main` and
`lua-all-boards` and on pull requests to `main`. It builds MK1B (`build`),
MK1C, MK1A and the reference template. `lua_tests` runs in MK1C's build,
`integration_tests` in MK1B's, MK1C's and MK1A's, `ms5607_tests` in MK1B's and
MK1C's, and every other suite in MK1B's. It also runs `prove_core0.py` on each
board's ELF, `trace_check.py --counts`, `wait_check.py`,
`pressure_trace.py --selftest`, the Playwright suite in all three modes and
the browser simulator, and `scripts/sync_demo.sh --check`.

## Architecture

Flight code reaches the platform only through `hal.h`: no SDK header, no
`#ifdef`. The flight software suites link `hal_test.c`, which implements all of
`hal.h` on mock state.

```
test/
  hal_test.c             HAL implementation with mock state + in-memory filesystem
  mocks.h                Mock state declarations (pressure and sensor models, pyro, UART, time, stalls)
  board_harness.c/.h     the board booted and looped as main_hardware.c runs it
  ms5607_bus.h           the fake bus under src/ms5607_oneshot.c
  fake_sdk/              the Pico SDK under a board file: pins, ADC, clock, I2C with fake devices
  test_*.c               one file per suite (table above)
  web/                   Playwright UI tests, mock_server.js, static_server.js, hw_ui_check.js
test_data/
  open_rocket_export.csv OpenRocket simulation (228 points, 16s flight, 165ft peak)
  rockets.json           the closed-loop suite's four rocket profiles
```

## Unit Tests (test_flight_states.c) — 84 tests

The `test_REV*` and `test_USB_*` tests are in the sections above.

### Pressure & Altitude
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_SNS_ALT_01_pressure_to_altitude | SNS-ALT-01 | Altitude from pressure difference |
| test_SNS_ALT_02_altitude_accuracy_5000ft | SNS-ALT-01 | Within 0.5 % at 5000 ft |
| test_SNS_PRES_03_filter_init | SNS-PRES-03 | First reading unfiltered |
| test_SNS_PRES_02_filter_smoothing | SNS-PRES-02 | Step change smoothed |
| test_SNS_PRES_04_single_data_path | — | Readings reach the flight code only through the batch |
| test_buf_add | DAT-01 | Sample stored in ring buffer |
| test_DAT_01_buf_wraps | DAT-01 | Ring buffer wraps at `FLIGHT_BUF_SIZE` (64) |

### Boot Sequence
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_FLT_BOOT_01_reaches_pad_idle | FLT-BOOT-01 | Boot completes to PAD_IDLE |
| test_FLT_BOOT_08_calibrates_ground | FLT-BOOT-08 | Ground pressure from 10 readings |
| test_SNS_PRES_01_boot_no_sensor | SNS-PRES-01, FLT-BOOT-12 | No sensor: FAULT and system failure, before the pyro test |
| test_FLT_BOOT_11_waits_for_the_sensor_bringup | FLT-BOOT-11 | A bring-up still running is waited for |
| test_FLT_BOOT_12_bringup_that_never_ends_is_fault | FLT-BOOT-12 | A bring-up that never ends is FAULT |
| test_FLT_BOOT_04_settle_wait | FLT-BOOT-09 | 2.5s settle wait |
| test_FLT_BOOT_13_no_calibration_samples_is_fault | FLT-BOOT-13 | No calibration samples in 10 s: FAULT |
| test_FLT_BOOT_14_no_filesystem_is_fault | FLT-BOOT-14 | No filesystem: FAULT |
| test_FLT_BOOT_02_reads_config_at_boot | FLT-BOOT-02 | Config read at power-up |
| test_FLT_BOOT_03_writes_default_config | FLT-BOOT-03 | Defaults written when absent |

### Flight States
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_FLT_LAUNCH_02_stays_on_ground | FLT-LAUNCH-02 | No transition at ground level |
| test_FLT_LAUNCH_01_detects_ascent | FLT-LAUNCH-01, FLT-LAUNCH-07 | 100 ft and climbing, held 100 ms; T+0 at the first sample above 50 cm |
| test_GND_CAL_01_reference_follows_slow_drift | GND-CAL-01 | The ground reference follows weather drift |
| test_GND_CAL_02_reference_stops_tracking_when_the_rocket_moves | GND-CAL-03 | It holds still when the rocket moves |
| test_FLT_LAUNCH_08_ten_metres_is_no_longer_enough | FLT-LAUNCH-01 | 21 m is not a launch |
| test_FLT_LAUNCH_09_freezing_keeps_the_hundred_feet | GND-CAL-04 | The reference freezes at the pad's value |
| test_FLT_BOOT_10_no_false_launch_on_drift | FLT-LAUNCH-02 | ±25 Pa after calibration is not a launch |
| test_PAD_IDLE_noise_no_false_launch | FLT-LAUNCH-02 | Pad noise is not a launch |
| test_PYR_CONT_01_continuity_check | PYR-CONT-01 | Continuity checked, ADC stored |
| test_FLT_ASC_01_tracks_max_altitude | FLT-ASC-01 | Max altitude updated |
| test_FLT_ASC_04_arms_pyros | FLT-ASC-04 | Arm when speed < 10 m/s |
| test_FLT_ASC_07_arms_after_ten_metres_a_second | FLT-ASC-07 | A 9 m/s peak never arms; 11 m/s does |
| test_FLT_APO_01_detects_apogee | FLT-APO-01 | Over a free-fall peak: apogee between the peak and the 1.0001 drop |
| test_FLT_DESC_03_drogue_phase_from_rate | FLT-DESC-01 | A steady 20 m/s is DROGUE_DESCENT |
| test_FLT_DESC_04_chute_phase_from_rate | FLT-DESC-01 | A steady 5 m/s is CHUTE_DESCENT |
| test_FLT_LAND_01_detects_landing | FLT-LAND-01..03 | Stable + slow + low for 1s |
| test_FLT_LAND_06_stays_landed | FLT-LAND-06 | No state change after landing |

### Telemetry
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TEL_01_format | TEL-01 | $PYRO format |
| test_TEL_09_seq_increments | TEL-09 | Sequence increments |
| test_TEL_02_checksum | TEL-02 | XOR checksum |
| test_TEL_07_state_mapping | TEL-07 | PAD_IDLE..LANDED as 0-5 |
| test_telemetry_flags | TEL-08 | Both continuities and armed = 0x13 |
| test_TEL_06_altitude_and_speed | TEL-06 | Numeric fields |
| test_TEL_10_thrust_flag | TEL-10 | Thrust during ASCENT only |
| test_TEL_06_pyro_adc | TEL-06 | ADC values |
| test_TEL_08_all_flags | TEL-08 | All 6 flags = 0x3F |
| test_TEL_07_boot_maps_to_zero | TEL-07 | Boot states → 0 |

### Config Parser
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_CFG_02_parse_full | CFG-02 | All fields parsed |
| test_CFG_04_parse_all_modes | CFG-04 | delay/agl/fallen/speed |
| test_CFG_03_parse_all_units | CFG-03 | cm/m/ft |
| test_CFG_09_unix_newlines | CFG-09 | LF line endings |
| test_CFG_02_no_section_header | CFG-02 | No [section] needed |
| test_CFG_08_unknown_keys | CFG-08 | Unknown keys ignored |
| test_CFG_04_unknown_mode | CFG-04 | Unknown mode → 0 |
| test_CFG_02_empty_string | CFG-02 | Empty input safe |
| test_CFG_09_no_trailing_newline | CFG-09 | Last line without newline |
| test_CFG_07_id_truncated | CFG-07 | ID truncated to 8 chars |
| test_CFG_06_preserves_unset | CFG-06 | Partial config preserves fields |
| test_CFG_08_comment_lines | CFG-08 | Comments skipped |

## Integration Tests (test_integration.c) — 51 tests

Simulate a complete flight using OpenRocket trajectory data at 1ms resolution.

### How It Works
1. Load 228 data points from `test_data/open_rocket_export.csv`
2. Interpolate altitude at any time via binary search
3. Convert altitude to pressure (linear, 8.3 cm a pascal)
4. Start in PAD_IDLE with the pressure layer primed, and run app_tick() at 1ms
   intervals for ~18 seconds
5. Verify state transitions, pyro fires, telemetry, data log, buzzer

### Tests
The `test_REV*`, `test_FLT_LAUNCH_03_backdate`, `test_BRN_INT_05_*` and
`test_USB_INT_*` tests are in the sections above.

| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TST_02_sim_data_loads | TST-02 | CSV loads correctly |
| test_TST_02_interpolation | TST-02 | Altitude interpolation |
| test_SNS_ALT_01_roundtrip | SNS-ALT-01 | Pressure↔altitude round-trip |
| test_FLT_BOOT_01_all_states | FLT-PHASE-01..03 | Full state sequence |
| test_WEB_API_08_only_the_log_touches_the_filesystem_in_flight | WEB-API-08 | No other file call from launch to landing |
| test_WEB_API_08_spent_marker_waits_for_the_log | WEB-API-08 | The spent marker waits for the log's tail |
| test_FLT_APO_01_detected | FLT-APO-01 | Apogee detected, max altitude |
| test_PYR_MODE_01_fires | PYR-MODE-01 | Pyro fires |
| test_PYR_REL_01..06 | DD-019 | A channel released to Lua never fires, reads open, has no fault; its mocked calls are counted |
| test_BEEP_01..04 | BUZ-CODE-01, BUZ-CODE-02 | The pad's answers; unfixable outranks fixable |
| test_PAD_EXCL_01..05 | DD-020 | A pad has one owner, pyro or Lua |
| test_BUZ_07_03_lifecycle | BUZ-07, BUZ-03 | Buzzer stops on launch, plays on landing |
| test_DAT_04_events | DAT-04 | All event types logged |
| test_TEL_03_event_sentences | TEL-03 | $PYRO_APO, $PYRO_FIRE, $PYRO_LAND |
| test_TEL_04_json_format | TEL-04 | JSON with telem_format=1, no NMEA |
| test_TEL_01_output | TEL-01..02 | Telemetry with valid checksum |
| test_FLT_LAUNCH_01_timing | FLT-LAUNCH-01 | State timing bounds |
| test_FLT_LAND_04_duration | FLT-LAND-04 | Flight duration |
| test_DAT_06_csv_export | DAT-06, DAT-07 | CSV header + data + events |
| test_FLT_APO_04_no_apogee_before_armed | FLT-APO-04 | ARMED precedes APOGEE |
| test_FLT_ASC_03_06_thrust_and_arming | FLT-ASC-03, FLT-ASC-06 | Thrust flag; no arming above 10 m/s |
| test_PYR_ALT_02_cfg_range_beep | PYR-ALT-02 | Warning beep for an out-of-range value |
| test_GND_TEST_01..04 | GND-TEST-01..04 | Serial BEEP STATUS, ARM then FIRE, auto-disarm, PAD_IDLE only |
| test_FLT_LOG_07_the_three_logging_plans | FLT-LOG-07 | The three logging plans on one flight |
| test_BRN_INT_01..04 | FLT-BROWN-01, FLT-BROWN-02 | Pad marker after 10 s, once; a descending board rejoins its flight; a pad power-on calibrates |

## Closed-Loop Tests (test_closedloop.c) — 31 tests, 28+ flights

Physics simulation with pyro deployment feedback. Pyro fires change descent rate.

### Configurations × Rockets (7 × 4 = 28 flights)
| Config | Pyro 1 (Drogue) | Pyro 2 (Main) |
|--------|-----------------|---------------|
| Dly+Dly | Delay 0s | Delay 3s |
| Dly+AGL | Delay 0s | AGL 200ft |
| Dly+Fal | Delay 0s | Fallen 100ft |
| Dly+Spd | Delay 0s | Speed 30ft/s |
| AGL+AGL | AGL 400ft | AGL 200ft |
| Fal+AGL | Fallen 50ft | AGL 200ft |
| Spd+AGL | Speed 20ft/s | AGL 200ft |

The seven `test_PYR_MODE_*` tests fly one configuration each. Rockets (`test_data/rockets.json`): Estes Alpha III on A8-3 (about 65 m),
Big Bertha on C6-5 (175 m), Ventris on D12-5 (420 m), AeroTech Arreaux on
H73-8 (950 m). An AGL or FALLEN value above a small rocket's apogee is scaled
down to fit it.

### Safety Tests
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_PYR_SAFE_01_no_fire_without_continuity | PYR-SAFE-01 | No fire when continuity open |
| test_PYR_DEPLOY_01_low_flight_fires_both | PYR-DEPLOY-01 | Both channels deploy on one event, close together |
| test_SYS_DEPLOY_03_no_fire_during_ascent | SYS-DEPLOY-03 | No fire before apogee |
| test_PYR_FAULT_02_overcurrent_detection | PYR-FAULT-02 | FLAG pin fault logged |

### Other Tests
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TST_06_chute_effect | TST-06 | Chutes slow descent |
| test_TST_05_rocket_profiles | TST-05 | Each rocket's apogee within its tolerance |
| test_XIP_stall_pyro_timing | — | Pyro timing through 10 ms flash stalls |
| test_PYR_REFIRE_01_refire_ballistic | PYR-REFIRE-01 | One drogue retry on a ballistic descent |
| test_PYR_REFIRE_02_no_retry_when_opened | PYR-REFIRE-02 | No retry on a channel that opened |
| test_FLT_EMRG_01_shredded_drogue_fires_main | FLT-EMRG-01 | A failed drogue brings the main forward |
| test_FLT_EMRG_02_freefall_to_trigger_not_overridden | FLT-EMRG-02 | A planned free fall is left alone |
| test_FLT_MACH_02_fast_subsonic_flight_not_locked | FLT-MACH-02 | A fast subsonic flight is never locked |
| test_FLT_DESC_01_phase_without_pyros | FLT-DESC-01 | Phase follows the rate with no drogue configured |
| test_FLT_DESC_02_ballistic_reaches_landed | FLT-DESC-02 | A flight that deployed nothing still lands |

### High Flights (`flight_sim.h` profiles, DD-078, DD-079)

The profile does not answer the channels: it is the flight the board flies on
the bench. `fly_profile()` flies one through the flight software at 1 ms.

| Test | Requirement | Verifies |
|------|-------------|----------|
| test_SIM_02_the_flight_software_flies_a_30_km_profile | SIM-02, FLT-AIR-01 | Mach 2, apogee on time, main at 300 m, not forced |
| test_SIM_02_a_9_km_supersonic_flight_with_sensor_noise | FLT-MACH-02 | Inside the lockout's envelope with 3 Pa of noise: apogee within 3 s |
| test_SIM_02_30_km_with_sensor_noise_finds_apogee | SIM-02 | A known limit held as it stands: the lock never releases at 30 km with 3 Pa of noise, and the fallback finds apogee near the ground (`docs/high_altitude_flight.md`) |
| test_FLT_AIR_01_air_scale_is_the_pad_air_rate | FLT-AIR-01 | `pp_air_scale()` against the profile's own atmosphere, two pads |
| test_FLT_AIR_01_a_failed_drogue_is_seen_at_30_km | FLT-AIR-01, FLT-EMRG-01 | 90 m/s of pad air still forces the main |
| test_FLT_AIR_01_a_high_pad_flies_20_km | FLT-AIR-01 | From 1500 m, the main within 20 m of its trigger |
