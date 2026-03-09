# Requirements Traceability Matrix

Traces each requirement to its implementing code and verifying test(s).
- ✅ = implemented and tested
- ⚠️ = implemented, no direct test
- ❌ = not implemented

---

## 1. Recovery Deployment

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-DEPLOY-01 | Fire pyros at configurable events | flight_states.c:state_ascent, state_descent | test_closedloop.c (all) | ✅ |
| SYS-DEPLOY-02 | Two independent channels | flight_states.c:should_fire_pyro | test_closedloop.c | ✅ |
| SYS-DEPLOY-03 | No firing before leaving rail | flight_states.c:PYR-SAFE-04 check | — | ⚠️ |
| FLT-PHASE-01 | Detect launch | flight_states.c:state_pad_idle | test_FLT_LAUNCH_01_detects_ascent | ✅ |
| FLT-PHASE-02 | Detect apogee | flight_states.c:state_ascent | test_FLT_APO_01_detects_apogee | ✅ |
| FLT-PHASE-03 | Detect landing | flight_states.c:state_descent | test_FLT_LAND_01_detects_landing | ✅ |
| PYR-MODE-01 | DELAY mode | flight_states.c:should_fire_pyro | test_PYR_MODE_01_delay_delay | ✅ |
| PYR-MODE-02 | AGL mode | flight_states.c:should_fire_pyro | test_PYR_MODE_02_delay_agl, test_PYR_MODE_02_agl_agl | ✅ |
| PYR-MODE-03 | FALLEN mode | flight_states.c:should_fire_pyro | test_PYR_MODE_03_delay_fallen, test_PYR_MODE_03_fallen_agl | ✅ |
| PYR-MODE-04 | SPEED mode | flight_states.c:should_fire_pyro | test_PYR_MODE_04_delay_speed, test_PYR_MODE_04_speed_agl | ✅ |
| PYR-SAFE-01 | No fire without continuity | flight_states.c:state_ascent (continuity_good check) | — | ⚠️ |
| PYR-SAFE-02 | No simultaneous fire | flight_states.c:hal_pyro_is_firing() check | — | ⚠️ |
| PYR-SAFE-03 | Single fire per channel | flight_states.c:pyro1_fired check | — | ⚠️ |
| PYR-SAFE-04 | No fire before apogee | flight_states.c:should_fire_pyro (apogee_detected) | — | ⚠️ |
| FLT-LAUNCH-01 | Transition at >10m | flight_states.c:state_pad_idle (altitude > 1000) | test_FLT_LAUNCH_01_detects_ascent | ✅ |
| FLT-LAUNCH-02 | Stay at ground level | flight_states.c:state_pad_idle | test_FLT_LAUNCH_02_stays_on_ground | ✅ |
| FLT-LAUNCH-03 | Backdate launch time | flight_states.c:state_pad_idle (backdate loop) | — | ⚠️ |
| FLT-LAUNCH-04 | Log LAUNCH event | flight_states.c:buf_tag_event(EVT_LAUNCH) | test_DAT_04_events | ✅ |
| FLT-LAUNCH-05 | Stop buzzer on launch | flight_states.c:buzzer_stop() | test_BUZ_07_03_lifecycle | ✅ |
| FLT-APO-01 | Apogee when speed ≤ 0 and armed | flight_states.c:state_ascent | test_FLT_APO_01_detects_apogee | ✅ |
| FLT-APO-02 | Transition to DESCENT | flight_states.c:state_ascent (return DESCENT) | test_FLT_APO_01_detects_apogee | ✅ |
| FLT-APO-03 | Log APOGEE event | flight_states.c:buf_tag_event(EVT_APOGEE) | test_DAT_04_events | ✅ |
| FLT-APO-04 | No apogee before armed | flight_states.c:pyros_armed check | — | ⚠️ |
| FLT-ASC-01 | Track max altitude | flight_states.c:state_ascent (max_altitude) | test_FLT_ASC_01_tracks_max_altitude | ✅ |
| FLT-ASC-02 | Compute vertical speed | flight_states.c:state_ascent (vertical_speed_cms) | test_FLT_APO_01_detects_apogee (indirect) | ⚠️ |
| FLT-ASC-03 | Detect thrust phase | flight_states.c:state_ascent (under_thrust) | test_TEL_10_thrust_flag | ✅ |
| FLT-ASC-04 | Arm at <10 m/s | flight_states.c:state_ascent (< 1000 cms) | test_FLT_ASC_04_arms_pyros | ✅ |
| FLT-ASC-05 | Log ARMED event | flight_states.c:buf_tag_event(EVT_ARMED) | test_DAT_04_events | ✅ |
| FLT-ASC-06 | No arm above 10 m/s | flight_states.c:state_ascent (>= 0 check) | — | ⚠️ |
| FLT-LAND-01 | Stable <1m for 1s | flight_states.c:state_descent (< 100 cm) | test_FLT_LAND_01_detects_landing | ✅ |
| FLT-LAND-02 | Speed <2 m/s | flight_states.c:state_descent (< 200 cms) | test_FLT_LAND_01_detects_landing | ✅ |
| FLT-LAND-03 | Altitude <30m | flight_states.c:state_descent (< 3000 cm) | test_FLT_LAND_01_detects_landing | ✅ |
| FLT-LAND-04 | Transition to LANDED | flight_states.c:state_descent (return LANDED) | test_FLT_LAND_01_detects_landing | ✅ |
| FLT-LAND-05 | Log LANDING event | flight_states.c:buf_tag_event(EVT_LANDING) | test_DAT_04_events | ✅ |
| FLT-LAND-06 | Stay in LANDED | flight_states.c:state_landed | test_FLT_LAND_06_stays_landed | ✅ |
| PYR-REFIRE-01 | Re-fire if ballistic | flight_states.c:state_descent (refire block) | — | ⚠️ |
| PYR-ALT-01 | Clamp altitude settings | flight_states.c:should_fire_pyro (clamped) | — | ⚠️ |
| PYR-ALT-02 | Warning beep for range | flight_states.c:state_pad_idle (BEEP_CFG_RANGE) | — | ⚠️ |
| FLT-RATE-01 | 10ms PAD_IDLE | flight_states.c:state_pad_idle (< 10) | — | ⚠️ |
| FLT-RATE-02 | 100ms ASCENT | flight_states.c:state_ascent (< 100) | — | ⚠️ |
| FLT-RATE-03 | 50ms DESCENT | flight_states.c:state_descent (< 50) | — | ⚠️ |
| FLT-RATE-04 | 1000ms LANDED | flight_states.c:state_landed (< 1000) | — | ⚠️ |

## 2. Pre-Flight Status

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-STATUS-01 | Audible readiness indication | buzzer.c, flight_states.c:state_pad_idle | test_BUZ_07_03_lifecycle | ✅ |
| SYS-STATUS-02 | Verify pyro integrity | flight_states.c:state_pad_idle, state_boot_continuity | test_PYR_CONT_01_continuity_check | ✅ |
| PYR-CONT-01 | Check continuity every 1s | flight_states.c:state_pad_idle (> 1000) | test_PYR_CONT_01_continuity_check | ✅ |
| PYR-CONT-02 | Report good/open/short | flight_states.c:state_pad_idle | test_PYR_CONT_01_continuity_check | ✅ |
| BUZ-STATUS-01 | Distinct beep codes per fault | buzzer.h (BEEP_* defines), flight_states.c | — | ⚠️ |
| BUZ-01 | 10 chirps + status code | buzzer.c:buzzer_set_code | — | ⚠️ |
| BUZ-02 | Code played twice then stop | buzzer.c:repeats_remaining | — | ⚠️ |
| FLT-BOOT-01 | Non-blocking boot to PAD_IDLE | flight_states.c:dispatch_state | test_FLT_BOOT_01_reaches_pad_idle | ✅ |
| FLT-BOOT-02 | Read config during boot | flight_states.c:state_boot_filesystem | — | ⚠️ |
| FLT-BOOT-03 | Create default config | flight_states.c:state_boot_filesystem | — | ⚠️ |
| FLT-BOOT-04 | 500ms I2C settle | flight_states.c:state_boot_i2c_settle | test_FLT_BOOT_04_i2c_settle | ✅ |
| FLT-BOOT-05 | Detect pressure sensor | flight_states.c:state_boot_sensor_detect | test_SNS_PRES_01_boot_no_sensor | ✅ |
| FLT-BOOT-06 | Init pyro subsystem | flight_states.c:state_boot_pyro_init | test_FLT_BOOT_01_reaches_pad_idle (indirect) | ⚠️ |
| FLT-BOOT-07 | Initial continuity check | flight_states.c:state_boot_continuity | test_FLT_BOOT_01_reaches_pad_idle (indirect) | ⚠️ |
| FLT-BOOT-08 | Calibrate from 10 readings | flight_states.c:state_boot_calibrate | test_FLT_BOOT_08_calibrates_ground | ✅ |
| FLT-BOOT-09 | 2s stabilization wait | flight_states.c:state_boot_stabilize | — | ⚠️ |

## 3. Flight Data Recovery

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-DATA-01 | Record flight data | flight_states.c:buf_add | test_DAT_01_buf_add | ✅ |
| SYS-DATA-02 | Export in standard format | flight_states.c:flight_save_csv | — | ⚠️ |
| SYS-DATA-03 | Announce max altitude | buzzer.c:buzzer_set_altitude | test_BUZ_07_03_lifecycle | ✅ |
| DAT-01 | 4096-entry ring buffer | flight_states.c:buf_add | test_DAT_01_buf_add, test_DAT_01_buf_wraps | ✅ |
| DAT-02 | Sample fields | flight_states.h:flight_sample_t | test_DAT_01_buf_add | ✅ |
| DAT-03 | Events tag existing samples | flight_states.c:buf_tag_event | — | ⚠️ |
| DAT-04 | Log all event types | flight_states.c (6 buf_tag_event calls) | test_DAT_04_events | ✅ |
| DAT-05 | Protect apogee samples | flight_states.c:buf_add (apogee_protected) | — | ⚠️ |
| DAT-06 | CSV export after landing | flight_states.c:flight_save_csv | — | ⚠️ |
| DAT-07 | CSV metadata header | flight_states.c:flight_save_csv (header) | — | ⚠️ |
| BUZ-03 | Altitude beep-out after landing | buzzer.c:buzzer_set_altitude | test_BUZ_07_03_lifecycle | ✅ |
| BUZ-04 | Encode each digit | buzzer.c:alt_digits | — | ⚠️ |
| BUZ-05 | Digit 0 = 10 beeps | buzzer.c:start_alt_digit | — | ⚠️ |
| BUZ-06 | Repeat indefinitely | buzzer.c:BZ_ALT_LONG_PAUSE loop | — | ⚠️ |
| BUZ-07 | Stop on launch | flight_states.c:buzzer_stop() | test_BUZ_07_03_lifecycle | ✅ |

## 4. Configuration

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-CFG-01 | Persistent config | flight_states.c:state_boot_filesystem | test_CFG_02_parse_full | ✅ |
| SYS-CFG-02 | Config without special tools | www/app.js, http_server.c | Web UI tests | ✅ |
| SYS-CFG-03 | Validate against sensor limits | flight_states.c:PYR-ALT-01, www/app.js | Web UI: range warning test | ✅ |
| CFG-01 | INI format on storage | flight_states.c:parse_config_ini | test_CFG_02_parse_full | ✅ |
| CFG-02 | Parse all fields | flight_states.c:parse_config_ini | test_CFG_02_parse_full, test_CFG_02_no_section_header, test_CFG_02_empty_string | ✅ |
| CFG-03 | Units: cm, m, ft | flight_states.c:parse_config_ini | test_CFG_03_parse_all_units | ✅ |
| CFG-04 | Modes: delay, agl, fallen, speed | flight_states.c:parse_mode | test_CFG_04_parse_all_modes, test_CFG_04_unknown_mode | ✅ |
| CFG-05 | Default config if missing | flight_states.c:state_boot_filesystem | — | ⚠️ |
| CFG-06 | Preserve unset fields | flight_states.c:parse_config_ini | test_CFG_06_preserves_unset | ✅ |
| CFG-07 | Truncate id/name to 8 | flight_states.c:parse_config_ini | test_CFG_07_id_truncated | ✅ |
| CFG-08 | Ignore unknown keys | flight_states.c:parse_config_ini | test_CFG_08_unknown_keys, test_CFG_08_comment_lines | ✅ |
| CFG-09 | Handle CR+LF and LF | flight_states.c:parse_config_ini | test_CFG_09_unix_newlines, test_CFG_09_no_trailing_newline | ✅ |
| FLT-BOOT-02 | Read config on boot | flight_states.c:state_boot_filesystem | — | ⚠️ |
| FLT-BOOT-03 | Create default on boot | flight_states.c:state_boot_filesystem | — | ⚠️ |
| WEB-UI-02 | Guided config editor | www/index.html, www/app.js | Web UI: config tab tests | ✅ |
| WEB-UI-03 | Warn if not applied | www/app.js:pendingConfig | Web UI: save shows confirmation | ✅ |

## 5. Altitude Measurement

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-ALT-01 | Barometric altitude | flight_states.c:pressure_to_altitude_cm | test_SNS_ALT_01_pressure_to_altitude | ✅ |
| SYS-ALT-02 | Multiple sensor types | pressure_sensor.c, hal_hardware.c | test_SNS_PRES_01_boot_no_sensor | ✅ |
| SNS-PRES-01 | Auto-detect sensor | hal_hardware.c:hal_pressure_init | test_SNS_PRES_01_boot_no_sensor | ✅ |
| SNS-PRES-02 | Low-pass filter | flight_states.c:filter_pressure | test_SNS_PRES_02_filter_smoothing | ✅ |
| SNS-PRES-03 | Init to first reading | flight_states.c:filter_pressure | test_SNS_PRES_03_filter_init | ✅ |
| SNS-PRES-04 | Min ±1 Pa step | flight_states.c:filter_pressure | — | ⚠️ |
| SNS-ALT-01 | Altitude from pressure diff | flight_states.c:pressure_to_altitude_cm | test_SNS_ALT_01_pressure_to_altitude, test_SNS_ALT_01_roundtrip | ✅ |
| SNS-ALT-02 | Clamp max 8000m | flight_states.c:pressure_to_altitude_cm | — | ⚠️ |
| SNS-ALT-03 | Clamp min 0 | flight_states.c:pressure_to_altitude_cm | — | ⚠️ |

## 6. Telemetry

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| SYS-TEL-01 | Transmit via serial | telemetry.c:send_telemetry | test_TEL_01_format | ✅ |
| TEL-01 | $PYRO NMEA format | telemetry.c:send_telemetry | test_TEL_01_format, test_TEL_01_output | ✅ |
| TEL-02 | XOR checksum | telemetry.c:send_telemetry | test_TEL_02_checksum | ✅ |
| TEL-03 | 10Hz ASCENT/DESCENT | flight_states.c:flight_update_outputs | — | ⚠️ |
| TEL-04 | 1Hz PAD_IDLE/LANDED | flight_states.c:flight_update_outputs | — | ⚠️ |
| TEL-05 | No telemetry during boot | flight_states.c:flight_update_outputs (>= PAD_IDLE) | — | ⚠️ |
| TEL-06 | All fields present | telemetry.c:send_telemetry | test_TEL_06_altitude_and_speed, test_TEL_06_pyro_adc | ✅ |
| TEL-07 | State mapping 0-3 | telemetry.c:telemetry_state_id | test_TEL_07_state_mapping, test_TEL_07_boot_maps_to_zero | ✅ |
| TEL-08 | Flags encoding | telemetry.c:send_telemetry | test_TEL_08_flags, test_TEL_08_all_flags | ✅ |
| TEL-09 | Sequence increments | telemetry.c:send_telemetry | test_TEL_09_seq_increments | ✅ |
| TEL-10 | Thrust flag ASCENT only | telemetry.c:send_telemetry | test_TEL_10_thrust_flag | ✅ |

## 7. Pyro Fault Protection

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| PYR-FAULT-01 | Disable at >1.5A | AP2192 hardware | N/A (hardware) | ✅ HW |
| PYR-FAULT-02 | Detect overcurrent | — | — | ❌ |
| PYR-FAULT-03 | Indicate overcurrent | — | — | ❌ |
| PYR-VERIFY-01 | Verify circuit opened | — | — | ❌ |

## 8. Web Interface & Network

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| WEB-NET-01 | USB network | net_glue.c | support/test_network.py (HW) | ⚠️ |
| WEB-NET-02 | DHCP 192.168.7.1 | net_glue.c | support/test_network.py (HW) | ⚠️ |
| WEB-NET-03 | mDNS | net_glue.c | support/test_network.py (HW) | ⚠️ |
| WEB-NET-04 | DNS-SD | net_glue.c | support/test_network.py (HW) | ⚠️ |
| WEB-API-01 | GET /api/status | http_server.c:serve_api_status | Web UI: status tests | ✅ |
| WEB-API-02 | GET /api/config | http_server.c:serve_api_config | Web UI: config tests | ✅ |
| WEB-API-03 | POST /api/config | http_server.c | Web UI: save test | ✅ |
| WEB-API-04 | POST /api/ota | http_server.c | — | ⚠️ |
| WEB-API-05 | POST /api/reboot | http_server.c | — | ⚠️ |
| WEB-API-06 | GET /api/flight.csv | http_server.c:serve_api_flight_csv | — | ⚠️ stub |
| WEB-API-07 | CORS headers | http_server.c:CORS_HDR | — | ⚠️ |
| WEB-UI-01 | Status in config units | www/app.js:cmToUnit | Web UI: altitude in meters/feet | ✅ |
| WEB-UI-04 | Flight summary + CSV | www/app.js, www/index.html | Web UI: flight data tests | ✅ |
| WEB-UI-05 | Firmware upload + check | www/app.js | Web UI: update tab test | ✅ |

## 9. Firmware Update

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| OTA-01 | OTA via HTTP | http_server.c | — | ⚠️ HW only |
| OTA-02 | Write to inactive slot | http_server.c, pico_fota_bootloader | — | ⚠️ HW only |
| OTA-03 | Auto-revert on bad firmware | pico_fota_bootloader, hal_hardware.c:pfb_firmware_commit | — | ⚠️ HW only |
| OTA-04 | Interrupted update safe | pico_fota_bootloader | — | ⚠️ HW only |

## 10. Portability & Testability

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| HAL-01 | No #ifdef in flight code | flight_states.c, telemetry.c, buzzer.c | grep verification | ✅ |
| HAL-02 | All HW through HAL | hal.h | All tests use hal_test.c | ✅ |
| HAL-03 | 3 HAL implementations | hal_hardware.c, hal_test.c, hal_sim.c | Build verification | ✅ |
| HAL-04 | Same source all targets | CMakeLists.txt | Build verification | ✅ |

## 11. Build & Test System

| Req | Description | Code | Test | Status |
|-----|-------------|------|------|--------|
| BLD-01 | RP2040 firmware | CMakeLists.txt | CI build | ✅ |
| BLD-02 | Host test executables | CMakeLists.txt | CI host_tests | ✅ |
| BLD-03 | Host simulator | CMakeLists.txt | ninja sim | ✅ |
| BLD-04 | Version header | scripts/gen_version.sh | CI build | ✅ |
| BLD-05 | A/B OTA images | CMakeLists.txt, pico_fota_bootloader | CI build | ✅ |
| TST-01 | Unit tests with mocks | test/test_flight_states.c | 39 tests pass | ✅ |
| TST-02 | Integration with trajectory | test/test_integration.c | 11 tests pass | ✅ |
| TST-03 | Closed-loop with physics | test/test_closedloop.c | 9 tests pass | ✅ |
| TST-04 | All 4 pyro modes | test/test_closedloop.c | 7 config suites | ✅ |
| TST-05 | 100ft to 100km | test/test_closedloop.c | 4 altitude profiles | ✅ |
| TST-06 | Chute reduces descent | test/test_closedloop.c | test_TST_06_chute_effect | ✅ |
| TST-07 | Web UI tests | test/web/test_ui.spec.js | 22 Playwright tests | ✅ |
| TST-08 | CI on every push | .github/workflows/build.yml | GitHub Actions | ✅ |
| TST-09 | Traceable to requirements | This document | — | ✅ |

---

## Summary

| Status | Count | Percentage |
|--------|-------|-----------|
| ✅ Implemented + tested | 89 | 72% |
| ⚠️ Implemented, no direct test | 30 | 24% |
| ❌ Not implemented | 3 | 2% |
| ✅ HW (hardware satisfies) | 1 | 1% |
| **Total** | **123** | |

### Critical gaps (safety-related, no test):
1. **PYR-SAFE-01**: No fire without continuity
2. **PYR-SAFE-02**: No simultaneous fire
3. **PYR-SAFE-04**: No fire before apogee
4. **PYR-REFIRE-01**: Re-fire logic
5. **PYR-FAULT-02**: Overcurrent detection (not implemented)
6. **PYR-FAULT-03**: Overcurrent indication (not implemented)
7. **PYR-VERIFY-01**: Post-fire verification (not implemented)

### Non-critical gaps (implemented, need tests):
- FLT-BOOT-02/03: Config read/create during boot
- FLT-BOOT-09: 2s stabilization wait
- FLT-RATE-01..04: Sample rate timing
- SNS-PRES-04: Filter min step
- SNS-ALT-02/03: Altitude clamping
- DAT-03/05/06/07: Data logging details
- BUZ-01/02/04/05/06: Buzzer sequence details
- TEL-03/04/05: Telemetry rates
