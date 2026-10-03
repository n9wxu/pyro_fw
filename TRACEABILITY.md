# Requirements Traceability Matrix

Each live requirement of `REQUIREMENTS.md` and the tests that verify it.
Withdrawn requirements (its Appendix A) have no row. Tests are black box and
trace to a requirement (TST-01, CODE-08).

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
| PYR-DEPLOY-01 | Both channels may deploy on one event | Closed-loop: test_PYR_DEPLOY_01_low_flight_fires_both | ✅ |
| PYR-DEPLOY-02 | Not energised at the same instant, flight or ground test | `flight_pyro_energise()`, DD-021; Integration: test_REV06_ground_test_waits_for_the_other_channel; Board pyro (MK1C): test_mk1c_both_channels_one_after_the_other (never both gates) | ❌ changed 2026-10: `fire_gap` and the tie order |
| PYR-FIRE-01 | Fired only when energised; a refusal is recorded once | Unit: test_REV03_refused_fire_is_not_recorded_as_fired; Hardware: `pyro1_refused` on /api/status | ❌ changed 2026-10: no refusals; every pulse recorded with what the board observed |
| PYR-MODE-05 | AGL / FALLEN on the fit's height, SPEED on its speed, DELAY from its apogee | Chain: test_T5_speed_and_delay_triggers (SPEED within 1 m/s, DELAY within 0.1 s); Mach: test_T5_ejection (AGL within 8 m); Closed-loop: test_REV05_agl_drogue_fires_at_its_altitude; every AGL channel in the mode suites within 8 m | ❌ changed 2026-10: the filtered state of DD-085 |
| PYR-MODE-06 | Pressure triggers wait out an unclean fit, 2 s at most; believed after a charge if no lower than ballistic | Mach: test_T5_ejection (5 kPa bay charge), test_T5_canopy_swing; Chain: test_T5_descent_glitch; Closed-loop: test_PYR_MODE_02_agl_agl (a main 60 m below a drogue opening at 105 m/s) | ❌ changed 2026-10: the free-fall bound in place of the clean-fit wait |
| PYR-SAFE-03 | Single fire per channel | Closed-loop: verified by fire_count | ❌ changed 2026-10: re-fire and emergency fire change what a second pulse is |
| PYR-SAFE-04 | No fire before apogee | Closed-loop: drogue fires at/after apogee | ✅ |
| FLT-LAUNCH-02 | Stay at ground level | Integration: PAD_IDLE persists before launch | ✅ |
| GND-CAL-01 | Ground reference: a 5-second rolling mean | Unit: test_GND_CAL_01_reference_follows_slow_drift; Chain: test_T6_drift (2 hPa/h for an hour, within 1 m) | ✅ |
| GND-CAL-03 | A sample 50 Pa away is not averaged in | Unit: test_GND_CAL_02_reference_stops_tracking_when_the_rocket_moves | ✅ |
| GND-CAL-04 | Frozen from before T+0, not snapped | Unit: test_FLT_LAUNCH_09_freezing_keeps_the_hundred_feet; Chain: test_T7_ground_error (within 0.1 m at 2, 5, 15 and 30 g) | ✅ |
| GND-CAL-07 | A reference on under a second of pad is flagged | Chain: test_T7_early_launch_degraded | ✅ |
| GND-CAL-06 | Re-seed after a step | Chain: test_T6_step_reseeds (100-300 Pa either way), test_T6_launch_never_reseeds, test_T6_gusts_never_reseed, test_T6_rejecting_starts_at_zero | ✅ |
| FLT-LAUNCH-03 | T+0 at the first reading above 50 cm | Unit: test_REV07_launch_backdates_to_first_rise; Integration: test_FLT_LAUNCH_03_backdate; Chain: test_T3_latency (within 40 ms of the truth at 2-30 g) | ✅ |
| FLT-LAUNCH-07 | 100 ft and 5 m/s, held for 100 ms | Unit: test_FLT_LAUNCH_08_ten_metres_is_no_longer_enough, test_FLT_LAUNCH_01_detects_ascent; Chain: test_T3_pad_two_sample_glitch, test_T3_latency, test_T3_durations_not_counts | ✅ |
| GND-CAL-05 | LAUNCH reports the height reached | Integration: test_REV11_launch_row_reports_the_height_reached | ✅ |
| FLT-LAUNCH-04 | Log LAUNCH event | Integration: test_DAT_04_events | ✅ |
| FLT-LAUNCH-05 | Stop buzzer on launch | Integration: test_BUZ_07_03_lifecycle | ✅ |
| FLT-APO-01 | Apogee on clean fits, rising for 60 ms, 1.0001 above the lowest | Chain: test_T5_apogee (1000 flights, 100 m to 9 km), test_T3_coast_two_sample_glitch, test_T3_latency; Unit: test_FLT_APO_01_detects_apogee; Integration: test_FLT_APO_01_detected | ❌ changed 2026-10: lateness bound at every height; estimator DD-085 |
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
| FLT-LAND-02 | Speed <2 m/s | Integration: landing detected at correct time | ✅ |
| FLT-LAND-03 | Altitude <30m | Integration: landing detected at correct time | ✅ |
| FLT-LAND-04 | Transition to LANDED | Integration: test_FLT_LAND_04_duration | ✅ |
| FLT-LAND-05 | Log LANDING event | Integration: test_DAT_04_events | ✅ |
| FLT-LAND-06 | Stay in LANDED | Integration: state remains LANDED after detection | ✅ |
| FLT-LAND-07 | Landing timeout, on stillness | Chain: test_N7_no_landing_under_main (no LANDED under a 5 m/s main, and within 3 s of touchdown on the pad's level and 50 m above it) | ❌ changed 2026-10: `landing_timeout` |
| PYR-REFIRE-01 | One drogue retry at 2 s, only if unopened | Closed-loop: test_PYR_REFIRE_01_refire_ballistic | ❌ changed 2026-10: re-fire on descent speed, per channel, every `refire_interval` |
| FLT-EMRG-01 | Main early on evidence the drogue failed | Closed-loop: test_FLT_EMRG_01, test_REV01_failed_drogue_brings_the_main_forward | ❌ changed 2026-10: emergency all-fire at `emergency_fire_speed` |
| FLT-EMRG-04 | An emergency deployment is recorded | Closed-loop: test_REV16_forced_main_is_in_the_log; Hardware: `main_forced` on /api/status | ❌ changed 2026-10: EMERGENCY_FIRE event and counts |
| FLT-BROWN-01 | Pad marker at 10 s PAD_IDLE, with σ | Integration: test_BRN_INT_01/02/05; Chain: test_T5_sigma; Brownout: test_BRN_MARK_01/02/03; Hardware: `pad.mkr` present on MK1A/B/C | ✅ |
| FLT-BROWN-02 | Recover the ground reference, from medians since power-on | Brownout: test_BRN_01..08; Integration: test_BRN_INT_03/04; Chain: test_T1_rejoins_descent, test_T1_rejoins_ascent, test_T1_glitch_on_the_pad | ❌ changed 2026-10: resume after any restart |
| FLT-BROWN-03 | A stationary board is never airborne | Brownout: test_BRN_06/07; Chain: test_T1_still_board_stays_cold | ✅ |
| FLT-BROWN-04 | The marker is spent at landing | Chain: test_T1_marker_invalid_after_landing | ❌ changed 2026-10: cleared at the end of a bench flight too |
| FLT-BROWN-05 | Why a boot was cold | Chain: test_T1_cold_reasons | ❌ changed 2026-10: the reasons no longer include "not a power event" |
| FLT-BROWN-06 | A recovered flight reads continuity and produces altitude | Chain: test_T1_rejoins_descent (the main fires at 300 m), test_T1_rejoins_ascent (the drogue fires at apogee) | ❌ changed 2026-10: channels assumed unfired; DELAY in full from the resume |
| FLT-LOG-07 | Three logging plans; binary on disk | Plan: test_PLAN_01..06 (what each keeps, windows that overlap, the log's end, the base rate resuming, a stream faster than the delay line, time order throughout); Integration: test_FLT_LOG_07_the_three_logging_plans (1.0-1.1 s apart; 90+ rows within a second of apogee; 15x more at full; every event in all three); Format: test_FLOG_01..08 | ✅ |
| FLT-LOG-06 | Log committed every second, in the window | Hardware (MK1C, instrumented bench build): readable while written; reset mid-log keeps rows to 0.5 s before it; flash_refusals 0 | ✅ HW |
| LUA-IO-01 | Export / import the Lua program | Playwright: lua program exports to a file; imports into the editor | ✅ |
| LUA-IO-02 | Import does not touch the device | Playwright: imports into the editor without saving; oversized import refused | ✅ |
| PIN-LABEL-01 | Connector designator per pin | Host: test_PIN_LABEL_01..03; Playwright: pin tables name the connector | ✅ |
| PIN-BUZZ-01 | Buzzer assignable to a pad | Host: test_PIN_BUZZ_01/02/05/06/07; Playwright: buzzer can be moved | ✅ |
| PIN-BUZZ-02 | Buzzer pad exclusive against Lua | Host: test_PIN_BUZZ_03/04 | ✅ |
| FLT-MACH-02 | The flag, set while the data is clean | Mach: test_M1_flag_before_mach_085 (by Mach 0.82), test_M1_subsonic_never_locks; Closed-loop: test_FLT_MACH_02_fast_subsonic_flight_not_locked | ✅ |
| FLT-MACH-03 | Released on a second of a coast's signature | Mach: test_M1_mid_mach_releases, test_M1_release_at_altitude (1000 seeds), test_M1_port_error_margin, test_M1_integer_forms, test_M1_design_note | ❌ changed 2026-10: release reached at every height; estimator DD-085 |
| FLT-MACH-04 | The fallback | Mach: test_M1_fallback | ❌ changed 2026-10: apogee from a sustained rise under the flag; the flag-level fallback is rejected |
| FLT-MACH-05 | No apogee while flagged | Mach: test_M1_no_drogue_before_apogee (both pads, both port signs), test_M0_report | ✅ |
| FLT-MACH-06 | No arming below 30 m; a recovered ascent starts flagged | Chain: test_M1_minimum_altitude_arm, test_M1_recovered_ascent_locked | ✅ |
| FLT-MACH-07 | The peak from outside the lock | Mach: test_M1_peak_outside_lock; Web: apogee skips the Mach lock, a lock let go near apogee makes it a lower bound, a lock that fell back makes it a lower bound | ✅ |
| FLT-DESC-01 | Phase from rate, not from command | Closed-loop: test_FLT_DESC_01_phase_without_pyros; Chain: test_N12_drogue_from_below (a drogue speeding up through the main's band is still a drogue) | ✅ |
| FLT-DESC-02 | Landing from every descent phase | Closed-loop: test_FLT_DESC_02_ballistic_reaches_landed | ✅ |
| FLT-AIR-01 | Descent rates in the pad's air | Closed-loop: test_FLT_AIR_01_air_scale_is_the_pad_air_rate (against the bench flight's atmosphere, 0-32 km, two pads), test_SIM_02_the_flight_software_flies_a_30_km_profile (no forced main from a working drogue), test_FLT_AIR_01_a_failed_drogue_is_seen_at_30_km, test_FLT_AIR_01_a_high_pad_flies_20_km | ✅ |
| FLT-RATE-01 | Sample rates | Integration: test_FLT_LAUNCH_01_timing (timing bounds); Chain: test_T9_one_shot_cadence (the one-shot at the loop period: 498 pressures in 10 s at 20 ms, every interval one loop, DD-066); test_ms5607_pair_ready_before_the_next_loop (at each MS5607 board's own bus rate, 400 kHz on MK1B and MK1C, the pair is ready 1.4 ms before the next loop, DD-052), test_ms5607_a_pair_every_loop (2.7 ms of work a pressure, as measured, costs no pair); Hardware (2.1.697, 2026-09-27, `support/pressure_trace.py`, 60 s each): MK1C and both MK1Bs 50.0 pressures a second, a temperature with each, no gaps, no missed slots; MK1A's BMP280, one forced conversion a loop (2.1.701, DD-067), 50.0 a second, consistently good | ✅ |
| FLT-RATE-05 | Holds and dwells in sample time | Chain: test_T11_loop_clock_independent (a loop clock lagging 0-70 ms changes no decision's sample) | ✅ |

## 2. Pre-Flight Status

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-STATUS-01 | Audible readiness | Integration: test_BUZ_07_03_lifecycle | ✅ |
| SYS-STATUS-02 | Verify pyro integrity | Integration: continuity checked before flight | ✅ |
| PYR-CONT-01 | Check every 1s | Unit: test_PYR_CONT_01_continuity_check | ✅ |
| PYR-CONT-03 | Diagnosis and announcement follow each check | Unit: test_REV04_pad_fault_after_boot_is_announced; Hardware: bench smoke test | ✅ |
| PYR-CONT-02 | Report good/open/short | Web UI: pyro channels show OK/OPEN/FIRED; Board pyro (MK1B): test_mk1b_igniter_reads_good, test_mk1b_empty_connector_reads_open, test_mk1b_short_to_ground_reads_shorted, test_mk1b_bad_joint_reads_good_with_its_count (DD-059) | ❌ changed 2026-10: ready or fault |
| BUZ-01..02 | Four outcomes, said again on a cadence | Integration: test_BEEP_01_clean_board_says_ok_to_fly, test_BEEP_02_a_pyro_fault_names_its_channel, test_BEEP_03_anything_unfixable_says_system_failure, test_BEEP_04_unfixable_outranks_fixable; Beep: test_shipped_cadence_keeps_talking; Buzzer: test_BUZ_PAT_07_gap_between_passes | ✅ |
| FLT-BOOT-01 | Non-blocking boot | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-BOOT-02..03 | Config read, and written when absent | Unit: test_FLT_BOOT_02_reads_config_at_boot, test_FLT_BOOT_03_writes_default_config (the RP2040's hal_config_load() is the same code as the host's) | ✅ |
| FLT-BOOT-05 | Detect and initialise the sensor | Unit: test_FLT_BOOT_01_reaches_pad_idle | ✅ |
| FLT-BOOT-06 | Initialise the pyro subsystem | Unit: test_FLT_BOOT_01_reaches_pad_idle | ✅ |
| FLT-BOOT-07 | Initial continuity check | Integration: test_FLT_BOOT_01_all_states (BOOT_CONTINUITY on the way to PAD_IDLE) | ✅ |
| FLT-BOOT-08 | Calibrate from the median of 10 readings | Unit: test_FLT_BOOT_08_calibrates_ground; Chain: test_T2_calibration_glitch | ✅ |
| FLT-BOOT-12 | No sensor: FAULT and system failure | Unit: test_SNS_PRES_01_boot_no_sensor, test_FLT_BOOT_12_bringup_that_never_ends_is_fault; Bring-up: test_bringup_without_a_sensor on every board | ✅ |
| FLT-BOOT-13 | No calibration samples in 10 s: FAULT | Unit: test_FLT_BOOT_13_no_calibration_samples_is_fault | ✅ |
| FLT-BOOT-14 | No filesystem: FAULT and system failure | Unit: test_FLT_BOOT_14_no_filesystem_is_fault; Integration: test_BEEP_03_anything_unfixable_says_system_failure (the announcement) | ✅ |
| FLT-BOOT-15 | Every pad fault reported | Unit: test_REV04_pad_fault_after_boot_is_announced | ❌ changed 2026-10: one fault announced by priority, every fault on the status report |
| BUZ-CODE-01 | The vocabulary is the pad's actions | Integration: test_BEEP_01..04 | ✅ |
| BUZ-CODE-02 | Unfixable outranks a pyro fault | Integration: test_BEEP_03_anything_unfixable_says_system_failure, test_BEEP_04_unfixable_outranks_fixable | ❌ changed 2026-10: priority general, pyro 1, pyro 2 |
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
| SYS-DATA-04 | The flight's motion on an SD card | Through SD-01, SD-02 (hardware) and HR-01..06 | ⚠️ |
| DAT-02 | Sample fields, at the sample's time | Integration: events have correct fields; Chain: test_T11_log_rows_at_sample_time, test_T8_columns | ✅ |
| DAT-08 | A high-rate log replays through the firmware | Chain: test_T8_replay (every event to the sample, no state diverging), test_T8_replay_refuses_a_thinned_log | ✅ |
| DAT-03 | Events tag samples | Integration: test_DAT_04_events | ✅ |
| DAT-04 | Log all event types | Integration: test_DAT_04_events; Closed-loop: test_REV16_forced_main_is_in_the_log | ❌ changed 2026-10: REFIRE, EMERGENCY_FIRE and RESUMED events; REFUSED and MAIN_FORCED go |
| DAT-06 | Kept as binary, exported as CSV | Integration: test_DAT_06_csv_export (flight.csv); Chain: test_T8_columns (the binary log rendered, closed at landing); Format: test_FLOG_02, test_FLOG_06 (a log cut anywhere) | ✅ |
| DAT-07 | CSV metadata header | Integration: test_DAT_06_csv_export (ID, both channels, max altitude), test_FLT_LOG_07_the_three_logging_plans (the rate); Format: test_FLOG_01; Chain: test_T8_replay (reads the log's own header) | ✅ |
| BUZ-03..07 | Altitude beep-out | Integration: test_BUZ_07_03_lifecycle | ✅ |

## 4. Configuration

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-CFG-01 | Persistent config | Web UI: config persists across reboot | ✅ |
| SYS-CFG-02 | Config without tools | Web UI: config editor tests | ✅ |
| SYS-CFG-03 | Validate against limits | Web UI: range warning test | ✅ |
| CFG-01..09 | INI parsing | Closed-loop: all configs parsed and applied correctly | ❌ changed 2026-10: the five pyro fields; the two telemetry keys go |
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
| SNS-PRES-08 | Each sample stamped by its driver at its measurement | MS5607: test_SNS_PRES_08_stamps_are_the_conversions, test_SNS_PRES_08_held_read_keeps_its_stamp (a read held 60 ms), test_SNS_PRES_08_late_loop_keeps_the_stamps (the handler's state machine on a fake bus); Chain: test_T11_stalls_change_nothing (under the test HAL's model of the stamping); BMP280: test_bmp280_stamp_is_the_conversions (the forced conversion's command, through a late loop, DD-067); bench check owed | ⚠️ |
| SNS-PRES-10 | A stuck sensor: reported, and never a deployment | Mach: test_M2_stuck_in_coast, test_M2_reported, test_M2_real_sensor_never_stuck | ✅ |
| SNS-PRES-11 | A gap or a lost sensor: a whole window of new samples before any decision | Mach: test_M2_dropout_in_coast, test_M2_lost, test_M2_reported, test_M2_out_of_range | ✅ |
| SNS-PRES-14 | A conversion a flash operation ran beside is not used | MS5607: test_ms5607_flash_during_the_pressure_marks_it, test_ms5607_flash_during_the_temperature_marks_it, test_ms5607_cycle_skips_a_flashed_temperature; BMP280: test_bmp280_flash_during_the_conversion_marks_it; Status: test_SJ_01_keys_order_and_formatting_are_the_api (`pres_flashed`); `support/pressure_trace.py --selftest` (a discarded pressure explains its slot); Hardware (2.1.701): G4 on all four boards discards 23-38 conversions each, no rejects, no Mach flag; on the bench MK1B the discarded pressures scattered twice the kept (DD-068) | ✅ |
| SNS-PRES-13 | Every conversion, served for the bench | Trace: test_PTRACE_01..04 (order, numbering, a wrapped ring says what it lost); `support/pressure_trace.py --selftest`; Hardware: the four bench boards traced (DD-063) | ✅ |
| SNS-PRES-06 | Impossible readings discarded and counted | Hardware: pres_rejects on /api/status; the false launch they caused did not recur | ❌ changed 2026-10: a reading beyond the rated range is used |
| SNS-ALT-01..03 | Altitude computation | Integration: max altitude within expected range | ❌ changed 2026-10: no clamp at 8000 m or at zero |

## 6. Telemetry

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-TEL-01 | Serial telemetry | Integration: test_TEL_01_output | ✅ |
| TEL-01..02 | NMEA format + checksum | Integration: test_TEL_01_output (checksum verified) | ✅ |
| TEL-03 | NMEA event sentences ($PYRO_APO, $PYRO_FIRE, $PYRO_LAND) | Integration: test_TEL_03_event_sentences | ❌ changed 2026-10: one message a second in every state |
| TEL-03..05 | Telemetry rates | Integration: ≥10 sentences during flight; Unit: test_REV12_telem_rate_hz_sets_the_flight_cadence | ❌ changed 2026-10: one message a second in every state |
| TEL-05 | No $PYRO in boot or FAULT | Unit: test_REV08_fault_sends_no_state_sentence | ✅ |
| TEL-06..10 | Field contents | Integration: test_TEL_01_output | ✅ |

## 7. Pyro Fault Protection

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| PYR-FAULT-01 | Disable at >1.5A | N/A (hardware — AP2192) | ❌ changed 2026-10: protection never prevents the next attempt |
| PYR-FAULT-02 | Detect overcurrent | Closed-loop: test_PYR_FAULT_02_overcurrent_detection | ✅ |
| PYR-FAULT-03 | Indicate overcurrent | Beep codes 2-3/3-3 + flight buffer events | ✅ |
| PYR-VERIFY-01 | Post-fire verification | check_post_fire_verify() + beep codes 2-4/3-4; Board pyro (MK1C): test_mk1c_fired_channel_reads_open_after (the first tracking test on the drained bus reports the fired channel open) | ✅ |
| PYR-ARM-01 | The pump only inside a fire; a stopped loop disarms | Board pyro (MK1C): test_mk1c_pump_runs_only_inside_a_fire, test_mk1c_a_stopped_loop_disarms (U9 off within 35 ms at the 20 ms loop), test_mk1c_only_the_tracking_test_runs | ✅ |
| PYR-ARM-03 | Fire on the measured bus; a precharge timeout aborts and latches | Board pyro (MK1C): test_mk1c_fires_on_the_measured_bus, test_mk1c_a_short_during_precharge_aborts, test_mk1c_fires_a_present_channel, test_mk1c_fires_on_one_cell; the bench fire into a dummy load is owed | ❌ changed 2026-10: the pulse is delivered at the deadline, never abandoned |
| PYR-ARM-05 | No flash write during a fire | Board pyro (MK1C): test_mk1c_flash_waits_out_a_fire (`board_flash_ok()`); `main_hardware.c` shuts the window on it, by inspection | ✅ |
| PYR-ARM-06 | A misfire latches nothing; the other channel fires | Board pyro (MK1C): test_mk1c_misfire_leaves_the_other_channel, test_mk1c_both_channels_one_after_the_other | ✅ |

## 8. Web Interface & Network

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| WEB-NET-01..04 | USB network, DHCP, mDNS, DNS-SD | Hardware: support/test_network.py | ⚠️ HW |
| WEB-API-01 | GET /api/status | Web UI: status tests (3 modes) | ✅ |
| WEB-API-02 | GET /api/config | Web UI: config loads from device | ✅ |
| WEB-API-03 | POST /api/config | Web UI: save test | ✅ |
| WEB-API-04 | POST /api/ota | Hardware: OTA to MK1A/B/C, every littlefs file preserved | ✅ HW |
| WEB-API-05 | POST /api/reboot | Hardware: 200 with CORS, board back in PAD_IDLE | ✅ HW |
| WEB-API-06 | GET /api/flight.csv, rendered from the binary log | Format: test_FLOG_04 (any division of the reads), test_FLOG_05 (the counted length is the rendered one); Hardware: the empty log reads as the column header (`support/api_check.py`); a flown log's download is owed on the bench | ⚠️ |
| WEB-API-07 | CORS headers | Hardware: bench smoke test, every route | ✅ HW |
| WEB-API-08 | The API live in flight; only the log touches the filesystem | Integration: test_WEB_API_08_only_the_log_touches_the_filesystem_in_flight (a whole flight asks for no other file), test_WEB_API_08_spent_marker_waits_for_the_log; the web server's 423s and the dropped transfer by inspection, bench check owed | ⚠️ |
| WEB-API-09 | Erase the flight log | Web UI: the flight log can be erased; Hardware: bench smoke test | ✅ |
| WEB-API-10 | No file served while the log is written | `serve_file()` answers 423 through `hal_fs_enter()`, by inspection; bench check owed (section 6 of docs/outstanding_tasks.md) | ⚠️ |
| WEB-HTTP-01 | A request is a byte stream | HTTP: test_HTTP_02 (split at every byte), test_HTTP_03 (byte by byte, random), test_HTTP_04, test_HTTP_07; Hardware: `support/http_stream_check.py` 16/16 on MK1A/B/C (4/16 on the old server) | ✅ |
| WEB-HTTP-02 | Content-Length and Connection: close on every response | HTTP: body_of() asserts both on every test; Hardware: http_stream_check framing checks | ✅ |
| WEB-HTTP-03 | Flow control, not refusal | HTTP: test_HTTP_12 (a 10 kB body through a 2 kB ring into a sink that refuses 50 times); Hardware: uploads round-trip byte-exact, flash_refusals 0, Lua heartbeat unbroken on MK1C | ✅ |
| WEB-HTTP-04 | Status codes for bad requests | HTTP: test_HTTP_08, 09, 10, 11; Hardware: 405 and 413 in http_stream_check | ✅ |
| WEB-API-11 | /api/status from a snapshot; its keys; valid JSON | Status: test_SJ_01 (every key, in order, formatted), test_SJ_02 (the widest fits), test_SJ_03 (a quote in the rocket's name), test_SJ_04 (refused, not truncated), test_SJ_07 (no watchdog, no stage); Hardware: `support/api_check.py` on all four bench boards | ✅ |
| WEB-NET-05 | A busy endpoint holds the frame | Net: test_TXQ_01_sent_at_once_when_the_endpoint_is_free, test_TXQ_02_a_busy_endpoint_holds_the_frame, test_TXQ_03_order_is_kept, test_TXQ_04_drain_stops_when_the_endpoint_is_busy, test_TXQ_05_full_refuses, test_TXQ_06_not_ready_releases_everything, test_TXQ_07_wraps, test_TXQ_08_flush_releases_everything; Hardware (2.1.700, bench MK1B, one G4 round): 21 of 878 frames refused where 472 of 1182 were (DD-070) | ✅ |
| WEB-NET-06 | The MAC drawn from the RNG and kept | MAC: test_WEB_NET_06_drawn_mac_is_local_unicast, test_WEB_NET_06_subnet_is_never_0_1_or_255, test_WEB_NET_06_same_seed_different_samples_differ, test_WEB_NET_06_no_repeat_across_many_boards, test_WEB_NET_06_subnet_spreads_evenly, test_WEB_NET_06_file_round_trips_drawn_and_assigned, test_WEB_NET_06_assigned_file_as_the_api_writes_it, test_WEB_NET_06_malformed_files_are_refused_whole; Status: test_SJ_01_keys_order_and_formatting_are_the_api | ✅ |
| WEB-API-13 | /api/net: the network's counters | Net: test_NET_01_keys_order_and_formatting_are_the_api, test_NET_02_the_widest_fits_its_bound, test_NET_03_too_small_renders_nothing; Hardware: `support/api_check.py` (every pool, eleven states, this request's own connection) | ✅ |
| WEB-API-12 | /api/log/space | Hardware: `support/api_check.py` (bytes free, 22-byte records, 1 and 50 or 100 rows/s) on the bench boards; Web UI: the mock's answer drives WEB-UI-06's tests | ✅ HW |
| WEB-UI-01 | Status in config units | Web UI: altitude in meters/feet tests | ✅ |
| WEB-UI-04 | Flight summary + CSV, from the log, refreshed, named | Web UI: flight data tests, a flight recorded while the page is open appears on refresh; Unit: test_REV09_flight_time_freezes_at_landing | ✅ |
| WEB-UI-05 | Firmware upload | Web UI: update tab test | ✅ |
| WEB-UI-06 | The three logging plans, and the longest flight the log holds | Web UI: *log rate: the longest flight the log holds follows the plan*, *log rate: saved with the rest of the tab*, *log rate: no estimate while the flight log is written*; *config tab shows non-default values* | ✅ |

## 9. Firmware Update

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| OTA-01..04 | OTA, inactive slot, rollback, safety | — (hardware test only) | ⚠️ HW |

## 10. Ground Test Interface

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| GND-TEST-05 | Ground test mode at power-up, after the checks; a recovery wins | Ground test: test_GND_TEST_05_powered_up_asserted_enters_ground_test, test_GND_TEST_05_not_asserted_boots_to_the_pad, test_GND_TEST_05_asserted_briefly_boots_to_the_pad, test_GND_TEST_05_a_recovery_outranks_the_pin | ✅ |
| GND-TEST-06 | The mode announced; the buzzer the procedure's alone | Ground test: test_GND_TEST_06_usb_does_not_take_the_buzzer; Sequence: test_GT_mode_is_announced; Buzzer: test_BUZ_GT_01_alert | ✅ |
| GND-TEST-07 | Countdown, pyro 1, tone and countdown, pyro 2, all-clear | Ground test: test_GND_TEST_07_the_procedure; Sequence: test_GT_both_channels, test_GT_the_countdown_is_a_second_a_count; Buzzer: test_BUZ_GT_02_countdown, test_BUZ_GT_03_tone, test_BUZ_GT_04_all_clear; bench check owed (a person, dummy loads) | ✅ |
| GND-TEST-08 | Only enabled channels; the steps omitted | Ground test: test_GND_TEST_08_only_the_enabled_channels_fire, test_GND_TEST_08_a_released_channel_is_not_enabled; Sequence: test_GT_only_channel_1, test_GT_only_channel_2, test_GT_no_channel | ✅ |
| GND-TEST-09 | Held 1 s before a release counts; a 100 ms debounce | Sequence: test_GT_a_release_before_arming_does_nothing, test_GT_a_bounce_is_not_a_release | ✅ |
| GND-TEST-10 | Closed again: the procedure stops before the next fire | Ground test: test_GND_TEST_10_reasserting_aborts; Sequence: test_GT_reasserting_aborts_the_countdown, test_GT_reasserting_in_the_tone_stops_the_second, test_GT_reasserting_in_the_second_countdown_stops_the_second | ✅ |
| GND-TEST-11 | Terminal until power-up; never flies | Ground test: test_GND_TEST_11_never_flies; Sequence: test_GT_done_is_final | ✅ |
| GND-TEST-12 | The switch: to ground, or across two pads | Pin assignment: test_PIN_GT_01_default_is_none, test_PIN_GT_02_a_switch_to_ground_on_a_user_pad, test_PIN_GT_03_a_switch_across_two_pads, test_PIN_GT_04_each_wiring_needs_its_pads, test_PIN_GT_05_pads_it_cannot_take, test_PIN_GT_06_not_a_pad_already_in_use, test_PIN_GT_07_ini_round_trip; Switch: test_GT_SW_switch_to_ground, test_GT_SW_two_pads_closed_and_open, test_GT_SW_two_pads_a_stuck_pad_is_not_closed, test_GT_SW_two_pads_drive_alternates; Web UI: the ground test switch is wired to ground or across two pads; bench check owed | ❌ changed 2026-10: the pair switch on the buzzer's pad |

## 11. Portability & Testability

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| HAL-01 | No #ifdef in flight code | CI: grep verification | ✅ |
| HAL-02..04 | HAL interface | CI: all 3 targets build from same source | ✅ |

## 12. Build & Test System

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| BLD-01..05 | Build targets | CI: build succeeds | ✅ |
| TST-01..03 | Unit/integration/closed-loop | CI: all pass | ❌ changed 2026-10: black-box flight tests against the mocked HAL |
| TST-04 | All 4 pyro modes | Closed-loop: 7 config suites | ❌ changed 2026-10: closed-loop tests of the re-fire and emergency rules |
| TST-05 | 100ft to 100km | Closed-loop: 4 profiles, 65 m to 950 m, from rockets.json (H3) | ❌ |
| TST-06 | Chute reduces descent | Closed-loop: test_TST_06_chute_effect | ✅ |
| TST-07 | Web UI tests | CI: Playwright tests | ✅ |
| TST-08 | CI on every push | GitHub Actions | ✅ |
| TST-09 | Traceable to requirements | This document | ✅ |

---

## 13. Beep storage

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| BUZ-CODE-14 | A storage failure is not a validation failure | Buzzer: test_BEEP_STORE_01/02 | ✅ |

## 14. Configuration fields

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| CFG-TABLE-02 | Round-trip serialize → parse | Config: test_config_roundtrip_defaults, test_config_roundtrip_custom | ✅ |
| CFG-SUBSYS-01 | Each subsystem has configurable params, every key read | Config: test_config_writes_no_inert_keys; Unit: telem_rate_hz; Config: test_config_parse_new_fields (log_rate's three names; an unknown one, and an old log_rate_hz, log at the default); Integration: test_FLT_LOG_07_the_three_logging_plans | ✅ |

## 18. SD Card and High-Rate Log

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SD-01 | SPI mode, CRC, bounded waits, the bus shared | Hardware only. On MK1C-SD with the card on its own regulator (C-U6): 16 MB written with 0 CRC errors, 0 timeouts, 0 retries at 12.5 and 20.8 MHz; the LSM6DS3 read at 1.63 kHz with no FIFO overrun while the card wrote; a card that resets inside ACMD41 brought up again by `POST /api/sd/init` | ⚠️ |
| SD-02 | Every file on the card, configuration mirrored | Hardware only. On MK1C-SD the flight log routes to the card (`/api/log/space` store `sd`), config.ini and the web pages are served through `vfs.h`, and serial.txt stays in littlefs; the mirror into littlefs by inspection of `vfs.c` | ⚠️ |
| HR-01 | The next file ready, the second before launch kept | HR log: test_HR_01_between_flights_the_file_is_ready_and_the_ring_keeps_half, test_HR_02_a_flight_is_logged_whole_with_the_second_before_it | ✅ |
| HR-02 | Every set, every conversion, the state at 10 Hz | HR log: test_HR_02_a_flight_is_logged_whole_with_the_second_before_it; `support/hr_log.py --selftest` | ✅ |
| HR-04 | A CRC per record, a power cut's log kept | HR log: test_HR_04_a_log_a_power_cut_left_is_kept_under_a_number; `support/hr_log.py --selftest` (a torn last record) | ✅ |
| HR-05 | Whole records dropped and counted, off the flight path | HR log: test_HR_05_a_full_ring_drops_whole_records_and_counts_them; `support/prove_core0.py` on MK1C-SD in CI | ✅ |
| HR-06 | A file the card cannot take any more: go on in a new one | HR log: test_HR_06_a_card_mounted_again_under_a_log_goes_on_in_a_new_file, test_HR_06_a_card_that_refuses_writes_for_a_while_loses_one_record; Hardware: MK1C-SD mounted again 40 s into a 1.66 kHz log, the new file's first set 0.9 ms after the old one's last | ✅ |

## 19. Bench Flight

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-14 | See it fly before it flies | Through SYS-SIM-01 | ✅ |
| SYS-SIM-01 | A scripted flight on the bench, firing nothing | Through SIM-01..03; Hardware: MK1C-SD flew 1 km, 10 km and 30 km profiles to LANDED with both channels mocked (`support/bench_flight.py`); the 30 km flights found HA-1 | ✅ |
| SIM-01 | Only from the pad, in test mode | Flight source: test_SIM_01_a_bench_flight_starts_only_from_the_pad_in_test_mode, test_SIM_01_profiles_that_cannot_fly_are_refused | ✅ |
| SIM-02 | The profile's pressure in the reading's place | Closed-loop: test_SIM_02_a_9_km_supersonic_flight_with_sensor_noise, and test_SIM_02_30_km_with_sensor_noise_finds_apogee, ignored until HA-1; Flight source: test_SIM_02_isa_pressure_at_the_layer_bases, test_SIM_02_isa_pressure_inside_the_layers, test_SIM_02_isa_density_at_sea_level_and_30_km, test_SIM_02_isa_altitude_inverts_pressure, test_SIM_02_the_coast_peaks_at_the_apogee_asked_for, test_SIM_02_phases_run_in_order_and_it_lands, test_SIM_02_descent_times_at_constant_rates, test_SIM_02_thin_air_speeds_the_drogue, test_SIM_02_a_high_pad_adds_its_own_altitude, test_SIM_02_the_bench_replaces_the_reading_while_it_flies, test_SIM_02_the_bench_ends_when_both_have_landed; Closed-loop: test_SIM_02_the_flight_software_flies_a_30_km_profile | ✅ |
| SIM-03 | Every fire mocked until reboot | Flight source: test_SIM_03_the_channels_stay_mocked_after_a_stop; by inspection, `hal_pyro_fire()` is every fire's one path | ✅ |

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
| UN-11 | Long pad time on battery | Through SYS-PWR-01 | ⚠️ |
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
| SYS-PWR-01 | Minimise CPU active time | Through PWR-SLEEP-01; no test measures CPU active time | ❌ changed 2026-10: restated; no test measures power |
| SYS-TEST-01 | Ground test over serial | Integration: test_GND_TEST_01..04 | ❌ changed 2026-10: the serial commands are removed |

---

## 20. Added by the 2026-10 requirements review

Rows for the requirements the review added (`docs/requirements_review_2026-10-02.md`).
The tests and the code are reviewed against them next; until then a new
behaviour is ❌ and a behaviour the firmware is believed to have already is ⚠️.

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-DEPLOY-04 | A fire only on measured evidence; data believed | Through FLT-APO-01, FLT-MACH-04, SNS-EST-02, SNS-PRES-10, SNS-PRES-11 | ❌ estimator DD-085 |
| SYS-DEPLOY-05 | Nothing withholds a decided fire; attempts go on | Through PYR-FIRE-01, PYR-REFIRE-01, FLT-EMRG-01, PYR-HEALTH-01 | ❌ fire rules DD-082 |
| SYS-DEPLOY-06 | Resume after any restart | Through FLT-BROWN-01..07 | ❌ DD-086 |
| FLT-RT-01 | Other activity delays no decision beyond the board's bound | — (the design is DD-073, DD-074; no test measures the bound) | ⚠️ |
| FLT-EMRG-05 | The fire rules act on the filtered state | — | ❌ DD-082 |
| FLT-BROWN-07 | A resume is logged as an event | — | ❌ DD-086 |
| PYR-HEALTH-01 | Undetectable is ready; no fault prevents a fire | — | ❌ DD-083 |
| PYR-HEALTH-02 | Only enabled channels count; the pin assignment rules | — (enabled-channel rule exists for ground test and continuity faults) | ⚠️ |
| DAT-09 | A log of the longest flight, capacity declared | — (`/api/log/space` reports room; no board declares a duration) | ⚠️ |
| DAT-10 | Raw pressure records carry temperature | — (the flight log, high-rate log and trace carry it) | ⚠️ |
| FLT-LOG-08 | Records the log could not keep are counted | — | ⚠️ |
| CFG-10 | Changes take effect at start-up | — | ⚠️ |
| PYR-BOARD-01 | A board declares pyro defaults and ranges | — | ❌ DD-084 |
| PYR-BOARD-02 | Out of range is brought in and reported | — | ❌ DD-084 |
| PYR-BOARD-03 | Ranges served to the web interface | — | ❌ DD-084 |
| PYR-BOARD-04 | A constraint never withholds a fire | — | ❌ DD-084 |
| SNS-REC-01 | No sensor recovery in flight | — | ⚠️ |
| SNS-EST-01 | Decisions on one filtered state from raw readings | — | ❌ DD-085 |
| SNS-EST-02 | One or two bad readings change no decision | — (host evaluation: `docs/kalman_launch_evaluation.md`) | ❌ DD-085 |
| SNS-EST-03 | Noise crosses no threshold | — | ❌ DD-085 |
| SNS-EST-04 | An overspeed is reported within its bound | — | ❌ DD-085 |
| SNS-EST-05 | Every comparison in pressure; converted once | — | ❌ DD-085 |
| SNS-MAX-01 | Declared range and height for proper operation; best effort above | — | ❌ DD-085 |
| TEL-11 | Events queued into the next message | — | ❌ DD-088 |
| TEL-12 | The telemetry port accepts no commands | — | ❌ DD-087, DD-088 |
| OTA-05 | Another board's firmware is not kept | — (hardware only) | ⚠️ HW |
| UN-15 | Auxiliary functions from a script | Through SYS-LUA-01, SYS-LUA-02 | ⚠️ |
| SYS-LUA-01 | Every board runs a script with its pads and the flight's state | Through LUA-PAD-01..03, LUA-RUN-01 | ⚠️ |
| SYS-LUA-02 | No script can block the board | Through LUA-ISO-01..04, LUA-SAFE-01 | ⚠️ |
| LUA-ISO-01 | A bad script costs only itself | — (lua_tests cover the sandbox; to be traced) | ⚠️ |
| LUA-ISO-02 | The flight's state is read-only | — (to be traced) | ⚠️ |
| LUA-ISO-03 | No means to fire or stop an enabled channel | — (to be traced) | ⚠️ |
| LUA-ISO-04 | No file access | — (to be traced) | ⚠️ |
| LUA-SAFE-01 | The board starts whatever the script; a good script is not left disabled | — (a restart within 15 s of start leaves the script disabled) | ❌ DD-089 |
| LUA-PAD-01 | One owner per pad | — (pin_assign_tests; to be traced) | ⚠️ |
| LUA-PAD-02 | Resources by name and kind; full control of its pads | — (lua_tests; to be traced) | ⚠️ |
| LUA-PAD-03 | A board declares its script resources | — (pin_caps_tests; to be traced) | ⚠️ |
| LUA-RUN-01 | The enabled script always runs | — | ⚠️ |
| LUA-MGT-01 | Edit, check, save and remove from the web interface | — (Playwright; to be traced) | ⚠️ |
| LUA-MGT-02 | A console with output, errors and state | — (to be traced) | ⚠️ |
| GND-TEST-13 | A ground test fire is delivered on command | — | ❌ DD-087 |
| SIM-04 | Bench profiles with a failed canopy | — | ❌ DD-082 |
| HAL-05 | The HAL is the test seam | — (every host flight suite links `test/hal_test.c`) | ⚠️ |
| HAL-06 | The HAL is validated on hardware | — (`HARDWARE_CI_PLAN.md`) | ⚠️ HW |
| BLD-06 | HAL validation applications | — | ❌ DD-090 |
| CODE-01..10 | The code requirements | — (review; to be checked across the tree) | ❌ DD-090 |
| BRD-01 | What a board declares | — (each `THEORY_OF_OPERATION.md`) | ❌ DD-084 |
| BRD-02 | Declared values confirmed by HAL validation | — | ⚠️ HW |

---

## Summary

| Status | Count |
|--------|-------|
| ✅ Verified by a host, web or closed-loop test | 174 |
| ⚠️ Not directly verified (needs a test or hardware) | 43 |
| ❌ Not implemented | 57 (changed or added by the 2026-10 review; TST-05) |
| ✅ HW (hardware satisfies) | 9 |

Rows of the tables above. `support/trace_check.py --counts` computes them, and CI fails when this table disagrees.

### Remaining gaps
- **Every ❌ row marked "changed 2026-10" or in section 20**: the requirement was changed or added by the review of 2026-10-02 and the code and tests have not been brought to it yet.
- **TST-05**: closed-loop tests do not yet reach above each sensor's height for proper operation.
- **FLT-RATE-01, PYR-CONT-01**: sample rate and check period — hardware timing test
- **BUZ-01, BUZ-02**: the announcement and its cadence — hardware audio test
- **SYS-DATA-02 / DAT-06..07**: CSV export format — hardware integration test
- **SYS-ALT-02 / SNS-PRES-01**: multi-sensor detection — hardware test
- **WEB-NET-01..04**: USB network / mDNS / DNS-SD — hardware test
- **WEB-API-08**: the web server's 423s and the dropped transfer are verified by inspection; the bench check is a chamber flight in test mode
- **OTA-01..05**: update flow — hardware test
- **SD-01, SD-02 / SYS-DATA-04**: the card, verified on the bench only
