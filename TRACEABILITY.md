# Requirements Traceability Matrix

Requirements are verified through integration and closed-loop tests that exercise
the complete system through flight scenarios. Unit tests verify implementation
correctness but are not directly linked to requirements.

## Verification Tests

### Integration Tests (test_integration.c)
Simulate a complete flight using OpenRocket trajectory data at 1ms resolution.

### Closed-Loop Tests (test_closedloop.c)
Simulate flights with physics feedback across 7 pyro configurations × 4 altitudes.

### Web UI Tests (test_ui.spec.js)
Verify web interface behavior against mock server in 3 device modes.

---

## 1. Recovery Deployment

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-1 | Deploy recovery devices safely | All closed-loop tests | ✅ |
| SYS-DEPLOY-01 | Fire pyros at configurable events | Closed-loop: all 7 config suites | ✅ |
| SYS-DEPLOY-02 | Two independent channels | Closed-loop: both channels fire in all suites | ✅ |
| SYS-DEPLOY-03 | No firing before leaving rail | Closed-loop: test_SYS_DEPLOY_03_no_fire_during_ascent | ✅ |
| FLT-PHASE-01 | Detect launch | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-PHASE-02 | Detect apogee | Integration: test_FLT_APO_01_detected | ✅ |
| FLT-PHASE-03 | Detect landing | Integration: test_FLT_LAND_04_duration | ✅ |
| PYR-MODE-01 | DELAY mode | Closed-loop: test_PYR_MODE_01_delay_delay | ✅ |
| PYR-MODE-02 | AGL mode | Closed-loop: test_PYR_MODE_02_delay_agl, test_PYR_MODE_02_agl_agl | ✅ |
| PYR-MODE-03 | FALLEN mode | Closed-loop: test_PYR_MODE_03_delay_fallen, test_PYR_MODE_03_fallen_agl | ✅ |
| PYR-MODE-04 | SPEED mode | Closed-loop: test_PYR_MODE_04_delay_speed, test_PYR_MODE_04_speed_agl | ✅ |
| PYR-SAFE-01 | No fire without continuity | Closed-loop: test_PYR_SAFE_01_no_fire_without_continuity | ✅ |
| PYR-DEPLOY-01 | Both channels may deploy on one event | Closed-loop: test_PYR_DEPLOY_01_low_flight_fires_both | ✅ |
| PYR-DEPLOY-02 | Not energised at the same instant, flight or ground test | `flight_pyro_energise()`, DD-021; Integration: test_REV06_ground_test_waits_for_the_other_channel | ✅ |
| PYR-FIRE-01 | Fired only when energised; a refusal is recorded once | Unit: test_REV03_refused_fire_is_not_recorded_as_fired; Hardware: `pyro1_refused` on /api/status | ✅ |
| PYR-MODE-05 | AGL / FALLEN on the fit's height, SPEED on its speed, DELAY from its apogee | Chain: test_T5_speed_and_delay_triggers (SPEED within 1 m/s, DELAY within 0.1 s); Mach: test_T5_ejection (AGL within 8 m); Closed-loop: test_REV05_agl_drogue_fires_at_its_altitude; every AGL channel in the mode suites within 8 m | ✅ |
| PYR-MODE-06 | Pressure triggers wait out an unclean fit, 2 s at most; believed after a charge if no lower than ballistic | Mach: test_T5_ejection (5 kPa bay charge), test_T5_canopy_swing; Chain: test_T5_descent_glitch; Closed-loop: test_PYR_MODE_02_agl_agl (a main 60 m below a drogue opening at 105 m/s) | ✅ |
| PYR-SAFE-03 | Single fire per channel | Closed-loop: verified by fire_count | ✅ |
| PYR-SAFE-04 | No fire before apogee | Closed-loop: drogue fires at/after apogee | ✅ |
| FLT-LAUNCH-01 | Transition at >100 ft | Unit: test_FLT_LAUNCH_08/09; Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-LAUNCH-02 | Stay at ground level | Integration: PAD_IDLE persists before launch | ✅ |
| GND-CAL-01 | Ground reference: a 5-second rolling mean | Unit: test_GND_CAL_01_reference_follows_slow_drift; Chain: test_T6_drift (2 hPa/h for an hour, within 1 m) | ✅ |
| GND-CAL-02 | The reference averages pressure, not altitude | Unit: test_GND_CAL_01_reference_follows_slow_drift (the reference is in pascals) | ✅ |
| GND-CAL-03 | A sample 50 Pa away is not averaged in | Unit: test_GND_CAL_02_reference_stops_tracking_when_the_rocket_moves | ✅ |
| GND-CAL-04 | Frozen from before T+0, not snapped | Unit: test_FLT_LAUNCH_09_freezing_keeps_the_hundred_feet; Chain: test_T7_ground_error (within 0.1 m at 2, 5, 15 and 30 g) | ✅ |
| GND-CAL-07 | A reference on under a second of pad is flagged | Chain: test_T7_early_launch_degraded | ✅ |
| GND-CAL-06 | Re-seed after a step | Chain: test_T6_step_reseeds (100-300 Pa either way), test_T6_launch_never_reseeds, test_T6_gusts_never_reseed, test_T6_rejecting_starts_at_zero | ✅ |
| FLT-LAUNCH-03 | T+0 at the first reading above 50 cm | Unit: test_REV07_launch_backdates_to_first_rise; Integration: test_FLT_LAUNCH_03_backdate; Chain: test_T3_latency (within 40 ms of the truth at 2-30 g) | ✅ |
| FLT-LAUNCH-07 | 100 ft and 5 m/s, held for 100 ms | Unit: test_FLT_LAUNCH_08_ten_metres_is_no_longer_enough, test_FLT_LAUNCH_01_detects_ascent; Chain: test_T3_pad_two_sample_glitch, test_T3_latency, test_T3_durations_not_counts | ✅ |
| GND-CAL-05 | LAUNCH reports the height reached | Integration: test_REV11_launch_row_reports_the_height_reached | ✅ |
| FLT-LAUNCH-04 | Log LAUNCH event | Integration: test_DAT_04_events | ✅ |
| FLT-LAUNCH-05 | Stop buzzer on launch | Integration: test_BUZ_07_03_lifecycle | ✅ |
| FLT-APO-01 | Apogee on clean fits, rising for 60 ms, 1.0001 above the lowest | Chain: test_T5_apogee (1000 flights, 100 m to 9 km), test_T3_coast_two_sample_glitch, test_T3_latency; Unit: test_FLT_APO_01_detects_apogee; Integration: test_FLT_APO_01_detected | ✅ |
| FLT-APO-02 | Transition to DESCENT | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-APO-03 | Log APOGEE event | Integration: test_DAT_04_events | ✅ |
| FLT-APO-04 | No apogee before armed | Integration: test_FLT_APO_04_no_apogee_before_armed | ✅ |
| FLT-ASC-01 | Track max altitude | Integration: test_FLT_APO_01_detected (max_altitude > 0) | ✅ |
| FLT-ASC-02 | Every speed from the fit | Chain: test_T4_pad_speed, test_T11_stalls_change_nothing, test_T5_through_the_clamp, test_T5_speed_and_delay_triggers | ✅ |
| FLT-ASC-03 | Thrust from the fit's acceleration | Chain: test_T5_under_thrust (ends within 1 s of burnout, one change); Integration: test_FLT_ASC_03_06_thrust_and_arming | ✅ |
| FLT-ASC-04 | Arm at <10 m/s | Closed-loop: pyros arm and fire in all flights | ✅ |
| FLT-ASC-05 | Log ARMED event | Integration: test_DAT_04_events | ✅ |
| FLT-ASC-06 | No arm above 10 m/s | Integration: test_FLT_ASC_03_06_thrust_and_arming | ✅ |
| FLT-ASC-07 | No arming unless the peak speed passed 10 m/s | Unit: test_FLT_ASC_07_arms_after_ten_metres_a_second; Integration: test_FLT_ASC_03_06_thrust_and_arming | ✅ |
| FLT-LAND-01 | Stable <1m for 1s | Integration: test_FLT_BOOT_01_all_states reaches LANDED; Chain: test_T4_touchdown (within 3 s, under noise, on the pad's level and 5 m above), test_T11_landing_holds_a_second (the full second, on odd and even sample times) | ✅ |
| FLT-LAND-02 | Speed <2 m/s | Integration: landing detected at correct time | ✅ |
| FLT-LAND-03 | Altitude <30m | Integration: landing detected at correct time | ✅ |
| FLT-LAND-04 | Transition to LANDED | Integration: test_FLT_LAND_04_duration | ✅ |
| FLT-LAND-05 | Log LANDING event | Integration: test_DAT_04_events | ✅ |
| FLT-LAND-06 | Stay in LANDED | Integration: state remains LANDED after detection | ✅ |
| FLT-LAND-07 | Landing timeout, on stillness | Chain: test_N7_no_landing_under_main (no LANDED under a 5 m/s main, and within 3 s of touchdown on the pad's level and 50 m above it) | ✅ |
| PYR-REFIRE-01 | One drogue retry at 2 s, only if unopened | Closed-loop: test_PYR_REFIRE_01_refire_ballistic | ✅ |
| PYR-REFIRE-02 | No retry on an opened channel | Closed-loop: test_PYR_REFIRE_02_no_retry_when_opened | ✅ |
| FLT-EMRG-01 | Main early on evidence the drogue failed | Closed-loop: test_FLT_EMRG_01, test_REV01_failed_drogue_brings_the_main_forward | ✅ |
| FLT-EMRG-02 | No bare descent-rate trigger | Closed-loop: test_FLT_EMRG_02_freefall_to_trigger_not_overridden | ✅ |
| FLT-EMRG-03 | Not on the absence of a settled descent | Closed-loop: test_REV01_working_drogue_main_at_its_trigger; `main_forced` false in every mode suite | ✅ |
| FLT-EMRG-04 | An emergency deployment is recorded | Closed-loop: test_REV16_forced_main_is_in_the_log; Hardware: `main_forced` on /api/status | ✅ |
| FLT-BROWN-01 | Pad marker at 10 s PAD_IDLE, with σ | Integration: test_BRN_INT_01/02/05; Chain: test_T5_sigma; Brownout: test_BRN_MARK_01/02/03; Hardware: `pad.mkr` present on MK1A/B/C | ✅ |
| FLT-BROWN-02 | Recover the ground reference, from medians since power-on | Brownout: test_BRN_01..08; Integration: test_BRN_INT_03/04; Chain: test_T1_rejoins_descent, test_T1_rejoins_ascent, test_T1_glitch_on_the_pad | ✅ |
| FLT-BROWN-03 | A stationary board is never airborne | Brownout: test_BRN_06/07; Chain: test_T1_still_board_stays_cold | ✅ |
| FLT-BROWN-04 | The marker is spent at landing | Chain: test_T1_marker_invalid_after_landing | ✅ |
| FLT-BROWN-05 | Why a boot was cold | Chain: test_T1_cold_reasons | ✅ |
| FLT-BROWN-06 | A recovered flight reads continuity and produces altitude | Chain: test_T1_rejoins_descent (the main fires at 300 m), test_T1_rejoins_ascent (the drogue fires at apogee) | ✅ |
| FLT-LOG-05 | No flash write through the shock window | Code review: log_flash_service() holdoff | ⚠ untested |
| FLT-LOG-06 | Log committed every second, in the window | Hardware (MK1C, instrumented bench build): readable while written; reset mid-log keeps rows to 0.5 s before it; flash_refusals 0 | ✅ HW |
| LUA-IO-01 | Export / import the Lua program | Playwright: lua program exports to a file; imports into the editor | ✅ |
| LUA-IO-02 | Import does not touch the device | Playwright: imports into the editor without saving; oversized import refused | ✅ |
| PIN-LABEL-01 | Connector designator per pin | Host: test_PIN_LABEL_01..03; Playwright: pin tables name the connector | ✅ |
| PIN-BUZZ-01 | Buzzer assignable to a pad | Host: test_PIN_BUZZ_01/02/05/06/07; Playwright: buzzer can be moved | ✅ |
| PIN-BUZZ-02 | Buzzer pad exclusive against Lua | Host: test_PIN_BUZZ_03/04 | ✅ |
| FLT-MACH-02 | The flag, set while the data is clean | Mach: test_M1_flag_before_mach_085 (by Mach 0.82), test_M1_subsonic_never_locks; Closed-loop: test_FLT_MACH_02_fast_subsonic_flight_not_locked | ✅ |
| FLT-MACH-03 | Released on a second of a coast's signature | Mach: test_M1_mid_mach_releases, test_M1_release_at_altitude (1000 seeds), test_M1_port_error_margin, test_M1_integer_forms, test_M1_design_note | ✅ |
| FLT-MACH-04 | The fallback | Mach: test_M1_fallback | ✅ |
| FLT-MACH-05 | No apogee while flagged | Mach: test_M1_no_drogue_before_apogee (both pads, both port signs), test_M0_report | ✅ |
| FLT-MACH-06 | No arming below 30 m; a recovered ascent starts flagged | Chain: test_M1_minimum_altitude_arm, test_M1_recovered_ascent_locked | ✅ |
| FLT-MACH-07 | The peak from outside the lock | Mach: test_M1_peak_outside_lock; Web: apogee skips the Mach lock, a lock let go near apogee makes it a lower bound, a lock that fell back makes it a lower bound | ✅ |
| FLT-DESC-01 | Phase from rate, not from command | Closed-loop: test_FLT_DESC_01_phase_without_pyros; Chain: test_N12_drogue_from_below (a drogue speeding up through the main's band is still a drogue) | ✅ |
| FLT-DESC-02 | Landing from every descent phase | Closed-loop: test_FLT_DESC_02_ballistic_reaches_landed | ✅ |
| PYR-ALT-01 | Clamp altitude settings | Closed-loop: Karman suite (AGL > 8000m clamped, pyro still fires) | ✅ |
| PYR-ALT-02 | Warning beep for range | Integration: test_PYR_ALT_02_cfg_range_beep | ✅ |
| FLT-RATE-01..02 | Sample rates | Integration: test_FLT_LAUNCH_01_timing (timing bounds); Chain: test_T9_one_shot_cadence (the one-shot schedule: 900 pressures in 10 s, a 20 ms gap only where the temperature was read); test_ms5607_ready_before_the_next_loop (at each MS5607 board's own bus rate, 400 kHz on MK1B and MK1C, each conversion is ready 0.7 ms before the next loop, DD-052), test_ms5607_work_costs_no_samples (2.7 ms of work a pressure, as measured, costs no conversion); the rate on a board is a bench check owed (DD-051) | ⚠️ |
| FLT-RATE-03 | 1 Hz while LANDED | Chain: test_N18_landed_logs_once_a_second | ✅ |
| FLT-RATE-04 | The rate is the HAL's | Chain: test_T9_same_outcomes (the same outcomes at 11 ms and 20 ms), test_T3_durations_not_counts (at 10 ms) | ✅ |
| FLT-RATE-05 | Holds and dwells in sample time | Chain: test_T11_loop_clock_independent (a loop clock lagging 0-70 ms changes no decision's sample) | ✅ |

## 2. Pre-Flight Status

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-STATUS-01 | Audible readiness | Integration: test_BUZ_07_03_lifecycle | ✅ |
| SYS-STATUS-02 | Verify pyro integrity | Integration: continuity checked before flight | ✅ |
| PYR-CONT-01 | Check every 1s | Unit: test_PYR_CONT_01_continuity_check | ✅ |
| PYR-CONT-03 | Diagnosis and announcement follow each check | Unit: test_REV04_pad_fault_after_boot_is_announced; Hardware: bench smoke test | ✅ |
| FLT-BOOT-16 | No fault for a released or disabled channel | Unit: test_REV_NEW_disabled_channel_is_not_a_fault; Hardware: bench smoke test | ✅ |
| PYR-CONT-02 | Report good/open/short | Web UI: pyro channels show OK/OPEN/FIRED | ✅ |
| BUZ-STATUS-01 | Distinct beep codes | Beep: test_shipped_table_is_valid, test_two_outcomes_that_sound_alike_are_refused | ✅ |
| BUZ-01..02 | Four outcomes, said again on a cadence | Integration: test_BEEP_01_clean_board_says_ok_to_fly, test_BEEP_02_a_pyro_fault_names_its_channel, test_BEEP_03_anything_unfixable_says_system_failure, test_BEEP_04_unfixable_outranks_fixable; Beep: test_shipped_cadence_keeps_talking; Buzzer: test_BUZ_PAT_07_gap_between_passes | ✅ |
| FLT-BOOT-01 | Non-blocking boot | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-BOOT-02..03 | Config read, and written when absent | Unit: test_FLT_BOOT_02_reads_config_at_boot, test_FLT_BOOT_03_writes_default_config (the RP2040's hal_config_load() is the same code as the host's) | ✅ |
| FLT-BOOT-04 | Settle wait | Integration: boot completes in expected time | ✅ |
| FLT-BOOT-05 | Detect and initialise the sensor | Unit: test_FLT_BOOT_01_reaches_pad_idle | ✅ |
| FLT-BOOT-06 | Initialise the pyro subsystem | Unit: test_FLT_BOOT_01_reaches_pad_idle | ✅ |
| FLT-BOOT-07 | Initial continuity check | Integration: test_FLT_BOOT_01_all_states (BOOT_CONTINUITY on the way to PAD_IDLE) | ✅ |
| FLT-BOOT-08 | Calibrate from the median of 10 readings | Unit: test_FLT_BOOT_08_calibrates_ground; Chain: test_T2_calibration_glitch | ✅ |
| FLT-BOOT-09 | 2s stabilization | Unit: test_FLT_BOOT_04_settle_wait (2.5 s) | ✅ |
| FLT-BOOT-11 | The sensor is tested before the pyros | Unit: test_SNS_PRES_01_boot_no_sensor (BOOT_SENSOR first; a failure never reaches the pyro test), test_FLT_BOOT_11_waits_for_the_sensor_bringup (a sensor still being brought up is waited for, not called missing) | ✅ |
| FLT-BOOT-12 | No sensor: FAULT and system failure | Unit: test_SNS_PRES_01_boot_no_sensor, test_FLT_BOOT_12_bringup_that_never_ends_is_fault; Bring-up: test_bringup_without_a_sensor on every board | ✅ |
| FLT-BOOT-13 | No calibration samples in 10 s: FAULT | Unit: test_FLT_BOOT_13_no_calibration_samples_is_fault | ✅ |
| FLT-BOOT-14 | No filesystem: FAULT and system failure | Unit: test_FLT_BOOT_14_no_filesystem_is_fault; Integration: test_BEEP_03_anything_unfixable_says_system_failure (the announcement) | ✅ |
| FLT-BOOT-15 | Every pad fault reported | Unit: test_REV04_pad_fault_after_boot_is_announced | ✅ |
| BUZ-CODE-01 | The vocabulary is the pad's actions | Integration: test_BEEP_01..04 | ✅ |
| BUZ-CODE-02 | Unfixable outranks a pyro fault | Integration: test_BEEP_03_anything_unfixable_says_system_failure, test_BEEP_04_unfixable_outranks_fixable | ✅ |
| BUZ-CODE-03 | The diagnosis by name on /api/status | Hardware: `faults` on /api/status (`support/api_check.py`) | ✅ HW |
| BUZ-CODE-04 | Key, meaning and sound per outcome | Beep: test_shipped_table_is_valid, test_three_personalities_all_named | ✅ |
| BUZ-CODE-05 | Chirp, tone, count or silence | Beep: test_ok_to_fly_is_a_chirp_not_a_count, test_silence_is_not_a_duplicate | ✅ |
| BUZ-CODE-06 | Counts of 1 to 9 | Beep: test_a_zero_beep_count_is_refused, test_a_count_above_nine_is_refused | ✅ |
| BUZ-CODE-07 | No two outcomes sound alike | Beep: test_two_outcomes_that_sound_alike_are_refused, test_two_chirps_are_a_duplicate | ✅ |
| BUZ-CODE-08 | A wholly silent personality is refused | Beep: test_a_wholly_silent_personality_is_refused | ✅ |
| BUZ-CODE-09 | Three named personalities, one active | Beep: test_three_personalities_all_named, test_the_active_personality_is_the_one_used | ✅ |
| BUZ-CODE-10 | An invalid table is rejected whole | Beep: test_an_unreadable_table_still_answers; Buzzer: test_BEEP_STORE_01/02 | ✅ |
| BUZ-CODE-11 | The vocabulary is served to the web UI | Hardware: GET /api/beeps (`support/api_check.py`) | ✅ HW |
| BUZ-CODE-12 | No beep.ini: the shipped table is written | Buzzer: test_BUZ_CODE_12_missing_table_is_written | ✅ |
| BUZ-CODE-13 | Eggtimer defaults | Beep: test_shipped_pyro_codes_follow_eggtimer | ✅ |

## 3. Flight Data Recovery

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-DATA-01 | Record flight data | Integration: test_DAT_04_events (samples > 100) | ✅ |
| SYS-DATA-02 | Export standard format | Integration: test_DAT_06_csv_export; Chain: test_T8_columns, test_T8_replay | ✅ |
| SYS-DATA-03 | Announce max altitude | Integration: test_BUZ_07_03_lifecycle | ✅ |
| DAT-01 | 4096-entry ring buffer | Integration: samples recorded throughout flight | ✅ |
| DAT-02 | Sample fields, at the sample's time | Integration: events have correct fields; Chain: test_T11_log_rows_at_sample_time, test_T8_columns | ✅ |
| DAT-08 | A log replays through the firmware | Chain: test_T8_replay (every event to the sample, no state diverging) | ✅ |
| DAT-03 | Events tag samples | Integration: test_DAT_04_events | ✅ |
| DAT-04 | Log all event types | Integration: test_DAT_04_events; Closed-loop: test_REV16_forced_main_is_in_the_log | ✅ |
| DAT-06 | CSV export | Integration: test_DAT_06_csv_export (flight.csv); Chain: test_T8_columns (flight_log.csv, closed at landing) | ✅ |
| DAT-07 | CSV metadata header | Integration: test_DAT_06_csv_export (ID, both channels, max altitude); Chain: test_T8_replay (reads the log's own header) | ✅ |
| BUZ-03..07 | Altitude beep-out | Integration: test_BUZ_07_03_lifecycle | ✅ |

## 4. Configuration

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-CFG-01 | Persistent config | Web UI: config persists across reboot | ✅ |
| SYS-CFG-02 | Config without tools | Web UI: config editor tests | ✅ |
| SYS-CFG-03 | Validate against limits | Web UI: range warning test | ✅ |
| CFG-01..09 | INI parsing | Closed-loop: all configs parsed and applied correctly | ✅ |
| CFG-04 | `none` survives the round trip; unknown modes stored as none | Config: test_config_mode_none_round_trips, test_config_unknown_mode_serialises_as_none; Hardware: bench smoke test | ✅ |
| CFG-07 | Shipped defaults fit their fields | Config: test_config_default_name_is_not_truncated; compile-time check in config.c | ✅ |
| SYS-CFG-04 | A new field changes one place | Config: test_config_roundtrip_defaults, test_config_roundtrip_custom run over every field of `config_fields.h` | ✅ |
| WEB-UI-02 | Guided editor; units convert; limits stated | Web UI: config tab tests, changing units converts the pyro values, the rocket name shows its 8-character limit | ✅ |
| WEB-UI-03 | Warn if not applied | Web UI: save shows confirmation | ✅ |

## 5. Altitude Measurement

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-ALT-01 | Barometric altitude | Integration: altitude tracks trajectory | ✅ |
| SYS-ALT-02 | Multiple sensors | — (hardware test only) | ⚠️ |
| SNS-PRES-01 | Auto-detect sensor | — (hardware test only) | ⚠️ |
| SNS-PRES-02..03 | Pressure filter, fractional | Unit: test_SNS_PRES_02_filter_smoothing, test_SNS_PRES_03_filter_init; Chain: test_T4_filter_noise (0.17 Pa of 1.2), test_T4_pad_speed (0.22 m/s) | ✅ |
| SNS-PRES-05 | Conversion read after its worst case | Hardware (MK1C, MK1B): pres_rejects 0 under HTTP load, 18 in 60 s without the timing. The read is now the one-shot's, at the same 9.1 ms (DD-051): test_SNS_PRES_05_read_after_worst_case; `support/prove_core0.py` proves its handler RAM-closed; the bench recheck is owed | ⚠️ |
| SNS-PRES-07 | A single outlier never reaches the filter | Chain: test_T2_pad_glitch_sweep, test_T2_coast_glitch, test_T2_median_timing | ✅ |
| SNS-PRES-08 | Each sample stamped by its driver at its measurement | MS5607: test_SNS_PRES_08_stamp_is_the_conversions, test_SNS_PRES_08_held_read_keeps_its_stamp (a read held 60 ms), test_SNS_PRES_08_late_loop_keeps_the_stamp (the handler's state machine on a fake bus); Chain: test_T11_stalls_change_nothing (under the test HAL's model of the stamping); the BMP280's stamp in `bmp280_read()` by inspection; bench check owed | ⚠️ |
| SNS-PRES-09 | A quadratic fit over the last second, and whether it is clean | Chain: test_T5_fit_reference, test_T5_fit_noise, test_T5_clean, test_T5_sigma, test_T5_sigma_ignores_the_launch, test_T9_same_outcomes (the whole second at ~90 Hz); Mach: test_T9_mach_at_90hz; the fit's cost on the RP2040 is a bench check owed | ✅ |
| SNS-PRES-10 | A stuck sensor: reported, and never a deployment | Mach: test_M2_stuck_in_coast, test_M2_reported, test_M2_real_sensor_never_stuck | ✅ |
| SNS-PRES-11 | A gap or a lost sensor: a whole window of new samples before any decision | Mach: test_M2_dropout_in_coast, test_M2_lost, test_M2_reported, test_M2_out_of_range | ✅ |
| SNS-PRES-12 | Each pressure compensated with the temperature at its own time | Chain: test_T9_temperature_reuse (0.71 Pa RMS, 2.00 worst, warming at 1 °C/s; 12.1 Pa reusing the last reading), test_T9_datasheet_example (the datasheet's worked example to the pascal) | ✅ |
| SNS-PRES-06 | Impossible readings discarded and counted | Hardware: pres_rejects on /api/status; the false launch they caused did not recur | ✅ HW |
| SNS-ALT-01..03 | Altitude computation | Integration: max altitude within expected range | ✅ |
| SNS-ALT-04 | Speed from the unclamped height | Chain: test_N26_apogee_above_8km, test_T3_coast_two_sample_glitch (a glitch's decay below the pad) | ✅ |

## 6. Telemetry

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-TEL-01 | Serial telemetry | Integration: test_TEL_01_output | ✅ |
| TEL-01..02 | NMEA format + checksum | Integration: test_TEL_01_output (checksum verified) | ✅ |
| TEL-03 | NMEA event sentences ($PYRO_APO, $PYRO_FIRE, $PYRO_LAND) | Integration: test_TEL_03_event_sentences | ✅ |
| TEL-04 | JSON format (telem_format=1) | Integration: test_TEL_04_json_format | ✅ |
| TEL-03..05 | Telemetry rates | Integration: ≥10 sentences during flight; Unit: test_REV12_telem_rate_hz_sets_the_flight_cadence | ✅ |
| TEL-05 | No $PYRO in boot or FAULT | Unit: test_REV08_fault_sends_no_state_sentence | ✅ |
| TEL-06..10 | Field contents | Integration: test_TEL_01_output | ✅ |

## 7. Pyro Fault Protection

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| PYR-FAULT-01 | Disable at >1.5A | N/A (hardware — AP2192) | ✅ HW |
| PYR-FAULT-02 | Detect overcurrent | Closed-loop: test_PYR_FAULT_02_overcurrent_detection | ✅ |
| PYR-FAULT-03 | Indicate overcurrent | Beep codes 2-3/3-3 + flight buffer events | ✅ |
| PYR-VERIFY-01 | Post-fire verification | check_post_fire_verify() + beep codes 2-4/3-4 | ✅ |

## 8. Web Interface & Network

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| WEB-NET-01..04 | USB network, DHCP, mDNS, DNS-SD | Hardware: support/test_network.py | ⚠️ HW |
| WEB-API-01 | GET /api/status | Web UI: status tests (3 modes) | ✅ |
| WEB-API-02 | GET /api/config | Web UI: config loads from device | ✅ |
| WEB-API-03 | POST /api/config | Web UI: save test | ✅ |
| WEB-API-04 | POST /api/ota | Hardware: OTA to MK1A/B/C, every littlefs file preserved | ✅ HW |
| WEB-API-05 | POST /api/reboot | Hardware: 200 with CORS, board back in PAD_IDLE | ✅ HW |
| WEB-API-06 | GET /api/flight.csv | Hardware: bench smoke test | ✅ HW |
| WEB-API-07 | CORS headers | Hardware: bench smoke test, every route | ✅ HW |
| WEB-API-08 | No state-changing request in flight | Unit: test_REV18_flight_in_progress_is_launch_to_landing (the predicate); route wiring by inspection | ⚠️ |
| WEB-API-09 | Erase the flight log | Web UI: the flight log can be erased; Hardware: bench smoke test | ✅ |
| WEB-API-10 | No file served while the log is written | `serve_file()` refuses on `hal_log_active()`, by inspection; bench check owed (section 6 of docs/outstanding_tasks.md) | ⚠️ |
| WEB-HTTP-01 | A request is a byte stream | HTTP: test_HTTP_02 (split at every byte), test_HTTP_03 (byte by byte, random), test_HTTP_04, test_HTTP_07; Hardware: `support/http_stream_check.py` 16/16 on MK1A/B/C (4/16 on the old server) | ✅ |
| WEB-HTTP-02 | Content-Length and Connection: close on every response | HTTP: body_of() asserts both on every test; Hardware: http_stream_check framing checks | ✅ |
| WEB-HTTP-03 | Flow control, not refusal | HTTP: test_HTTP_12 (a 10 kB body through a 2 kB ring into a sink that refuses 50 times); Hardware: uploads round-trip byte-exact, flash_refusals 0, Lua heartbeat unbroken on MK1C | ✅ |
| WEB-HTTP-04 | Status codes for bad requests | HTTP: test_HTTP_08, 09, 10, 11; Hardware: 405 and 413 in http_stream_check | ✅ |
| WEB-HTTP-05 | HTTP work from the main loop, stack-neutral | By construction: http_conn.c and net_ring.c build and test on the host with no lwIP; callbacks only queue (http_server.c) | ✅ |
| WEB-UI-01 | Status in config units | Web UI: altitude in meters/feet tests | ✅ |
| WEB-UI-04 | Flight summary + CSV, from the log, refreshed, named | Web UI: flight data tests, a flight recorded while the page is open appears on refresh; Unit: test_REV09_flight_time_freezes_at_landing | ✅ |
| WEB-UI-05 | Firmware upload | Web UI: update tab test | ✅ |

## 9. Firmware Update

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| OTA-01..04 | OTA, inactive slot, rollback, safety | — (hardware test only) | ⚠️ HW |

## 10. Ground Test Interface

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| GND-TEST-01 | BEEP STATUS replays last continuity code | Integration: test_GND_TEST_01_beep_status_replay | ✅ |
| GND-TEST-02 | ARM then FIRE within 3s fires the pyro | Integration: test_GND_TEST_02_arm_fire_sequence | ✅ |
| GND-TEST-03 | ARM auto-disarms after 3s timeout | Integration: test_GND_TEST_03_auto_disarm | ✅ |
| GND-TEST-04 | Commands rejected outside PAD_IDLE | Integration: test_GND_TEST_04_only_in_pad_idle | ✅ |
| DD-011 | NMEA-style $GT,... responses with XOR checksum | Integration: test_GND_TEST_02_arm_fire_sequence | ✅ |
| CFG-HAL-01 | hal_config_load() abstracts config storage | Integration: all tests load config via HAL | ✅ |
| CFG-HAL-02 | hal_config_save() persists config changes | Integration: hal_test.c implements in-memory save | ✅ |
| PWR-SLEEP-01 | CPU sleeps between events (hal_sleep_until_event) | Integration: no-op in test; __wfe on hardware | ✅ |

## 11. Portability & Testability

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| HAL-01 | No #ifdef in flight code | CI: grep verification | ✅ |
| HAL-02..04 | HAL interface | CI: all 3 targets build from same source | ✅ |

## 12. Build & Test System

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| BLD-01..05 | Build targets | CI: build succeeds | ✅ |
| TST-01..03 | Unit/integration/closed-loop | CI: all pass | ✅ |
| TST-04 | All 4 pyro modes | Closed-loop: 7 config suites | ✅ |
| TST-05 | 100ft to 100km | Closed-loop: 4 altitude profiles | ✅ |
| TST-06 | Chute reduces descent | Closed-loop: test_TST_06_chute_effect | ✅ |
| TST-07 | Web UI tests | CI: Playwright tests | ✅ |
| TST-08 | CI on every push | GitHub Actions | ✅ |
| TST-09 | Traceable to requirements | This document | ✅ |

---

## 13. Power Management (v2.0)

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| PWR-SAMPLE-01 | 50Hz pressure sampling via async task | Integration: all tests use hal_pressure_fifo_* | ✅ |
| PWR-SAMPLE-02 | 5-sample batch delivery | Integration: flight_process_samples() in all integration runs | ✅ |
| PWR-TELEM-01 | Async telemetry TX | Host: buzzer_tests / integration (non-blocking send) | ✅ |
| PWR-BUZZ-01 | Autonomous buzzer via async task | Buzzer: test_BUZ_ACT_01..04, test_BUZ_PAT_01..04, test_BUZ_PAT_06..10 | ✅ |
| BUZ-CODE-14 | A storage failure is not a validation failure | Buzzer: test_BEEP_STORE_01/02 | ✅ |
| PWR-SLEEP-01 | CPU sleep between events | Integration: no-op in test; __wfe on hardware | ✅ |
| PWR-LOG-01 | RAM-buffered async flash logging | Integration: hal_log_sample() called; mock records calls | ✅ |
| PWR-LOG-02 | hal_log_start() opens file + registers task | Integration: test_FLT_BOOT_01 (log starts on LAUNCH) | ✅ |
| PWR-LOG-03 | hal_log_sample() is non-blocking | Integration: called 50×/s during flight; no stall | ✅ |
| PWR-LOG-04 | hal_log_stop() signals flush close | Integration: test_FLT_LAND_04 (log stops on LANDED) | ✅ |
| PWR-BUZZ-02 | Buzzer: IDLE → ENCODE → PLAYING states | Buzzer: test_BUZ_ACT_01_lifecycle, test_BUZ_ACT_02_stop (active and idle; no test observes ENCODE) | ⚠️ |
| PWR-BUZZ-03 | Pattern computed at request time | Buzzer: test_BUZ_PAT_02_counted_codes, test_BUZ_PAT_03_altitude_165_digits | ✅ |
| PWR-TELEM-02 | hal_telemetry_send() O(n), no stall | Buzzer/Integration: verified by non-blocking assertion | ✅ |
| PWR-TELEM-03 | TX ring ≥ 512 bytes; overflow drops end | Host: hal_test.c mock buffers full sentence | ✅ |

## 14. Configuration System (v2.0)

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| CFG-TABLE-01 | X-macro single-table config | Config: test_config_defaults, test_config_roundtrip_defaults (every field from `config_fields.h`) | ✅ |
| CFG-TABLE-02 | Round-trip serialize → parse | Config: test_config_roundtrip_defaults, test_config_roundtrip_custom | ✅ |
| CFG-SUBSYS-01 | Each subsystem has configurable params, every key read | Config: test_config_writes_no_inert_keys; Unit: telem_rate_hz; Integration: test_REV12_log_rate_hz_thins_samples_not_events | ✅ |

## 15. Telemetry Formatting (v2.0)

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| TELEM-FMT-01 | telemetry_formatter.c module | Integration: TEL-03 / TEL-04 tests | ✅ |
| TELEM-FMT-02 | Event + state messages | Integration: test_TEL_03_event_sentences | ✅ |
| TELEM-FMT-03 | HAL transport is raw bytes | Integration: hal_telemetry_send(const char*) | ✅ |

## 16. On USB

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-13 | Quiet and grounded on the bench | USB-01..05, 07, 08. Not for a charger (USB-06) | ⚠️ |
| SYS-USB-01 | No flight, no announcement on USB | As UN-13 | ⚠️ |
| USB-01 | No launch, recovery or marker on USB | Unit: test_USB_01_no_launch_while_attached; Integration: test_USB_INT_01_marker_waits_for_the_cable_to_go, test_USB_INT_02_no_flight_recovery_on_usb; Hardware: 0 flash programs after 50-90 s of PAD_IDLE on MK1A/B/C | ✅ |
| USB-02 | Nothing announced on USB | Unit: test_USB_02, test_USB_03, test_USB_04, test_USB_05; Hardware: `buzzer_active` false on MK1A/B/C (`support/api_check.py`) | ✅ |
| USB-03 | One double chirp on attach | Buzzer: test_BUZ_PAT_10_usb_ok_is_one_double_chirp; Unit: test_USB_02; Integration: test_USB_INT_01 | ✅ |
| USB-04 | Resume on detach | Unit: test_USB_02, test_USB_04, test_USB_05; Integration: test_USB_INT_01. Not on hardware: nobody unplugged a board | ✅ |
| USB-05 | Ignored in flight | Unit: test_USB_06_ignored_once_airborne | ✅ |
| USB-06 | A charger counts as USB | No hardware path (DD-037) | ❌ |
| USB-08 | Test mode flies on USB, RAM only | Unit: test_USB_07_test_mode_flies_on_usb, test_USB_08_test_mode_announces_and_leaving_it_chirps, test_USB_09_test_mode_is_off_at_boot, test_USB_10_test_mode_does_not_change_in_flight; Integration: test_USB_INT_03_test_mode_writes_the_marker_on_usb; Web: 3 tests; Hardware: on MK1A/B/C test mode announced and wrote pad.mkr on USB, went quiet when turned off, and was off after a reboot | ✅ |
| USB-07 | Errors leave launch detection on | By design: SOF needs a host (DD-037); Hardware: `usb_attached` true on MK1A/B/C with a PC attached | ✅ |

---

## 17. User Needs and System Requirements

A user need is verified through the system requirements under it, and is marked ⚠️ if any of them is.

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-2 | Know it is ready before the pad | Through SYS-STATUS-01, SYS-STATUS-02 | ✅ |
| UN-3 | Flight data after recovery | Through SYS-DATA-01..03 | ✅ |
| UN-4 | Configure for different rockets | Through SYS-CFG-01..04, CFG-SUBSYS-01 | ✅ |
| UN-5 | Accurate altitude | Through SYS-ALT-01, SYS-ALT-02; SYS-ALT-02 is hardware only | ⚠️ |
| UN-6 | Real-time telemetry | Through SYS-TEL-01 | ✅ |
| UN-7 | Protection against pyro faults | Through SYS-FAULT-01..03 | ✅ |
| UN-8 | Monitor, configure and update without special software | Through SYS-WEB-01, SYS-WEB-02; SYS-WEB-02 is hardware only | ⚠️ |
| UN-9 | Update without bricking | Through SYS-OTA-01, SYS-OTA-02; SYS-OTA-02 is hardware only | ⚠️ |
| UN-10 | Develop without flight hardware | Through SYS-PORT-01, SYS-PORT-02 | ✅ |
| UN-11 | Long pad time on battery | Through SYS-PWR-01, SYS-PWR-02 | ⚠️ |
| UN-12 | Ground checks without a computer | Through SYS-TEST-01 | ✅ |
| SYS-WEB-01 | Web interface over USB | Web UI: the Playwright suite; Hardware: `test/web/hw_ui_check.js` on MK1A/B/C | ✅ |
| SYS-WEB-02 | Discoverable without configuration | Through WEB-NET-01..04 (hardware only) | ⚠️ HW |
| SYS-OTA-01 | Update without physical access | Through WEB-API-04 | ✅ HW |
| SYS-OTA-02 | Recover from a failed update | Through OTA-01..04 (hardware only) | ⚠️ HW |
| SYS-FAULT-01 | Limit pyro current | Through PYR-FAULT-01 | ✅ HW |
| SYS-FAULT-02 | Detect pyro faults | Closed-loop: test_PYR_FAULT_02_overcurrent_detection | ✅ |
| SYS-FAULT-03 | Notify pyro faults | Through PYR-FAULT-03 | ✅ |
| SYS-PORT-01 | Testable on a host | Every host suite, in CI | ✅ |
| SYS-PORT-02 | Runnable in a browser | Web: test_sim.spec.js flies docs/sim.html's WASM build from power-on to LANDED; CI checks docs/app against www/ (`scripts/sync_demo.sh --check`) | ✅ |
| SYS-PWR-01 | Minimise CPU active time | Through PWR-SLEEP-01; no test measures CPU active time | ⚠️ |
| SYS-PWR-02 | I/O without the CPU | Through PWR-SAMPLE-01/02, PWR-TELEM-01..03, PWR-BUZZ-01, PWR-LOG-01..04 | ✅ |
| PWR-WAIT-01 | No sleeps: the exec loop is the only clock | Board pyro: test_mk1b_continuity_never_sleeps, test_mk1b_reads_after_the_settle (MK1B's continuity against a fake SDK whose sleeps fail the test); Bring-up, on every board: test_bringup_finds_its_sensor, test_bringup_recovers_the_bus_first, test_bringup_waits_out_the_reset (bus recovery, settles and sensor resets as loop steps); Board pyro (MK1C): test_mk1c_only_the_tracking_test_runs (the real backend against the plant: nothing it does holds the loop); `support/wait_check.py` in CI, which fails on any sleep or busy-wait in `src/` or `boards/`, and finds none (DD-053, DD-055) | ✅ |
| PWR-USB-01 | USB serviced autonomously | — (deferred to v2.1; USB is serviced from the main loop) | ⚠️ |
| SYS-TEST-01 | Ground test over serial | Integration: test_GND_TEST_01..04 | ✅ |

---

## Summary

| Status | Count |
|--------|-------|
| ✅ Verified by a host, web or closed-loop test | 222 |
| ⚠️ Not directly verified (needs a test or hardware) | 21 |
| ❌ Not implemented | 1 (USB-06: no hardware path) |
| ✅ HW (hardware satisfies) | 11 |

Rows of the tables above. `support/trace_check.py --counts` computes them, and CI fails when this table disagrees.

_+8 requirements in v2 Task 2/3 (GND-TEST-01..04, DD-011, CFG-HAL-01..02, PWR-SLEEP-01)_
_+2 requirements in v2 Task 5 (TEL-03 event sentences, TEL-04 JSON format)_
_+20 requirements in v2 Tasks 7–10 (PWR-*, CFG-TABLE-*, TELEM-FMT-*), all verified by buzzer/config/integration tests_

### Remaining gaps (hardware or future work only):
- **FLT-RATE-01..04**: Sample rate precision — hardware timing test
- **PYR-CONT-01**: Continuity check period — hardware timing test
- **BUZ-STATUS-01 / BUZ-01..02**: Beep codes / startup chirps — hardware audio test
- **FLT-BOOT-02..03 / FLT-BOOT-09**: Config load and settle timing — hardware test
- **SYS-DATA-02 / DAT-06..07**: CSV export format — hardware integration test
- **SYS-ALT-02 / SNS-PRES-01**: Multi-sensor detection — hardware test
- **WEB-NET-01..04**: USB network / mDNS / DNS-SD — hardware test
- **WEB-API-08**: the in-flight refusal is wired into on_head() in http_server.c and verified by inspection; a bench board cannot be put in flight to exercise it
- **OTA-01..04**: OTA update flow — hardware test
- **PWR-USB-01**: USB servicing autonomy — deferred to v2.1
