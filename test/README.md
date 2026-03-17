# Unit & Integration Tests

Host-compiled tests for the Pyro MK1B flight computer. All tests run on the build machine (not ARM target) using the HAL test implementation.

## Running

```bash
cd build

ninja host_tests          # 39 unit tests
ninja integration_tests   # 12 integration tests (OpenRocket data)
ninja closedloop_tests    # 13 closed-loop tests (32+ simulated flights)
```

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
| test_FLT_APO_01_detects_apogee | FLT-APO-01 | Transition when speed ≤ 0 |
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
| test_PYR_SAFE_02_no_simultaneous_fire | PYR-SAFE-02 | Channels never fire at same time |
| test_SYS_DEPLOY_03_no_fire_during_ascent | SYS-DEPLOY-03 | No fire before apogee |
| test_PYR_FAULT_02_overcurrent_detection | PYR-FAULT-02 | FLAG pin fault logged |

### Other Tests
| Test | Requirement | Verifies |
|------|-------------|----------|
| test_TST_06_chute_effect | TST-06 | Chutes slow descent |
| test_TST_05_karman_apogee | TST-05 | 100km flight reaches target |
