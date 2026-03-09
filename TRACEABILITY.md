# Requirements Traceability Matrix

## Test → Requirement Mapping

### Unit Tests (test_flight_states.c)

| Test | Requirement | Verified Behavior |
|------|-------------|-------------------|
| test_SNS_ALT_01_pressure_to_altitude | SNS-ALT-01 | Altitude computed from pressure difference |
| test_SNS_ALT_02_altitude_clamped_high | SNS-ALT-02 | Altitude clamped at 800000cm |
| test_SNS_ALT_03_altitude_clamped_low | SNS-ALT-03 | Negative altitude clamped to 0 |
| test_SNS_PRES_03_filter_init | SNS-PRES-03 | First reading passes through unfiltered |
| test_SNS_PRES_02_filter_smoothing | SNS-PRES-02 | Step change is smoothed |
| test_SNS_PRES_04_filter_min_step | SNS-PRES-04 | Filter advances ±1 when truncation would give 0 |
| test_DAT_01_buf_add | DAT-01 | Sample stored in ring buffer |
| test_DAT_01_buf_wraps | DAT-01 | Ring buffer wraps at 4096 |
| test_FLT_BOOT_01_reaches_pad_idle | FLT-BOOT-01 | Boot sequence completes to PAD_IDLE |
| test_FLT_BOOT_08_calibrates_ground | FLT-BOOT-08 | Ground pressure averaged from 10 readings |
| test_SNS_PRES_01_boot_no_sensor | SNS-PRES-01 | Graceful handling when no sensor detected |
| test_FLT_BOOT_04_i2c_settle | FLT-BOOT-04 | 500ms wait before sensor communication |
| test_FLT_LAUNCH_02_stays_on_ground | FLT-LAUNCH-02 | No transition at ground level |
| test_FLT_LAUNCH_01_detects_ascent | FLT-LAUNCH-01 | Transitions to ASCENT above 10m |
| test_PYR_CONT_01_continuity_check | PYR-CONT-01 | Continuity checked, ADC stored |
| test_FLT_ASC_01_tracks_max_altitude | FLT-ASC-01 | max_altitude updated on new highs |
| test_FLT_ASC_04_arms_pyros | FLT-ASC-04 | Pyros arm when speed < 10 m/s |
| test_FLT_APO_01_detects_apogee | FLT-APO-01 | Transitions to DESCENT when speed ≤ 0 |
| test_FLT_LAND_01_detects_landing | FLT-LAND-01, FLT-LAND-02, FLT-LAND-03 | Landing after 1s stable + low speed + low alt |
| test_FLT_LAND_06_stays_landed | FLT-LAND-06 | No state change after landing |
| test_TEL_01_format | TEL-01 | Starts with $PYRO, ends with *XX\r\n |
| test_TEL_09_seq_increments | TEL-09 | Sequence number increments |
| test_TEL_02_checksum | TEL-02 | XOR checksum matches payload |
| test_TEL_07_state_mapping | TEL-07 | PAD_IDLE=0, ASCENT=1, DESCENT=2, LANDED=3 |
| test_TEL_08_flags | TEL-08 | Continuity + armed flags encode correctly |
| test_TEL_06_altitude_and_speed | TEL-06 | All numeric fields present and correct |
| test_TEL_10_thrust_flag | TEL-10 | Thrust flag set only during ASCENT |
| test_TEL_06_pyro_adc | TEL-06 | ADC values from context |
| test_TEL_08_all_flags | TEL-08 | All 6 flags set = 0x3F |
| test_TEL_07_boot_maps_to_zero | TEL-07 | Boot states output state=0 |
| test_CFG_02_parse_full | CFG-02 | All config fields parsed correctly |
| test_CFG_04_parse_all_modes | CFG-04 | All 4 pyro modes recognized |
| test_CFG_03_parse_all_units | CFG-03 | cm/m/ft units recognized |
| test_CFG_09_unix_newlines | CFG-09 | LF-only line endings handled |
| test_CFG_02_no_section_header | CFG-02 | Parses without [section] header |
| test_CFG_08_unknown_keys | CFG-08 | Unknown keys ignored |
| test_CFG_04_unknown_mode | CFG-04 | Unknown mode stays at 0 |
| test_CFG_02_empty_string | CFG-02 | Empty input handled safely |
| test_CFG_09_no_trailing_newline | CFG-09 | Last line without newline parsed |
| test_CFG_07_id_truncated | CFG-07 | ID truncated to 8 characters |
| test_CFG_06_preserves_unset | CFG-06 | Partial config preserves existing fields |
| test_CFG_08_comment_lines | CFG-08 | Comment/section lines skipped |

### Integration Tests (test_integration.c)

| Test | Requirement | Verified Behavior |
|------|-------------|-------------------|
| test_sim_data_loads | TST-02 | Trajectory data loads correctly |
| test_interpolation | TST-02 | Altitude interpolation between data points |
| test_SNS_ALT_01_roundtrip | SNS-ALT-01 | Altitude→pressure→altitude round-trips |
| test_FLT_BOOT_01_all_states | FLT-BOOT-01, FLT-LAUNCH-01, FLT-APO-01, FLT-LAND-04 | Full state sequence PAD→ASC→DESC→LAND |
| test_FLT_APO_01_detected | FLT-APO-01, FLT-ASC-01 | Apogee detected, max altitude recorded |
| test_PYR_MODE_01_fires | PYR-MODE-01 | At least one pyro fires |
| test_BUZ_07_05_lifecycle | BUZ-07, BUZ-03 | Buzzer stops on launch, plays altitude on landing |
| test_DAT_04_events | DAT-04 | Launch, armed, pyro, landing events logged |
| test_TEL_01_output | TEL-01 | Telemetry sentences emitted with valid checksum |
| test_FLT_LAUNCH_01_timing | FLT-LAUNCH-01, FLT-APO-01 | State transitions at expected times |
| test_flight_duration | FLT-LAND-04 | Flight duration reasonable |

### Closed-Loop Tests (test_closedloop.c)

| Test | Requirement | Verified Behavior |
|------|-------------|-------------------|
| test_PYR_MODE_04_delay_delay | PYR-MODE-01, TST-04, TST-05 | DELAY+DELAY at 4 altitudes |
| test_PYR_MODE_02_delay_agl | PYR-MODE-01, PYR-MODE-02, TST-04, TST-05 | DELAY+AGL at 4 altitudes |
| test_PYR_MODE_03_delay_fallen | PYR-MODE-01, PYR-MODE-03, TST-04, TST-05 | DELAY+FALLEN at 4 altitudes |
| test_PYR_MODE_04_delay_speed | PYR-MODE-01, PYR-MODE-04, TST-04, TST-05 | DELAY+SPEED at 4 altitudes |
| test_PYR_MODE_02_agl_agl | PYR-MODE-02, TST-04, TST-05 | AGL+AGL at 4 altitudes |
| test_PYR_MODE_03_fallen_agl | PYR-MODE-03, PYR-MODE-02, TST-04, TST-05 | FALLEN+AGL at 4 altitudes |
| test_PYR_MODE_04_speed_agl | PYR-MODE-04, PYR-MODE-02, TST-04, TST-05 | SPEED+AGL at 4 altitudes |
| test_TST_06_chute_effect | TST-06 | Chute deployment slows descent |
| test_TST_05_karman_apogee | TST-05 | 100km flight reaches target apogee |

### Web UI Tests (test_ui.spec.js)

| Test | Requirement | Verified Behavior |
|------|-------------|-------------------|
| status tab shows PAD_IDLE | TEL-07 | State displayed correctly |
| status shows altitude in meters | CFG-03 | Units applied to display |
| config tab loads default values | CFG-05 | Default config loaded |
| pyro channels show OK | PYR-CONT-02 | Continuity status displayed |
| no pending config warning | CFG-01 | Clean state on fresh device |
| active config shows default settings | CFG-02 | Config fields displayed |
| flight data tab shows no data | DAT-06 | No data before flight |
| status shows values in feet | CFG-03 | Feet unit conversion |
| config tab shows non-default values | CFG-02 | Custom config loaded |
| edit config → save shows confirmation | CFG-01 | Config save works |
| default button loads defaults | CFG-05 | Default values available |
| current button restores device config | CFG-01 | Revert to device config |
| range warning for sensor limit | PYR-ALT-01 | Warning for out-of-range values |
| status shows LANDED | FLT-LAND-06 | Post-flight state |
| max altitude shows ~10000ft | FLT-ASC-01, CFG-03 | Max altitude in config units |
| pyro channels show FIRED | PYR-SAFE-04 | Fire status displayed |
| flight data shows duration and apogee | DAT-06 | Flight summary |
| flight CSV download works | DAT-06 | CSV export |
| update tab shows firmware version | BLD-04 | Version displayed |
| all four tabs navigable | TST-07 | UI navigation |

## Gap Analysis

### Requirements WITHOUT test coverage:

| Requirement | Gap | Recommended Test |
|-------------|-----|-----------------|
| FLT-BOOT-02 | Config read during boot not directly tested | Unit test: boot with pre-written config file |
| FLT-BOOT-03 | Default config creation not directly tested | Unit test: boot with no config file, verify file created |
| FLT-BOOT-05 | Sensor init during boot not isolated | Covered indirectly by boot sequence test |
| FLT-BOOT-06 | Pyro init during boot not isolated | Covered indirectly by boot sequence test |
| FLT-BOOT-07 | Initial continuity check not isolated | Covered indirectly by boot sequence test |
| FLT-BOOT-09 | 2-second stabilization wait not tested | Unit test: verify BOOT_STABILIZE waits 2s |
| FLT-LAUNCH-03 | Launch time backdating not tested | Unit test: verify launch_time is backdated |
| FLT-LAUNCH-04 | LAUNCH event logging not directly tested | Covered by integration test_DAT_04_events |
| FLT-ASC-02 | Vertical speed computation not isolated | Covered indirectly by apogee detection |
| FLT-ASC-03 | Thrust detection not isolated | Covered by TEL-10 thrust flag test |
| FLT-ASC-05 | ARMED event not directly tested | Covered by integration test_DAT_04_events |
| FLT-ASC-06 | Pyros not armed at high speed not tested | Unit test: verify no arming above 10 m/s |
| FLT-APO-02 | ASCENT→DESCENT transition tested but not named | Rename existing test |
| FLT-APO-03 | APOGEE event not directly tested | Covered by integration test |
| FLT-APO-04 | No apogee before arming not tested | Unit test: verify no apogee when unarmed |
| FLT-LAND-05 | LANDING event not directly tested | Covered by integration test |
| FLT-RATE-01..04 | Sample rates not directly tested | Unit tests: verify timing guards |
| PYR-SAFE-01 | No fire without continuity not tested | Unit test: verify no fire when continuity bad |
| PYR-SAFE-02 | No simultaneous fire not tested | Unit test: verify second channel blocked during fire |
| PYR-SAFE-03 | Single fire per channel not tested | Covered indirectly by closed-loop |
| PYR-REFIRE-01 | Re-fire logic not tested | Unit test: verify re-fire at >30m/s after 1s |
| PYR-ALT-02 | Config range beep code not tested | Unit test: verify BEEP_CFG_RANGE |
| SNS-ALT-02 | High altitude clamp not directly tested | Add to existing altitude test |
| SNS-ALT-03 | Negative altitude clamp not tested | Unit test: negative pressure difference |
| DAT-02 | Sample fields not individually verified | Covered by buf_add test |
| DAT-03 | Events on existing samples not tested | Unit test: verify event tags last sample |
| DAT-05 | Apogee protection not tested | Unit test: verify protected samples not overwritten |
| DAT-06 | CSV export not tested | Unit test: verify CSV content after flight |
| DAT-07 | CSV metadata header not tested | Unit test: verify header fields |
| HAL-01 | No conditional compilation verified | Build system test: grep for #ifdef in flight code |
| HAL-04 | Same source for all targets verified | Build system test: diff source lists |
| BLD-01..05 | Build outputs not tested | CI verifies build success |
| TEL-03..05 | Telemetry rates not directly tested | Integration test timing |
| BUZ-01..02 | Startup chirps + code not tested | Unit test: verify buzzer sequence |
| BUZ-04..06 | Altitude digit encoding not tested | Unit test: verify digit extraction |
| PYR-FAULT-01 | Overcurrent disable not implemented | Hardware feature: AP2192 handles this |
| PYR-FAULT-02 | Overcurrent detection not implemented | Read FLAG pins during/after fire |
| PYR-FAULT-03 | Overcurrent indication not implemented | Beep code or web display |
| PYR-VERIFY-01 | Post-fire verification not implemented | Read continuity after fire |
| WEB-NET-01 | USB network not tested in CI | Hardware test only |
| WEB-NET-02 | DHCP not tested in CI | Hardware test: support/test_network.py |
| WEB-NET-03 | mDNS not tested in CI | Hardware test: support/test_network.py |
| WEB-NET-04 | DNS-SD not tested in CI | Hardware test: support/test_network.py |
| WEB-API-01 | Status API tested via web tests | Mock only, not real device |
| WEB-API-02 | Config GET tested via web tests | Mock only |
| WEB-API-03 | Config POST tested via web tests | Mock only |
| WEB-API-04 | OTA not tested in CI | Hardware test |
| WEB-API-05 | Reboot tested via web tests | Mock only |
| WEB-API-06 | Flight CSV endpoint is a stub | Implement: serve flight.csv from littlefs |
| WEB-API-07 | CORS not tested | Add to web tests |
| WEB-UI-01..05 | Partially covered by Playwright | Mock server only |
| OTA-01..04 | Not tested in CI | Hardware test: OTA + rollback |

### Requirements with PARTIAL coverage:

| Requirement | Current Coverage | Gap |
|-------------|-----------------|-----|
| FLT-LAND-01 | Unit test checks stable altitude | Does not verify 1-second duration explicitly |
| PYR-MODE-05 | Closed-loop tests verify indirectly | No isolated unit test |
| CFG-01 | Web test verifies save | No unit test for file persistence |
| TEL-06 | Altitude and speed verified | Battery and temperature fields not verified |
