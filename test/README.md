# Unit & Integration Tests

Host-compiled tests for the Pyro MK1B flight computer. All tests run on the build machine (not ARM target) using the HAL test implementation.

## Running

```bash
cd build

ninja host_tests          # 58 unit tests
ninja integration_tests   # 46 integration tests (OpenRocket data)
ninja closedloop_tests    # 25 closed-loop tests (60+ simulated flights)
```

The other host suites (`config_tests`, `config_persistence_tests`,
`buzzer_tests`, `beep_tests`, `brownout_tests`, `pin_assign_tests`,
`pin_caps_tests`, `plant_tests`, `lua_tests`) build and run the same way, and
`test/web/run_web_tests.sh` runs the Playwright suite against the mock server
in all three modes.

## The pressure chain under noise (T0)

`test_pressure_chain.c` (target `pressure_chain_tests`) is the one suite where
the sensor is not perfect. The test HAL's sensor model (`mocks.h`) adds seeded
Gaussian noise (the MS5607's 1.2 Pa at OSR 4096), truncates to whole pascals
and range-checks as `hal_common.c` does, can inject glitches, and can
reproduce the MS5607's conversion timing with core0's flash stalls. Every model
is off unless a test turns it on, so the other suites see the smooth signal
they always have. The board boots from BOOT_SETTLE with nothing primed, and
each flight's truth is integrated beside the firmware so detections can be
timed against it. The `BASELINE` lines it prints are the "Now" column of
`docs/outstanding_tasks.md`'s baseline table.

`support/noise_baseline.py <board-ip>` measures a board's real noise from
`raw_pa` and `pad_speed_cms` on `/api/status`.

The `test_T5_*` tests fly the detectors on the pressure fit (DD-048): apogee
over 1000 flights from 100 m to 9 km, SPEED and DELAY channels, two bad
readings under the drogue, the thrust flag, and σ through a brownout.
`mach_tests` adds `test_T5_ejection` (a bay charge of up to 5 kPa at the
drogue) and `test_T5_canopy_swing`, flown on a standard-atmosphere pad, where
the firmware's pressure altitude is the true height.

`test_T8_replay` flies a flight, reads back its log, and replays the log's
readings through `sim/replay.c`; every event must land on the sample the
flight decided it on. The same code is `pyro_sim --replay <flight_log.csv>`
(build `sim` with `PYRO_BOARD=sim`), for real flights.

## The Mach lockout's test ground (M0)

`test_mach.c` (target `mach_tests`) flies supersonic rockets through the board
harness (`board_harness.c`, shared with `pressure_chain_tests`) against a
plant, `sim/mach_plant.c`:
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
the document's rows differ.

`sim/physics.c` drives the browser simulator and is untouched. The report
`test_M0_report` prints, for every profile at a 10 °C sea-level pad and a
45 °C pad at 2000 m, when the drogue fired against the true apogee and where
the Mach gate let go.

## Code review 2026-09-24 regressions

Each finding the review proved, or that was found while fixing it, has a test
that fails on the code it was reported against. See
`docs/code_review_2026-09-24_resolution.md` for the mapping.

| Suite | Tests |
|-------|-------|
| config | `test_config_mode_none_round_trips`, `test_config_unknown_mode_serialises_as_none`, `test_config_default_name_is_not_truncated`, `test_config_writes_no_inert_keys` |
| unit | `test_REV03_*`, `test_REV04_*`, `test_REV07_*`, `test_REV08_*`, `test_REV09_*`, `test_REV12_*`, `test_REV18_*`, `test_REV_NEW_disabled_channel_is_not_a_fault` |
| integration | `test_FLT_LAUNCH_03_backdate` (now exact), `test_REV06_*`, `test_REV11_*`, `test_REV12_log_rate_*`, `test_REV_NEW_log_header_*`, `test_BRN_INT_05_*` |
| closed-loop | `test_REV01_working_drogue_main_at_its_trigger`, `test_REV01_failed_drogue_brings_the_main_forward`, `test_REV05_*`, `test_REV16_*`; every mode suite now asserts no forced main and AGL channels within 8 m |
| buzzer | rewritten against `buzzer_play_spec()`; `test_BUZ_ACT_04_*`, `test_BEEP_STORE_01/02` |
| web | flight data refresh, naming, erase and column parsing; unit conversion; name limit; one Save; no beep mode |

## The HTTP server as a byte stream (WEB-HTTP-01..05)

`test_http.c` (target `http_tests`) drives `http_conn.c` through a fake
transport: each request whole, split at every byte position, byte by byte and
in random pieces, drained through windows of 1 to 9 bytes. It covers framing,
long header blocks, HEAD, refusals (400/405/411/413/414/431), a peer that
closes early, a streamed body five times the ring into a sink that refuses,
a streamed response, a response larger than tx, and `Expect: 100-continue`.
`support/http_stream_check.py <board-ip>` does the same over raw sockets on a
board.

## On USB (USB-01..07)

| Suite | Tests |
|-------|-------|
| unit | `test_USB_01..06`: no launch, no announcement, the beep-out and fault stop and resume, ignored in flight. `test_USB_07..10`: test mode flies and announces on USB, chirps on leaving, is off at boot, does not change in flight |
| integration | `test_USB_INT_01` (no marker on USB, dwell restarts on detach, one chirp per attach), `test_USB_INT_02` (no flight recovery on USB), `test_USB_INT_03` (test mode writes the marker on USB) |
| buzzer | `test_BUZ_PAT_10_usb_ok_is_one_double_chirp` |
| web | the USB row says grounded; test mode asks first, warns while on and turns off; declining leaves it off |

`test/web/hw_ui_check.js <board-ip>` runs the read-only UI checks against a real
board, and `support/api_check.py <board-ip>` the HTTP ones.

All three run automatically in GitHub Actions CI on every push.

## Architecture

Flight code includes only `hal.h` — no platform-specific headers, no `#ifdef`. Tests link against `hal_test.c` which provides mock implementations of all HAL functions.

```
test/
  hal_test.c             HAL implementation with mock state + in-memory filesystem
  mocks.h                Mock state declarations (pressure, pyro, UART, time)
  test_flight_states.c   39 unit tests
  test_integration.c     12 integration tests
  test_closedloop.c      13 closed-loop simulation tests
test_data/
  open_rocket_export.csv OpenRocket simulation (228 points, 16s flight, 165ft peak)
```

## Unit Tests (test_flight_states.c) — 39 tests

### Pressure & Altitude
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_SNS_ALT_01_pressure_to_altitude | SNS-ALT-01 | Altitude from pressure difference |
| test_SNS_PRES_03_filter_init | SNS-PRES-03 | First reading unfiltered |
| test_SNS_PRES_02_filter_smoothing | SNS-PRES-02 | Step change smoothed |
| test_DAT_01_buf_add | DAT-01 | Sample stored in ring buffer |
| test_DAT_01_buf_wraps | DAT-01 | Ring buffer wraps at 4096 |

### Boot Sequence
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_FLT_BOOT_01_reaches_pad_idle | FLT-BOOT-01 | Boot completes to PAD_IDLE |
| test_FLT_BOOT_08_calibrates_ground | FLT-BOOT-08 | Ground pressure from 10 readings |
| test_SNS_PRES_01_boot_no_sensor | SNS-PRES-01 | Graceful handling, no sensor |
| test_FLT_BOOT_04_settle_wait | FLT-BOOT-04 | 2.5s settle wait |

### Flight States
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_FLT_LAUNCH_02_stays_on_ground | FLT-LAUNCH-02 | No transition at ground level |
| test_FLT_LAUNCH_01_detects_ascent | FLT-LAUNCH-01 | Transition above 10m |
| test_PYR_CONT_01_continuity_check | PYR-CONT-01 | Continuity checked, ADC stored |
| test_FLT_ASC_01_tracks_max_altitude | FLT-ASC-01 | Max altitude updated |
| test_FLT_ASC_04_arms_pyros | FLT-ASC-04 | Arm when speed < 10 m/s |
| test_FLT_ASC_07_arms_after_ten_metres_a_second | FLT-ASC-07 | A 9 m/s peak never arms; 11 m/s does |
| test_FLT_APO_01_detects_apogee | FLT-APO-01 | Over a free-fall peak: apogee between the peak and the 1.0001 drop |
| test_FLT_LAND_01_detects_landing | FLT-LAND-01..03 | Stable + slow + low for 1s |
| test_FLT_LAND_06_stays_landed | FLT-LAND-06 | No state change after landing |

### Telemetry
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TEL_01_format | TEL-01 | $PYRO format |
| test_TEL_09_seq_increments | TEL-09 | Sequence increments |
| test_TEL_02_checksum | TEL-02 | XOR checksum |
| test_TEL_07_state_mapping | TEL-07 | State IDs 0-3 |
| test_TEL_08_flags | TEL-08 | Flag encoding |
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

## Integration Tests (test_integration.c) — 12 tests

Simulate a complete flight using OpenRocket trajectory data at 1ms resolution.

### How It Works
1. Load 228 data points from `test_data/open_rocket_export.csv`
2. Interpolate altitude at any time via binary search
3. Convert altitude to pressure (inverse barometric formula)
4. Run app_tick() at 1ms intervals for ~18 seconds
5. Verify state transitions, pyro fires, telemetry, data log, buzzer

### Tests
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TST_02_sim_data_loads | TST-02 | CSV loads correctly |
| test_TST_02_interpolation | TST-02 | Altitude interpolation |
| test_SNS_ALT_01_roundtrip | SNS-ALT-01 | Pressure↔altitude round-trip |
| test_FLT_BOOT_01_all_states | FLT-PHASE-01..03 | Full state sequence |
| test_FLT_APO_01_detected | FLT-APO-01 | Apogee detected, max altitude |
| test_PYR_MODE_01_fires | PYR-MODE-01 | Pyro fires |
| test_BUZ_07_03_lifecycle | BUZ-07, BUZ-03 | Buzzer stops on launch, plays on landing |
| test_DAT_04_events | DAT-04 | All event types logged |
| test_TEL_01_output | TEL-01..02 | Telemetry with valid checksum |
| test_FLT_LAUNCH_01_timing | FLT-LAUNCH-01 | State timing bounds |
| test_FLT_LAND_04_duration | FLT-LAND-04 | Flight duration |
| test_DAT_06_csv_export | DAT-06, DAT-07 | CSV header + data + events |

## Closed-Loop Tests (test_closedloop.c) — 13 tests, 32+ flights

Physics simulation with pyro deployment feedback. Pyro fires change descent rate.

### Configurations × Altitudes (7 × 4 = 28 flights)
| Config | Pyro 1 (Drogue) | Pyro 2 (Main) |
|--------|-----------------|---------------|
| Dly+Dly | Delay 0s | Delay 3s |
| Dly+AGL | Delay 0s | AGL 200ft |
| Dly+Fal | Delay 0s | Fallen 100ft |
| Dly+Spd | Delay 0s | Speed 30ft/s |
| AGL+AGL | AGL 400ft | AGL 200ft |
| Fal+AGL | Fallen 50ft | AGL 200ft |
| Spd+AGL | Speed 20ft/s | AGL 200ft |

Altitudes: 100ft, 500ft, 5000ft, 100km (Karman line)

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
| test_TST_05_karman_apogee | TST-05 | 100km flight reaches target |
