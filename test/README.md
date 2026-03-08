# Unit & Integration Tests

Host-compiled tests for the Pyro MK1B flight computer. All tests run on the build machine (not ARM target) using mock hardware.

## Running

```bash
cd build

ninja host_tests          # 27 unit tests
ninja integration_tests   # 11 integration tests (OpenRocket data)
ninja closedloop_tests    # 9 closed-loop tests (28 simulated flights)
```

All three run automatically in GitHub Actions CI on every push.

## Architecture

Tests compile with the host C compiler using `-DUNIT_TEST`. Source files use `#ifdef UNIT_TEST` to include mocks instead of real Pico SDK headers.

```
test/
  mocks.h              Mock declarations (GPIO, I2C, UART, LFS, time)
  mocks.c              Mock implementations (pressure sensor, pyro, UART capture)
  test_flight_states.c  27 unit tests
  test_integration.c    11 integration tests
  test_closedloop.c     9 closed-loop simulation tests
test_data/
  open_rocket_export.csv  OpenRocket simulation (228 points, 16s flight, 165ft peak)
```

## Unit Tests (test_flight_states.c) — 27 tests

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
| descent_detects_landing | Transitions to LANDED after 1s stable + low speed + low altitude |

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

## Integration Tests (test_integration.c) — 11 tests

Simulates a complete flight using OpenRocket trajectory data at 1ms resolution.

### How It Works
1. **Load** 228 data points from `test_data/open_rocket_export.csv` (time, altitude_ft)
2. **Interpolate** altitude at any time via binary search + linear interpolation
3. **Convert** altitude to pressure: `P = 101325 - (alt_ft × 30.48 × 10 / 83)`
4. **Run** app_tick() at 1ms intervals for ~18 seconds (sim + 2s settling)
5. **Verify** state transitions, pyro fires, telemetry, data log, buzzer

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
| data_log_complete | >100 samples with launch/armed/pyro/landing events |
| telemetry_output | ≥10 $PYRO sentences with valid checksum |
| state_timing | Ascent <3s, descent <6s, landed after 12s |
| flight_duration | Total flight ~15 seconds |

## Closed-Loop Simulation Tests (test_closedloop.c) — 9 tests, 28 flights

Full physics simulation with pyro deployment feedback. When the firmware fires a pyro, the physics model deploys the corresponding chute, changing the descent rate.

### Physics Model
- **Thrust phase:** configurable acceleration and burn time (binary search for target apogee)
- **Coast:** ballistic trajectory under gravity
- **Descent drag:** `a_drag = drag_coeff × density_fraction × |velocity|`
- **Atmospheric density:** exponential decay with 8.5km scale height
- **Standard atmosphere:** troposphere + stratosphere pressure model
- **Chute deployment:** drogue (0.8 drag) and main (4.0 drag) change terminal velocity

### Configurations Tested
| Config | Pyro 1 (Drogue) | Pyro 2 (Main) |
|--------|-----------------|---------------|
| Dly+Dly | Delay 0s (at apogee) | Delay 3s |
| Dly+AGL | Delay 0s | AGL 200ft |
| Dly+Fal | Delay 0s | Fallen 100ft |
| Dly+Spd | Delay 0s | Speed 30ft/s |
| AGL+AGL | AGL 400ft | AGL 200ft |
| Fal+AGL | Fallen 50ft | AGL 200ft |
| Spd+AGL | Speed 20ft/s | AGL 200ft |

### Altitude Profiles
| Profile | Target | Typical Apogee | Flight Time |
|---------|--------|---------------|-------------|
| Low | 100 ft (31m) | 31m | ~10s |
| Medium | 500 ft (152m) | 152m | ~35-60s |
| High | 5000 ft (1524m) | 1525m | ~150-580s |
| Karman | 100 km | 100,045m | ~860-3630s |

### Sample Output
```
  Dly+Dly@100ft        apogee=    31m  launch=3.2s apogee=5.5s P1=5.5s@26m P2=8.5s@0m landed=10.3s
  Dly+AGL@5000ft       apogee=  1525m  launch=3.0s apogee=21.8s P1=21.8s@1519m P2=131.5s@47m landed=150.6s
  Fal+AGL@Karman       apogee=100045m  launch=3.2s apogee=64.2s P1=273.1s@39233m P2=845.9s@47m landed=864.9s
  chute: with          apogee=  1525m  launch=3.0s apogee=21.8s P1=21.8s@1519m P2=131.5s@47m landed=150.6s
  chute: without       apogee=  1525m  launch=3.0s apogee=21.8s landed=45.9s
```

### Tests
| Test | Flights | Verifies |
|------|---------|----------|
| test_delay_delay | 4 | DELAY+DELAY at all altitudes |
| test_delay_agl | 4 | DELAY+AGL at all altitudes |
| test_delay_fallen | 4 | DELAY+FALLEN at all altitudes |
| test_delay_speed | 4 | DELAY+SPEED at all altitudes |
| test_agl_agl | 4 | AGL+AGL at all altitudes |
| test_fallen_agl | 4 | FALLEN+AGL at all altitudes |
| test_speed_agl | 4 | SPEED+AGL at all altitudes |
| test_chute_slows_descent | 2 | With chutes takes longer than ballistic |
| test_karman_apogee | 1 | Apogee within 20km of 100km target |

Each flight verifies: full state sequence, drogue fires, data log, telemetry. Medium+ flights also verify main fires and drogue-before-main ordering (except Karman where both fire near apogee).

### Firmware Bugs Found
These tests discovered two real firmware bugs:
1. **pyro_fired flags never set** — pyros re-fired every tick, blocking the other channel
2. **Pressure filter integer stall** — truncation to 0 when diff < 22 Pa prevented convergence from near-vacuum
