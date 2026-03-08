# Unit & Integration Tests

Host-compiled tests for the Pyro MK1B flight computer. All tests run on the build machine (not ARM target) using mock hardware.

## Running

```bash
cd build

# Unit tests (27 tests)
ninja host_tests

# Integration tests (11 tests)
ninja integration_tests
```

Both run automatically in GitHub Actions CI on every push.

## Architecture

Tests compile with the host C compiler using `-DUNIT_TEST`. Source files use `#ifdef UNIT_TEST` to include mocks instead of real Pico SDK headers.

```
test/
  mocks.h              Mock declarations (GPIO, I2C, UART, LFS, time)
  mocks.c              Mock implementations (pressure sensor, pyro, UART capture)
  test_flight_states.c  27 unit tests
  test_integration.c    11 integration tests
test_data/
  open_rocket_export.csv  OpenRocket simulation (228 points, 16s flight, 165ft peak)
```

## Unit Tests (test_flight_states.c)

### Helpers
| Test | Verifies |
|------|----------|
| pressure_to_altitude_cm | Barometric formula: 1000 Pa drop ≈ 8300 cm |
| filter_pressure_init | First reading passes through unfiltered |
| filter_pressure_smoothing | Step change is smoothed (output between old and new) |
| buf_add | Single sample stored correctly |
| buf_add_wraps | Ring buffer wraps at 4096, overwrites oldest |

### Boot Sequence
| Test | Verifies |
|------|----------|
| boot_sequence_reaches_pad_idle | Full boot: FILESYSTEM → ... → MDNS → PAD_IDLE |
| boot_calibrates_ground_pressure | 10 readings averaged for ground reference |
| boot_no_sensor | Graceful handling when no sensor detected |
| boot_i2c_settle_waits | 500ms delay before sensor detect |

### PAD_IDLE
| Test | Verifies |
|------|----------|
| pad_idle_stays_on_ground | No state change at ground level |
| pad_idle_detects_ascent | Transitions to ASCENT at >10m altitude |
| pad_idle_continuity_check | Reads pyro ADC, stores in context |

### ASCENT
| Test | Verifies |
|------|----------|
| ascent_tracks_max_altitude | max_altitude updated on new highs |
| ascent_arms_pyros_at_low_speed | Armed when vertical speed < 10 m/s |
| ascent_detects_apogee | Transitions to DESCENT when speed ≤ 0 |

### DESCENT
| Test | Verifies |
|------|----------|
| descent_detects_landing | Transitions to LANDED after 1s stable altitude |

### LANDED
| Test | Verifies |
|------|----------|
| landed_stays_landed | No state change after landing |

### Telemetry ($PYRO NMEA)
| Test | Verifies |
|------|----------|
| telemetry_format | Starts with $PYRO, ends with *XX\r\n |
| telemetry_seq_increments | Sequence number increments each call |
| telemetry_checksum | XOR checksum matches payload |
| telemetry_state_mapping | PAD_IDLE=0, ASCENT=1, DESCENT=2, LANDED=3 |
| telemetry_flags | Continuity + armed flags encode correctly (0x13) |
| telemetry_altitude_and_speed | All numeric fields parsed correctly |
| telemetry_thrust_flag | Set only during ASCENT with under_thrust |
| telemetry_pyro_adc | ADC values from context, not globals |
| telemetry_all_flags | All 6 flags set = 0x3F |
| telemetry_boot_state_maps_to_zero | Boot states output state=0 |

## Integration Tests (test_integration.c)

Simulates a complete flight using OpenRocket trajectory data at 1ms resolution.

### How It Works
1. **Load** 228 data points from `test_data/open_rocket_export.csv` (time, altitude_ft)
2. **Interpolate** altitude at any time via binary search + linear interpolation
3. **Convert** altitude to pressure: `P = 101325 - (alt_ft × 30.48 × 10 / 83)`
4. **Run** app_tick() at 1ms intervals for ~18 seconds (sim + 2s settling)
5. **Verify** state transitions, pyro fires, telemetry, data log, buzzer

### Configuration
- Pyro 1: delay = 0 (fire at apogee)
- Pyro 2: AGL = 50 ft
- Units: feet

### Tests
| Test | Verifies |
|------|----------|
| sim_data_loads | CSV parsed, >100 data points |
| interpolation | t=0 → 0ft, mid-flight > 100ft, post-landing → 0ft |
| pressure_conversion_roundtrip | 100ft → Pa → altitude_cm ≈ 3048cm |
| full_flight_reaches_all_states | PAD_IDLE → ASCENT → DESCENT → LANDED all visited |
| apogee_detected_and_altitude | Apogee flagged, max altitude ~5000cm (165ft) |
| pyro_fires | At least one pyro channel fired |
| buzzer_lifecycle | Stops on launch, plays altitude on landing |
| data_log_complete | >100 samples, launch/armed/pyro/landing events present |
| telemetry_output | ≥10 $PYRO sentences with valid checksum |
| state_timing | Ascent <3s, descent <6s, landed after 12s |
| flight_duration | Total flight ~15 seconds |

### Known Issue
None currently.
