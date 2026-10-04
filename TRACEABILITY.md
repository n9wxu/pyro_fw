# Requirements Traceability Matrix

Each live requirement of `REQUIREMENTS.md` and what verifies it. Withdrawn
requirements (its Appendix A) have no row.

This file is written by `support/trace_matrix.py` from the tests' own
citations, the rules of `support/structure_check.py`, and the notes in
`support/trace_notes.tsv`. Edit those, not this.

- **✅** a host test, a web test or a structural check verifies it.
- **✅ HW** verified on hardware only: a bench script or a measurement.
- **⚠️** not verified, or not wholly: the row says what is owed.

## 1. Recovery Deployment

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-1 | The user needs recovery devices deployed at the correct flight events to safely recover the rocket | Through SYS-DEPLOY-01, SYS-DEPLOY-02, SYS-DEPLOY-03, SYS-DEPLOY-04, SYS-DEPLOY-05, SYS-DEPLOY-06 | ✅ |
| SYS-DEPLOY-01 | The system shall fire pyrotechnic charges at configurable flight events | the suite test_recorded_flight.c | ✅ |
| SYS-DEPLOY-02 | The system shall support two independent pyrotechnic channels | the suite test_recorded_flight.c | ✅ |
| SYS-DEPLOY-03 | The system shall prevent pyrotechnic firing before the rocket has left the launch rail | test_flight_profiles.c: test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event, test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers | ✅ |
| SYS-DEPLOY-04 | A fire shall be decided only on measured evidence that its flight event has happened | test_fire_control.c: test_PYR_MODE_04_speed_fires_at_the_descent_speed, test_SYS_DEPLOY_04_without_data_only_a_delay_fires | ✅ |
| SYS-DEPLOY-05 | Once a fire is decided nothing shall withhold, abort or indefinitely delay the attempt, and the system... | test_fire_rules.c: test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen | ✅ |
| SYS-DEPLOY-06 | The system shall resume a flight in progress after any restart | Through FLT-BROWN-01 | ✅ |
| FLT-PHASE-01 | The system shall detect the transition from ground to powered flight | test_recorded_flight.c: test_TST_02_the_recorded_flight_runs_pad_to_landed | ✅ |
| FLT-PHASE-02 | The system shall detect apogee (peak altitude) | the suite test_recorded_flight.c | ✅ |
| FLT-PHASE-03 | The system shall detect landing | test_recorded_flight.c: test_TST_02_the_recorded_flight_runs_pad_to_landed | ✅ |
| FLT-PHASE-04 | A flight state that is none of the defined states shall be FAULT, announced as a general fault | test_flight_pad.c: test_FLT_PHASE_04_a_state_that_is_no_state_is_a_fault | ✅ |
| FLT-RT-01 | No network, script or storage activity shall delay a flight decision or a pulse by more than the... | test_flight_profiles.c: test_FLT_RATE_05_a_late_loop_changes_no_decision, test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound; the suite test_http_work.c; The bound on each board is declared and not yet measured on this revision: `loop_late_max_us` on `/api/status` | ✅ |
| FLT-LAUNCH-02 | The system shall remain in PAD_IDLE while the rocket is at or below 100 feet above the ground reference | test_flight_pad.c: test_CFG_10_a_saved_change_waits_for_the_next_start, test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch, test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch; test_launch_detector.c: test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch; test_recorded_flight.c: test_TST_02_launch_is_detected_as_the_rocket_leaves | ✅ |
| FLT-LAUNCH-03 | T+0 shall be the time of the first sample of the rise more than 50 cm above the pad, not the moment of... | test_flight_pad.c: test_CFG_10_a_saved_change_waits_for_the_next_start, test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch; test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order, test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise; test_telemetry.c: test_TEL_11_each_event_is_carried_once_by_the_next_message | ✅ |
| FLT-LAUNCH-04 | The system shall log a LAUNCH event at the transition | test_flight_pad.c: test_CFG_10_a_saved_change_waits_for_the_next_start, test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch; test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order | ✅ |
| FLT-LAUNCH-05 | The system shall stop the buzzer upon launch detection | test_buzzer.c: test_BUZ_ACT_01_lifecycle, test_BUZ_ACT_02_stop; test_flight_pad.c: test_CFG_10_a_saved_change_waits_for_the_next_start, test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch | ✅ |
| FLT-LAUNCH-07 | The system shall declare launch, and enter ASCENT, only when the filtered state shows a height more than... | test_flight_pad.c: test_CFG_10_a_saved_change_waits_for_the_next_start, test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch, test_FLT_LAUNCH_07_a_slow_rise_is_not_a_launch; test_launch_detector.c: test_FLT_LAUNCH_07_high_and_climbing_is_a_launch, test_FLT_LAUNCH_07_slower_than_5_m_s_is_not_a_launch | ✅ |
| GND-CAL-01 | The ground reference shall follow the ambient pressure on the pad, as its mean over the last 5 seconds | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_01_the_reference_follows_the_weather | ✅ |
| GND-CAL-03 | A sample more than 50 Pa from the reference shall not move it, so a climbing rocket cannot drag it | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_01_the_reference_follows_the_weather, test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure | ✅ |
| GND-CAL-04 | The reference shall freeze at launch to the pad's pressure from before T+0, not to the pressure at detection | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_01_the_reference_follows_the_weather, test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure | ✅ |
| GND-CAL-05 | Altitude at launch detection shall report the height actually reached, not zero | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_01_the_reference_follows_the_weather, test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure; test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order, test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise | ✅ |
| GND-CAL-06 | When every sample has been more than 50 Pa from the reference for 5 s while the board is still (under 1... | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure, test_GND_CAL_06_a_moved_board_takes_a_new_reference, test_GND_CAL_06_a_launch_never_moves_the_reference | ✅ |
| GND-CAL-07 | A reference frozen on less than a second of the pad shall be reported as degraded on /api/status | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_GND_CAL_06_a_launch_never_moves_the_reference, test_GND_CAL_07_a_reference_from_under_a_second_is_reported | ✅ |
| FLT-ASC-01 | The system shall track the peak of the flight during ascent | test_recorded_flight.c: test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon | ✅ |
| FLT-ASC-02 | Every detector shall take its vertical speed from the filtered state (SNS-EST-01) | the suite test_fire_control.c | ✅ |
| FLT-ASC-03 | The system shall report the thrust phase while the filtered acceleration is upward | test_flight_profiles.c: test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers, test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn, test_FLT_ASC_03_the_thrust_report_ends_within_a_second_of_burnout | ✅ |
| FLT-ASC-04 | The system shall arm pyrotechnics when vertical speed drops below 10 m/s | test_flight_profiles.c: test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers, test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn | ✅ |
| FLT-ASC-05 | The system shall log an ARMED event when pyrotechnics are armed | test_flight_profiles.c: test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers, test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn | ✅ |
| FLT-ASC-06 | The system shall not arm pyrotechnics while vertical speed exceeds 10 m/s | test_flight_profiles.c: test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers, test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn | ✅ |
| FLT-ASC-07 | The system shall not arm pyrotechnics unless a vertical speed above 10 m/s was measured during ASCENT | test_flight_profiles.c: test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers, test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn | ✅ |
| FLT-APO-01 | The system shall declare apogee from the filtered pressure passing its minimum | test_flight_profiles.c: test_FLT_ASC_03_the_thrust_report_ends_within_a_second_of_burnout, test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height; test_recorded_flight.c: test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon | ✅ |
| FLT-APO-02 | The system shall leave ASCENT for descent upon apogee detection | test_recorded_flight.c: test_TST_02_the_recorded_flight_runs_pad_to_landed | ✅ |
| FLT-APO-03 | The system shall log an APOGEE event at the transition | the suite test_recorded_flight.c | ✅ |
| FLT-APO-04 | The system shall not detect apogee before pyros are armed | test_flight_profiles.c: test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event, test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers | ✅ |
| FLT-APO-07 | Apogee shall be declared when the obeyed estimator has been seen climbing and is then seen falling, each... | test_apogee_detector.c: test_FLT_APO_07_over_the_top_is_apogee_at_once, test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last_2_s, test_FLT_APO_07_readings_not_explained_forget_the_climb, test_FLT_APO_07_a_suspect_reading_decides_nothing_and_forgets_nothing and 1 more; test_mach.c: test_SNS_EST_03_a_swinging_canopy_moves_neither_the_main_nor_the_landing, test_FLT_APO_07_a_subsonic_flight_deploys_at_apogee, test_FLT_ASC_08_nothing_arms_below_30_m, test_FLT_APO_07_no_fire_before_apogee_when_supersonic and 6 more | ✅ |
| FLT-APO-08 | The reported peak shall be the height of the lowest filtered pressure the estimator explained, marked a... | test_mach.c: test_FLT_APO_07_at_10_km_on_every_seed, test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last, test_FLT_APO_08_the_peak_is_from_what_the_estimator_explained, test_FLT_APO_08_the_peak_is_logged; Web UI: test_ui.spec.js | ✅ |
| FLT-ASC-08 | No channel shall arm before the filtered pressure has been below 0.9965·p0 (about 30 m) | test_mach.c: test_FLT_APO_07_a_subsonic_flight_deploys_at_apogee, test_FLT_ASC_08_nothing_arms_below_30_m | ✅ |
| FLT-DESC-01 | The system shall determine the descent phase from the measured descent speed holding steady, not from... | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent, test_FLT_DESC_02_a_ballistic_flight_reaches_landed, test_FLT_DESC_01_the_phase_follows_the_rate_not_the_channel | ✅ |
| FLT-DESC-02 | The system shall detect landing in every descent phase, so that a flight which deployed nothing still... | test_flight_profiles.c: test_FLT_DESC_02_a_ballistic_flight_reaches_landed | ✅ |
| FLT-AIR-01 | The descent phases and the fire rules (PYR-REFIRE-01, FLT-EMRG-01) shall judge a descent speed as the... | test_atmosphere.c: test_FLT_AIR_01_temperature_at_each_pressure, test_FLT_AIR_01_scale_height_follows_the_temperature, test_FLT_AIR_01_pad_air_ratio; test_fire_control.c: test_PYR_REFIRE_01_each_channel_has_its_own_speed, test_FLT_AIR_01_a_good_canopy_in_thin_air_is_not_refired; test_fire_rules.c: test_FLT_EMRG_01_zero_leaves_each_channel_to_its_trigger, test_FLT_AIR_01_a_good_drogue_at_20_km_is_not_a_failed_one, test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen | ✅ |
| FLT-LAND-02 | The system shall require vertical speed below 2 m/s, for 1 s of sample time, for landing detection | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent, test_FLT_DESC_01_the_phase_follows_the_rate_not_the_channel, test_FLT_LAND_02_a_slow_descent_is_not_a_landing; test_recorded_flight.c: test_TST_02_landing_follows_the_ground_hit | ✅ |
| FLT-LAND-03 | The system shall require a height below 30 meters above the ground reference for landing detection, except... | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent | ✅ |
| FLT-LAND-04 | The system shall enter LANDED upon landing detection | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent | ✅ |
| FLT-LAND-05 | The system shall log a LANDING event at the transition | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent | ✅ |
| FLT-LAND-06 | The system shall remain in LANDED until the next start | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent | ✅ |
| FLT-LAND-07 | The system shall detect landing at any height once descent has lasted the configured landing_timeout... | test_flight_profiles.c: test_SNS_ALT_01_below_the_pad_is_a_negative_altitude, test_TST_06_a_canopy_slows_the_descent | ✅ |
| PYR-MODE-01 | The system shall support a DELAY mode that fires N seconds after apogee | test_fire_control.c: test_PYR_MODE_01_delay_counts_from_apogee; test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee | ✅ |
| PYR-MODE-02 | The system shall support an AGL mode that fires when the rocket descends below a set height above the... | test_fire_control.c: test_PYR_MODE_02_agl_fires_on_descending_through_the_height; test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee, test_PYR_MODE_02_agl_at_its_height, test_PYR_MODE_02_agl_on_both_channels | ✅ |
| PYR-MODE-03 | The system shall support a FALLEN mode that fires when the rocket has descended a set distance from its peak | test_fire_control.c: test_PYR_MODE_03_fallen_measures_from_the_peak; test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee, test_PYR_MODE_03_fallen_from_the_peak | ✅ |
| PYR-MODE-04 | The system shall support a SPEED mode that fires when descent speed exceeds a set value | test_fire_control.c: test_PYR_MODE_04_speed_fires_at_the_descent_speed; test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee, test_PYR_MODE_04_speed_of_descent | ✅ |
| PYR-MODE-05 | AGL, FALLEN and SPEED shall compare the filtered state, and FALLEN shall measure from the peak (FLT-APO-08) | test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee | ✅ |
| PYR-MODE-06 | A charge pressurising the bay, a rise of up to 5 kPa lasting up to 0.5 s ‡, shall not bring a pressure... | test_fire_control.c: test_PYR_DEPLOY_02_two_refiring_channels_take_turns, test_PYR_MODE_06_a_charge_does_not_bring_a_trigger_forward; test_mach.c: test_PYR_MODE_06_a_charge_in_the_bay_does_not_move_the_main | ✅ |
| PYR-SAFE-03 | No channel shall fire before its own trigger or the emergency fire (FLT-EMRG-01) | test_fire_control.c: test_PYR_SAFE_03_a_channel_fires_once_when_no_rule_asks_again | ✅ |
| PYR-SAFE-04 | The system shall not fire any pyro before apogee is declared | test_fire_control.c: test_PYR_SAFE_04_nothing_fires_before_apogee; test_flight_profiles.c: test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event, test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers | ✅ |
| PYR-DEPLOY-01 | The system shall allow both channels to deploy on a single flight event, so that a low flight can put out... | test_fire_control.c: test_PYR_DEPLOY_02_never_together_and_the_gap_is_quiet_time; test_flight_profiles.c: test_CFG_03_a_height_in_feet, test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event | ✅ |
| PYR-DEPLOY-02 | The two channels shall never be energised together, in flight or in ground test | test_board_pyro_mk1a.c: test_PYR_FIRE_01_mk1a_pulse_lasts_when_the_fire_clock_leads_the_loop, test_PYR_DEPLOY_02_mk1a_refuses_a_fire_mid_pulse, test_PYR_DEPLOY_02_mk1a_refuses_a_channel_out_of_range; test_board_pyro_mk1b.c: test_PYR_FIRE_01_mk1b_pulse_lasts_when_the_fire_clock_leads_the_loop, test_PYR_DEPLOY_02_mk1b_refuses_a_fire_mid_pulse, test_PYR_DEPLOY_02_mk1b_refuses_a_channel_out_of_range; test_fire_control.c: test_PYR_HEALTH_02_a_channel_that_is_not_enabled_never_fires, test_PYR_DEPLOY_02_never_together_and_the_gap_is_quiet_time, test_PYR_DEPLOY_02_a_first_fire_goes_before_a_refire, test_PYR_DEPLOY_02_two_refiring_channels_take_turns; test_fire_rules.c: test_FLT_EMRG_01_no_canopy_fires_everything_and_keeps_firing; test_flight_profiles.c: test_PYR_MODE_02_agl_on_both_channels, test_CFG_03_a_height_in_feet, test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event; test_resume.c: test_FLT_BROWN_06_a_descent_resumed_fires_every_channel_afresh, test_FLT_BROWN_06_a_resume_below_the_main_height_fires_both; the suite test_board_pyro_mk1c.c; Web UI: test_ui.spec.js | ✅ |
| PYR-FIRE-01 | Every pulse shall be recorded, in the flight log and on /api/status, with what the board observed of it | test_board_pyro_mk1a.c: test_mk1a_igniters_read_good, test_PYR_FIRE_01_mk1a_pulse_lasts_when_the_fire_clock_leads_the_loop; test_board_pyro_mk1b.c: test_PYR_CONT_01_mk1b_a_reading_at_least_once_a_second, test_PYR_FIRE_01_mk1b_pulse_lasts_when_the_fire_clock_leads_the_loop; test_board_pyro_mk1c.c: test_mk1c_a_failed_pulse_does_not_prevent_the_next; test_fire_rules.c: test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen, test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused; the suite test_status_json.c; Web UI: test_ui.spec.js; Structure: `support/structure_check.py` | ✅ |
| PYR-REFIRE-01 | After a channel's first fire, while the descent speed exceeds that channel's re-fire speed... | test_config.c: test_config_defaults; test_fire_control.c: test_PYR_SAFE_03_a_channel_fires_once_when_no_rule_asks_again, test_PYR_REFIRE_01_refires_every_interval_while_too_fast, test_PYR_REFIRE_01_stops_when_the_descent_slows, test_PYR_REFIRE_01_zero_disables_it and 1 more; test_fire_rules.c: test_PYR_REFIRE_01_a_working_flight_gets_no_lasting_refire, test_PYR_REFIRE_01_a_charge_that_lights_on_the_third_pulse; Web UI: test_ui.spec.js | ✅ |
| FLT-EMRG-01 | At any time after apogee, when the descent speed reaches emergency_fire_speed, every enabled channel shall... | test_config.c: test_config_defaults; test_fire_control.c: test_FLT_AIR_01_a_good_canopy_in_thin_air_is_not_refired, test_FLT_EMRG_01_every_enabled_channel_fires_whatever_its_trigger, test_FLT_EMRG_01_fires_again_until_the_speed_drops, test_FLT_EMRG_01_zero_disables_it; test_fire_rules.c: test_SNS_EST_04_a_lost_canopy_is_seen_within_two_seconds, test_FLT_EMRG_01_no_canopy_fires_everything_and_keeps_firing, test_FLT_EMRG_01_a_canopy_that_opens_ends_it, test_FLT_EMRG_01_zero_leaves_each_channel_to_its_trigger; Web UI: test_ui.spec.js | ✅ |
| FLT-EMRG-04 | An emergency fire shall be recorded as one | test_fire_rules.c: test_SNS_EST_04_a_lost_canopy_is_seen_within_two_seconds, test_FLT_EMRG_01_no_canopy_fires_everything_and_keeps_firing; the suite test_status_json.c; Web UI: test_ui.spec.js | ✅ |
| FLT-EMRG-05 | The re-fire and emergency rules shall act on the filtered state, which is the best knowledge the system... | the suite test_fire_control.c | ✅ |
| FLT-BROWN-01 | From 10 seconds of PAD_IDLE with no USB host attached (USB-01, USB-04), the system shall hold a record of... | test_flight_pad.c: test_GND_CAL_07_a_reference_from_under_a_second_is_reported, test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds; test_resume.c: test_FLT_BROWN_01_a_damaged_record_is_no_record, test_FLT_BROWN_01_the_record_lasts_the_whole_flight; Hardware: `support/api_check.py` | ✅ |
| FLT-BROWN-02 | After any restart, whatever its cause, the system shall resume the flight if and only if that record... | test_resume.c: test_FLT_BROWN_02_above_the_ground_and_moving_is_a_flight, test_FLT_BROWN_01_a_damaged_record_is_no_record, test_FLT_BROWN_03_a_still_board_at_altitude_goes_to_the_pad, test_FLT_BROWN_02_two_bad_readings_cannot_decide_it and 1 more | ✅ |
| FLT-BROWN-03 | The system shall not treat a stationary board as airborne, whatever its apparent altitude | test_resume.c: test_FLT_BROWN_02_above_the_ground_and_moving_is_a_flight, test_FLT_BROWN_03_a_still_board_is_not_airborne, test_FLT_BROWN_05_no_sample_in_time_is_reported, test_FLT_BROWN_03_a_still_board_at_altitude_goes_to_the_pad | ✅ |
| FLT-BROWN-04 | All resume state shall be cleared when the flight lands and when a bench flight ends, so that no later... | test_resume.c: test_FLT_BROWN_06_the_emergency_fire_applies_from_the_resume, test_FLT_BROWN_04_a_landing_clears_the_resume_state; Hardware: `support/bench_flight.py` | ✅ |
| FLT-BROWN-05 | /api/status shall say whether a start resumed a flight and, if not, why | test_resume.c: test_FLT_BROWN_05_no_record_is_reported, test_FLT_BROWN_05_at_ground_level_is_reported, test_FLT_BROWN_05_on_usb_is_reported, test_FLT_BROWN_05_no_sample_in_time_is_reported; the suite test_status_json.c; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| FLT-BROWN-06 | A resumed flight shall measure altitude against the recorded ground, shall assume no channel has fired,... | test_board_pyro_mk1c.c: test_mk1c_fires_a_channel_that_reads_open, test_mk1c_fires_before_a_presence_test; test_resume.c: test_FLT_BROWN_02_every_cause_of_restart_resumes_the_flight, test_FLT_BROWN_06_a_descent_resumed_fires_every_channel_afresh, test_FLT_BROWN_06_a_resume_below_the_main_height_fires_both, test_FLT_BROWN_06_a_delay_counts_in_full_from_the_resume and 2 more | ✅ |
| FLT-BROWN-07 | A resume shall be recorded as an event in the flight log | test_resume.c: test_FLT_BROWN_02_every_cause_of_restart_resumes_the_flight | ✅ |
| FLT-BROWN-08 | The record shall be cleared when the board sits in PAD_IDLE with a USB host attached and test mode off, so... | test_resume.c: test_FLT_BROWN_08_a_record_left_by_a_scrubbed_flight_is_cleared_on_the_bench, test_FLT_BROWN_08_a_bench_with_no_record_writes_nothing | ✅ |

## 2. Pre-Flight Status

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-2 | The user needs to verify the system is ready before placing the rocket on the pad | Through SYS-STATUS-01, SYS-STATUS-02 | ✅ |
| SYS-STATUS-01 | The system shall indicate readiness and faults audibly without requiring a display | Through BUZ-01, BUZ-02, FLT-BOOT-01, SNS-PRES-17 | ✅ |
| SYS-STATUS-02 | The system shall verify pyrotechnic circuit integrity before flight | Through BUZ-CODE-01, PYR-CONT-01, PYR-CONT-02, PYR-HEALTH-01, PYR-HEALTH-02, FLT-BOOT-07, FLT-BOOT-15 | ✅ |
| BUZ-01 | The system shall announce one of four outcomes | test_buzzer.c: test_BUZ_GT_04_all_clear, test_BUZ_ACT_04_new_outcome_silences_the_old_one; test_flight_pad.c: test_BUZ_01_a_clean_board_says_ok_to_fly, test_BUZ_01_a_pyro_fault_names_its_channel; the suite test_beep_codes.c | ✅ |
| BUZ-02 | The announcement shall repeat on a configurable cadence, defaulting to every 5 s until launch, so that... | test_beep_codes.c: test_an_active_slot_that_does_not_exist_is_refused, test_a_silent_ok_to_fly_is_refused; test_buzzer.c: test_BUZ_PAT_03_altitude_165_digits, test_BUZ_ACT_03_repeat_restarts, test_BUZ_PAT_04_task_armed_on_play, test_BUZ_PAT_06_repeat_count_2_buz02 and 1 more | ✅ |
| BUZ-CODE-01 | The beep vocabulary shall be the set of actions available at the pad | test_beep_codes.c: test_a_silent_ok_to_fly_is_refused, test_the_vocabulary_is_four_outcomes_in_priority_order; Web UI: test_ui.spec.js | ✅ |
| BUZ-CODE-02 | One outcome shall be announced at a time, by priority | test_beep_codes.c: test_a_silent_ok_to_fly_is_refused, test_the_vocabulary_is_four_outcomes_in_priority_order; test_flight_pad.c: test_BUZ_01_a_clean_board_says_ok_to_fly, test_BUZ_CODE_02_pyro_1_is_announced_before_pyro_2, test_BUZ_CODE_02_a_general_fault_outranks_a_pyro_fault | ✅ |
| BUZ-CODE-03 | The diagnosis shall be reported by name on /api/status, not encoded in the announcement | the suite test_beep_codes.c, test_status_json.c; Hardware: `support/api_check.py` | ✅ |
| BUZ-CODE-04 | Each outcome shall carry a stable key, a human-readable meaning, and a configurable sound | test_beep_codes.c: test_the_vocabulary_is_four_outcomes_in_priority_order, test_a_table_stored_before_the_rename_is_still_read | ✅ |
| BUZ-CODE-05 | A sound shall be a chirp, a steady tone, a beep count, or silence | test_buzzer.c: test_BUZ_ACT_02_stop, test_BUZ_PAT_01_chirp_is_one_warble, test_BUZ_PAT_02_counted_codes; the suite test_beep_codes.c | ✅ |
| BUZ-CODE-06 | A beep count shall be 1 to 9 per group; a zero cannot be heard and a long count cannot be counted | the suite test_beep_codes.c | ✅ |
| BUZ-CODE-07 | No two audible outcomes within a personality shall sound alike | the suite test_beep_codes.c | ✅ |
| BUZ-CODE-08 | A personality that is wholly silent shall be refused, and so shall one whose OK to fly is silent | the suite test_beep_codes.c | ✅ |
| BUZ-CODE-09 | The system shall hold three named personalities, one active | the suite test_beep_codes.c | ✅ |
| BUZ-CODE-10 | A beep table that fails validation shall be rejected whole and the shipped personalities used | the suite test_beep_codes.c | ✅ |
| BUZ-CODE-11 | The outcomes, their meanings and the personalities shall be served to the web interface so the firmware is... | Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| BUZ-CODE-12 | A board with no stored beep table shall store the shipped personalities | test_buzzer.c: test_BEEP_STORE_02_missing_file_reason_reaches_the_caller, test_BUZ_CODE_12_missing_table_is_written | ✅ |
| BUZ-CODE-13 | The shipped defaults shall follow the Eggtimer Rocketry convention | test_beep_codes.c: test_shipped_pyro_codes_follow_eggtimer | ✅ |
| BUZ-CODE-14 | A valid beep table that cannot be stored shall be reported as a storage failure, not as a validation failure | test_buzzer.c: test_BUZ_ACT_04_new_outcome_silences_the_old_one, test_BEEP_STORE_01_write_failure_is_not_a_digit_error | ✅ |
| PYR-CONT-01 | The system shall check every enabled channel at least once per second during PAD_IDLE, for the faults the... | test_board_pyro_mk1b.c: test_mk1b_released_enable_left_alone, test_PYR_CONT_01_mk1b_a_reading_at_least_once_a_second; test_flight_pad.c: test_BUZ_01_a_clean_board_says_ok_to_fly, test_PYR_CONT_01_health_is_checked_every_second, test_PYR_CONT_01_no_second_passes_without_a_check; the suite test_board_pyro_mk1c.c | ✅ |
| PYR-CONT-02 | The system shall report each enabled channel as ready or as faulted, and shall name what the board... | test_flight_pad.c: test_BUZ_01_a_clean_board_says_ok_to_fly; the suite test_board_pyro_mk1b.c, test_board_pyro_mk1c.c, test_status_json.c | ✅ |
| PYR-CONT-03 | The pad diagnosis and announcement shall be re-derived at every check, so that a fault which appears or... | test_flight_pad.c: test_BUZ_01_a_clean_board_says_ok_to_fly, test_PYR_CONT_03_a_fault_that_appears_on_the_pad_is_announced | ✅ |
| PYR-HEALTH-01 | A board that cannot detect a fault on a channel shall treat the channel as ready | test_board_pyro_mk1b.c: test_mk1b_fire_then_a_fresh_reading, test_mk1b_as_fitted_every_channel_is_ready; test_board_pyro_mk1c.c: test_mk1c_a_failed_pulse_does_not_prevent_the_next; test_fire_rules.c: test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen, test_PYR_HEALTH_01_a_faulted_channel_still_fires | ✅ |
| PYR-HEALTH-02 | Only enabled channels shall count, for firing and for the pad verdict | test_fire_control.c: test_PYR_HEALTH_02_a_channel_that_is_not_enabled_never_fires; test_flight_pad.c: test_FLT_PHASE_04_a_state_that_is_no_state_is_a_fault, test_PYR_HEALTH_02_a_channel_set_to_none_is_not_a_fault, test_PYR_HEALTH_02_a_channel_given_to_the_script_is_not_a_pyro; test_flight_profiles.c: test_CFG_04_a_channel_set_to_none_never_fires; test_ground_test.c: test_GND_TEST_08_only_an_enabled_channel_fires, test_GND_TEST_08_a_channel_given_to_the_script_does_not_fire; the suite test_pin_assign.c | ✅ |
| FLT-BOOT-01 | The system shall complete its start-up checks before entering PAD_IDLE | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad | ✅ |
| FLT-BOOT-02 | The system shall read configuration from persistent storage during start-up | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad, test_FLT_BOOT_02_the_stored_configuration_is_what_flies | ✅ |
| FLT-BOOT-05 | The system shall detect and initialise the pressure sensor during start-up | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad, test_FLT_BOOT_05_a_sensor_slow_to_answer_is_waited_for | ✅ |
| FLT-BOOT-06 | The system shall initialise the pyrotechnic subsystem during start-up | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad | ✅ |
| FLT-BOOT-07 | The system shall perform an initial pyro health check during start-up | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad, test_FLT_BOOT_07_a_fault_found_at_start_up_is_the_first_thing_said | ✅ |
| FLT-BOOT-08 | The system shall calibrate ground pressure from at least 10 readings, so that one bad reading cannot bias it | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad, test_FLT_BOOT_07_a_fault_found_at_start_up_is_the_first_thing_said, test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference | ✅ |
| FLT-BOOT-12 | The system shall enter a terminal FAULT state, and announce general fault, when no pressure sensor answers | test_flight_boot.c: test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference, test_FLT_BOOT_12_no_sensor_is_a_terminal_general_fault, test_FLT_BOOT_12_a_sensor_that_never_finishes_coming_up_is_a_fault | ✅ |
| FLT-BOOT-13 | The system shall enter FAULT when calibration produces no samples within 10 seconds, rather than... | test_flight_boot.c: test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference, test_FLT_BOOT_13_a_calibration_with_no_samples_is_a_fault | ✅ |
| FLT-BOOT-14 | The system shall enter FAULT, and announce general fault, when its storage cannot be used | test_flight_boot.c: test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference, test_FLT_BOOT_14_unusable_storage_is_a_terminal_general_fault | ✅ |
| FLT-BOOT-15 | /api/status shall report every fault found on the pad | test_flight_pad.c: test_BUZ_CODE_02_pyro_1_is_announced_before_pyro_2; the suite test_status_json.c | ✅ |
| FLT-BOOT-17 | The system shall enter FAULT, and announce general fault, at every start of an image built for a different... | test_flight_boot.c: test_FLT_BOOT_17_an_image_built_for_another_board_is_a_terminal_general_fault, test_FLT_BOOT_17_a_board_with_its_own_stamp_or_none_reaches_the_pad | ✅ |
| FLT-BOOT-18 | The system shall enter FAULT, and announce general fault, when the configuration file exists and cannot be... | test_config.c: test_lua_baud_holds_115200, test_CFG_05_only_a_missing_file_is_rewritten; test_flight_boot.c: test_FLT_BOOT_18_an_unreadable_configuration_is_a_fault_and_the_file_is_kept | ✅ |
| FLT-RATE-01 | The system shall take at least 50 pressure readings a second from PAD_IDLE to landing | test_collector.c: test_SNS_COL_01_it_runs_with_nothing_starting_it, test_FLT_RATE_01_each_sensor_outruns_the_flight_software | ✅ |
| FLT-RATE-06 | The flight software shall act on every sample in the loop it arrives in, each as a step of its own,... | test_flight_pad.c: test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch, test_FLT_RATE_06_every_waiting_sample_is_taken_in_its_loop | ✅ |
| FLT-RATE-05 | Every detector hold and dwell that measures the sensor shall run in sample time, so that lateness in... | test_flight_profiles.c: test_DAT_08_a_thinned_log_is_refused, test_FLT_RATE_05_a_late_loop_changes_no_decision | ✅ |

## 3. Flight Data Recovery

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-3 | The user needs to retrieve flight performance data after recovery | Through SYS-DATA-01, SYS-DATA-02, SYS-DATA-03, SYS-DATA-04 | ✅ |
| SYS-DATA-01 | The system shall record flight data throughout the flight | the suite test_recorded_flight.c | ✅ |
| SYS-DATA-02 | The system shall export flight data in a standard format | Through DAT-06, DAT-07 | ✅ |
| SYS-DATA-03 | The system shall announce maximum altitude audibly after landing | test_flight_profiles.c: test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound; test_recorded_flight.c: test_TST_02_landing_follows_the_ground_hit | ✅ |
| SYS-DATA-04 | A board with a card and an accelerometer shall record the flight's motion at the accelerometer's rate on... | the suite test_hr_log.c | ✅ |
| DAT-02 | Each sample shall include | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order; the suite test_flight_log.c | ✅ |
| DAT-03 | An event shall be recorded as a sample row at the event's own time | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order; the suite test_flight_log.c | ✅ |
| DAT-04 | The system shall log events | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order | ✅ |
| DAT-06 | The system shall keep flight data in persistent storage after landing and export it as CSV when it is read... | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order; the suite test_flight_log.c | ✅ |
| DAT-07 | The CSV shall include a metadata header with configuration, flight summary and the rate it was logged at | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order; the suite test_flight_log.c | ✅ |
| DAT-08 | A flight log written with log_rate=full shall carry what is needed to replay the flight through the... | test_flight_profiles.c: test_FLT_LAND_02_a_slow_descent_is_not_a_landing, test_DAT_04_the_events_of_a_flight_in_order, test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise, test_DAT_08_a_full_rate_log_replays_to_the_same_events and 2 more | ✅ |
| DAT-09 | Each board shall hold a flight log of the longest flight it supports, at each log rate, and shall declare... | The room is reported at `/api/log/space` and each board declares its capacity in its `THEORY_OF_OPERATION.md`; no test flies a log to the declared length | ⚠️ |
| DAT-10 | Any record that holds a pressure not yet converted to altitude shall hold the temperature the sensor... | the suite test_flight_log.c | ✅ |
| FLT-LOG-06 | A flight that never lands shall keep its record | the suite test_flight_log.c | ✅ |
| FLT-LOG-07 | The flight log shall keep, by log_rate | the suite test_log_plan.c | ✅ |
| FLT-LOG-08 | Records the flight log could not keep shall be counted and reported on /api/status | the suite test_status_json.c | ✅ |
| BUZ-03 | The system shall play an altitude beep-out sequence after landing, holding it while a USB host is attached... | test_flight_profiles.c: test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound, test_BUZ_03_the_peak_is_beeped_out_after_landing, test_BUZ_03_the_beep_out_is_held_on_usb_and_resumes | ✅ |
| BUZ-04 | The altitude beep-out shall encode each digit of the peak altitude in configured units | test_buzzer.c: test_BUZ_PAT_02_counted_codes, test_BUZ_PAT_03_altitude_165_digits, test_BUZ_PAT_07_gap_between_passes, test_BUZ_PAT_08_altitude_1000_zero_digits and 1 more; test_flight_profiles.c: test_BUZ_03_the_peak_is_beeped_out_after_landing | ✅ |
| BUZ-05 | The digit 0 shall be encoded as 10 beeps | test_buzzer.c: test_BUZ_PAT_07_gap_between_passes, test_BUZ_PAT_08_altitude_1000_zero_digits, test_BUZ_PAT_09_altitude_10000 | ✅ |
| BUZ-06 | The altitude beep-out shall repeat indefinitely | the suite test_buzzer.c | ✅ |

## 4. Configuration

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-4 | The user needs to configure the system for different rockets and flight profiles | Through SYS-CFG-01, SYS-CFG-02, SYS-CFG-03, CFG-SUBSYS-01 | ✅ |
| SYS-CFG-01 | The system shall store configuration persistently across power cycles | test_config_persistence.c: test_SYS_CFG_01_a_saved_configuration_is_read_back, test_SYS_CFG_01_each_save_replaces_the_last | ✅ |
| SYS-CFG-02 | The system shall allow configuration changes without special tools | Web UI: test_ui.spec.js | ✅ |
| SYS-CFG-03 | The system shall validate configuration against the limits of the sensor and of the board | test_config.c: test_config_worst_case_fits_the_budget, test_SYS_CFG_03_a_value_beyond_its_field_is_refused_not_wrapped, test_SYS_CFG_03_a_value_that_does_not_parse_keeps_the_previous, test_SYS_CFG_03_every_bounded_field_refuses_beyond_its_row; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| CFG-01 | The system shall store configuration in an INI-format file on persistent storage | test_config_persistence.c: test_SYS_CFG_01_a_saved_configuration_is_read_back, test_CFG_01_the_file_is_ini_text; the suite test_config.c | ✅ |
| CFG-02 | The system shall parse the fields of the table below | the suite test_config.c | ✅ |
| CFG-03 | The system shall support unit settings | test_config.c: test_config_parse_all_units; test_config_persistence.c: test_CFG_03_units_it_cannot_name_are_metres; test_flight_profiles.c: test_CFG_04_a_channel_set_to_none_never_fires, test_CFG_03_a_height_in_feet | ✅ |
| CFG-04 | The system shall support pyro mode settings | test_config.c: test_config_parse_all_modes, test_config_default_ini_string, test_config_mode_none_round_trips, test_SYS_CFG_03_every_bounded_field_refuses_beyond_its_row and 1 more; test_config_persistence.c: test_SYS_CFG_01_each_save_replaces_the_last, test_CFG_05_a_board_with_no_file_starts_on_the_defaults_and_stores_them, test_CFG_04_a_mode_it_cannot_name_is_none; test_flight_profiles.c: test_CFG_04_a_channel_set_to_none_never_fires; Hardware: `support/api_check.py` | ✅ |
| CFG-05 | The system shall create a default configuration if the config file is missing | test_config.c: test_lua_baud_holds_115200, test_CFG_05_only_a_missing_file_is_rewritten, test_CFG_05_a_file_that_filled_the_buffer_is_not_parsed, test_CFG_05_a_long_commented_file_is_read_whole; test_config_persistence.c: test_SYS_CFG_01_each_save_replaces_the_last, test_CFG_05_a_board_with_no_file_starts_on_the_defaults_and_stores_them | ✅ |
| CFG-06 | The system shall preserve existing config fields not present in a partial config file | test_config.c: test_config_parse_preserves_unset, test_config_writes_no_inert_keys, test_config_serialize_refuses_to_overflow; Hardware: `support/api_check.py` | ✅ |
| CFG-07 | The system shall truncate id and name fields to 8 characters | test_config.c: test_config_parse_id_truncated, test_config_unknown_mode_serialises_as_none, test_config_default_name_is_not_truncated; Web UI: test_ui.spec.js | ✅ |
| CFG-08 | The system shall ignore unknown keys in the config file | test_config.c: test_config_parse_unknown_keys, test_config_parse_comments | ✅ |
| CFG-09 | The system shall handle both CR+LF and LF line endings | test_beep_codes.c: test_an_empty_name_keeps_the_personality, test_blanks_around_key_and_value_are_not_part_of_them; test_config.c: test_config_parse_unix_newlines, test_config_parse_no_trailing_newline, test_CFG_04_an_unnamed_mode_is_none_and_reported, test_CFG_09_blanks_around_key_and_value_are_not_part_of_them and 1 more; test_pin_assign.c: test_PIN_GT_07_ini_round_trip, test_PIN_INI_blanks_around_key_and_value_are_not_part_of_them | ✅ |
| CFG-10 | Configuration and pin changes shall take effect at start-up | test_config_persistence.c: test_CFG_03_units_it_cannot_name_are_metres, test_CFG_10_the_running_system_keeps_the_configuration_it_started_with; test_flight_pad.c: test_PYR_HEALTH_02_a_channel_given_to_the_script_is_not_a_pyro, test_CFG_10_a_saved_change_waits_for_the_next_start; test_ground_test.c: test_GND_TEST_13_a_pulse_that_energises_nothing_is_reported_not_refused, test_CFG_10_a_change_saved_in_the_mode_waits_for_the_next_start; Web UI: test_ui.spec.js; Structure: `support/structure_check.py`; Hardware: `support/api_check.py` | ✅ |
| CFG-TABLE-02 | Every configuration field shall survive a save and a load unchanged | test_config.c: test_config_defaults, test_config_roundtrip_defaults, test_config_serialize_refuses_to_overflow | ✅ |
| CFG-SUBSYS-01 | Every configuration key shall have an effect | test_config.c: test_config_default_name_is_not_truncated, test_config_writes_no_inert_keys | ✅ |
| PYR-BOARD-01 | Each board shall declare the default and the permitted range of refire_interval and fire_gap, and may... | test_config.c: test_config_defaults; test_fire_rules.c: test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused, test_PYR_BOARD_01_a_board_redefines_the_defaults, test_PYR_BOARD_01_a_configured_value_in_range_is_used | ✅ |
| PYR-BOARD-02 | A configured value outside its permitted range shall be brought to the nearest permitted value, and the... | test_fire_rules.c: test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused, test_PYR_BOARD_02_a_value_out_of_range_is_brought_in_and_reported; the suite test_status_json.c; Web UI: test_ui.spec.js | ✅ |
| PYR-BOARD-03 | The defaults and ranges in force shall be served to the web interface, so the firmware is the only place... | test_fire_rules.c: test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused; the suite test_status_json.c; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| PYR-BOARD-04 | A board constraint may change when and how often a channel is energised | test_fire_rules.c: test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused, test_PYR_BOARD_01_a_board_redefines_the_defaults, test_PYR_BOARD_02_a_value_out_of_range_is_brought_in_and_reported | ✅ |
| PIN-LABEL-01 | Every assignable pin shall carry the connector designator silkscreened on the board, and the web UI shall... | test_pin_caps.c: test_the_default_lua_list_is_consistent, test_PIN_LABEL_01_every_row_is_labelled | ✅ |
| PIN-BUZZ-01 | The buzzer shall be assignable to any pad the board declares capable of driving one, defaulting to the... | test_pin_assign.c: test_a_pad_the_board_reserves_for_something_else_is_never_given_up, test_PIN_BUZZ_01_default_is_board_not_gpio0, test_PIN_BUZZ_02_digital_user_pad_accepted, test_PIN_BUZZ_04_cannot_take_a_pad_lua_holds and 3 more | ✅ |
| PIN-BUZZ-02 | A pad driving the buzzer shall be reserved against the script, and a pad holding a script role shall not... | test_pin_assign.c: test_PIN_BUZZ_02_digital_user_pad_accepted, test_PIN_BUZZ_03_buzzer_pad_is_reserved, test_PIN_BUZZ_04_cannot_take_a_pad_lua_holds | ✅ |
| WEB-UI-02 | The web interface shall provide a guided configuration editor with input validation | Web UI: test_ui.spec.js | ✅ |
| WEB-UI-03 | The web interface shall warn when configuration has been saved but not applied | Web UI: test_ui.spec.js | ✅ |

## 5. Pressure Measurement

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-5 | The user needs accurate altitude measurement for pyro deployment and data recording | Through SYS-ALT-01, SYS-ALT-02 | ✅ |
| SYS-ALT-01 | The system shall measure altitude using barometric pressure | test_recorded_flight.c: test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon | ✅ |
| SYS-ALT-02 | The system shall operate with multiple pressure sensor types | the suite test_bmp280.c, test_sensor_bringup.c | ✅ |
| SNS-PRES-01 | The system shall auto-detect the installed pressure sensor type | the suite test_sensor_bringup.c | ✅ |
| SNS-PRES-06 | Every conversion the sensor completes shall be used, whatever its value | test_mach.c: test_SNS_PRES_11_lost_under_a_canopy_waits_for_new_samples, test_SNS_PRES_06_wild_readings_are_data_and_decide_nothing, test_SNS_PRES_06_a_pressure_with_no_logarithm_is_no_reading | ✅ |
| SNS-PRES-08 | Each sample shall carry the time its pressure was measured, to within 1 ms ‡, unaffected by lateness in... | test_collector.c: test_SNS_COL_02_no_zero_however_the_handler_is_held_off, test_SNS_PRES_08_each_stamp_is_the_middle_of_its_conversion, test_SNS_PRES_08_a_held_read_keeps_its_stamp | ✅ |
| SNS-PRES-10 | A whole second of one reading, to the pascal, shall be taken as a failed sensor | test_fire_control.c: test_PYR_MODE_04_speed_fires_at_the_descent_speed, test_SYS_DEPLOY_04_without_data_only_a_delay_fires; test_mach.c: test_SNS_EST_07_every_estimator_is_flown_and_logged, test_SNS_PRES_10_stuck_in_coast_deploys_nothing, test_SNS_PRES_06_a_pressure_with_no_logarithm_is_no_reading, test_SNS_PRES_10_a_failure_is_reported_and_the_flight_goes_on and 1 more | ✅ |
| SNS-PRES-11 | A gap of more than 250 ms between samples shall suspend decisions until a whole second of new samples exists | test_fire_control.c: test_PYR_MODE_04_speed_fires_at_the_descent_speed, test_SYS_DEPLOY_04_without_data_only_a_delay_fires; test_mach.c: test_SNS_PRES_10_stuck_in_coast_deploys_nothing, test_SNS_PRES_11_a_gap_across_apogee_waits_for_new_samples, test_SNS_PRES_11_lost_under_a_canopy_waits_for_new_samples, test_SNS_PRES_06_a_pressure_with_no_logarithm_is_no_reading and 1 more | ✅ |
| SNS-PRES-13 | The system shall keep the last 256 sensor conversions, with the time each was measured, when it was read,... | the suite test_pressure_trace.c | ✅ |
| SNS-PRES-14 | A reading the board knows its own storage activity disturbed shall not be used, and shall be counted on... | test_collector.c: test_SNS_COL_03_the_handler_does_not_run_inside_a_take, test_SNS_PRES_14_a_flash_operation_marks_the_conversion_it_ran_beside | ✅ |
| SNS-PRES-17 | On the pad, a sensor that gives no sample for 0.5 s, or that sticks (SNS-PRES-10), shall be a general fault | test_flight_pad.c: test_BUZ_CODE_02_a_general_fault_outranks_a_pyro_fault, test_SNS_PRES_17_a_sensor_that_stops_on_the_pad_is_a_general_fault, test_SNS_PRES_17_a_stuck_sensor_on_the_pad_is_a_general_fault, test_SNS_PRES_17_a_working_sensor_is_no_fault; Hardware: `support/api_check.py` | ✅ |
| SNS-PRES-15 | An MS5607's pressure and temperature shall be compensated to the datasheet's second order below 20 °C | test_ms5607.c: test_SNS_PRES_15_the_datasheets_example, test_SNS_PRES_15_nothing_changes_at_20_C_and_above, test_SNS_PRES_15_the_second_order_below_20_C, test_SNS_PRES_15_the_very_low_terms_below_minus_15_C and 1 more | ✅ |
| SNS-PRES-16 | An MS5607 whose PROM fails its CRC shall not be taken for a sensor | test_ms5607.c: test_SNS_PRES_16_the_application_notes_example, test_SNS_PRES_16_one_wrong_bit_in_any_word_is_seen; test_sensor_bringup.c: test_bringup_without_a_sensor, test_SNS_PRES_16_a_prom_that_fails_its_crc_is_no_sensor | ✅ |
| SNS-REC-01 | The flight software shall not attempt to recover a failed sensor in flight | test_flight_profiles.c: test_WEB_API_10_other_storage_access_is_refused_until_the_record_is_safe, test_SNS_REC_01_a_failed_sensor_is_logged_and_left_alone; Structure: `support/structure_check.py` | ✅ |
| SNS-EST-01 | Every flight decision shall be made on a filtered estimate of the pressure, its rate and its acceleration,... | test_estimator.c: test_SNS_EST_01_the_state_is_the_pressure_its_rate_and_its_acceleration, test_SNS_EST_01_the_sensor_noise_is_tracked; Hardware: `support/api_check.py` | ✅ |
| SNS-EST-02 | One or two consecutive bad readings, of any value the sensor can produce, shall change no decision | test_estimator.c: test_SNS_EST_01_the_sensor_noise_is_tracked, test_SNS_EST_02_one_or_two_bad_readings_leave_the_state_alone, test_SNS_EST_02_a_sustained_run_is_followed; test_flight_pad.c: test_FLT_LAUNCH_07_a_slow_rise_is_not_a_launch, test_SNS_EST_02_one_or_two_bad_readings_are_not_a_launch | ✅ |
| SNS-EST-03 | Noise alone shall cross no threshold | test_estimator.c: test_SNS_EST_02_a_sustained_run_is_followed, test_SNS_EST_03_a_steady_descent_below_a_rule_never_reads_past_it, test_SNS_EST_03_a_still_board_reads_still; test_fire_rules.c: test_PYR_REFIRE_01_a_working_flight_gets_no_lasting_refire; test_flight_pad.c: test_FLT_LAUNCH_07_a_slow_rise_is_not_a_launch, test_SNS_EST_02_one_or_two_bad_readings_are_not_a_launch, test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch; test_mach.c: test_SNS_EST_03_a_swinging_canopy_moves_neither_the_main_nor_the_landing | ✅ |
| SNS-EST-04 | The filtered state shall follow a real change | test_estimator.c: test_SNS_EST_03_a_still_board_reads_still, test_SNS_EST_04_a_speed_that_passes_a_rule_is_reported_within_two_seconds; test_fire_rules.c: test_PYR_REFIRE_01_a_charge_that_lights_on_the_third_pulse, test_SNS_EST_04_a_lost_canopy_is_seen_within_two_seconds | ✅ |
| SNS-EST-05 | Every flight comparison shall be made in pressure | test_atmosphere.c: test_SNS_EST_05_pressure_at_the_standards_altitudes, test_SNS_EST_05_altitude_inverts_pressure, test_SNS_EST_05_a_height_is_measured_from_the_pad; test_estimator.c: test_SNS_EST_04_a_speed_that_passes_a_rule_is_reported_within_two_seconds, test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad | ✅ |
| SNS-EST-06 | The filtered state shall come from one of the estimators the build carries, each behind one interface... | test_estimators.c: test_SNS_EST_06_each_has_a_name_config_can_hold; test_mach.c: test_FLT_APO_08_the_peak_is_logged, test_SNS_EST_06_the_flight_obeys_the_configured_estimator, test_SNS_EST_06_an_unknown_name_is_the_default; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| SNS-EST-07 | Every estimator the build carries shall be given every reading, whichever is obeyed | test_mach.c: test_SNS_EST_06_an_unknown_name_is_the_default, test_SNS_EST_07_every_estimator_is_flown_and_logged | ✅ |
| SNS-EST-08 | An estimator shall report whether its model explains the readings | test_estimators.c: test_SNS_EST_08_explaining_takes_a_second_of_readings, test_SNS_EST_08_a_step_no_motion_makes_is_not_explained, test_SNS_EST_08_a_few_missing_readings_are_not_a_gap, test_SNS_EST_08_a_ballistic_arc_is_explained_over_the_top and 1 more | ✅ |
| SNS-EST-09 | Each estimator shall be told when the board pulses a channel | test_estimators.c: test_SNS_EST_08_an_estimate_does_not_fall_through_the_ground, test_SNS_EST_09_the_lumped_estimator_sits_out_its_own_charge | ✅ |
| SNS-COL-01 | One collector shall own the sensor and its bus | test_collector.c: test_SNS_COL_01_it_runs_with_nothing_starting_it, test_SNS_COL_01_a_cycle_is_each_conversion_in_turn | ✅ |
| SNS-COL-02 | A read shall follow its own conversion's command by the part's worst-case conversion time, and no read... | test_collector.c: test_SNS_COL_01_a_cycle_is_each_conversion_in_turn, test_SNS_COL_02_every_read_waits_out_the_worst_case, test_SNS_COL_02_no_zero_however_the_handler_is_held_off, test_SNS_COL_02_the_bmp280_is_read_after_its_worst_case and 1 more | ✅ |
| SNS-COL-03 | The collector shall keep the four newest cycles not yet taken | test_collector.c: test_SNS_PRES_08_a_held_read_keeps_its_stamp, test_SNS_COL_03_a_task_60_ms_late_loses_nothing, test_SNS_COL_03_four_are_kept_and_the_oldest_goes_first, test_SNS_COL_03_a_cycle_is_taken_once and 1 more | ✅ |
| SNS-COL-04 | A bus transfer that fails shall be counted on /api/status by its cause | test_collector.c: test_SNS_PRES_14_a_flash_operation_marks_the_conversion_it_ran_beside, test_SNS_COL_04_a_failed_transfer_is_counted_by_its_cause, test_SNS_COL_04_a_transfer_that_never_ends_is_aborted, test_SNS_COL_04_a_failed_cycle_is_not_queued; Hardware: `support/api_check.py` | ✅ |
| SNS-COL-05 | After three failed transfers in a row the collector shall clear the bus (nine clocks and a STOP), reset... | test_collector.c: test_SNS_COL_04_a_failed_cycle_is_not_queued, test_SNS_COL_05_three_failures_clear_the_bus_and_reset_the_part, test_SNS_COL_05_the_part_is_left_to_reload_after_its_reset, test_SNS_COL_05_a_silent_part_is_tried_until_it_answers | ✅ |
| SNS-COL-06 | The collector's interrupt handler and everything it calls shall run from RAM. The build shall fail otherwise | CI runs `support/prove_core0.py` on every image: it fails if `collector_alarm_isr` or anything it calls is outside RAM | ✅ |
| SNS-ALT-01 | Altitude shall be reported relative to the launch pad, computed from the ground reference and the current... | test_atmosphere.c: test_SNS_EST_05_a_height_is_measured_from_the_pad, test_SNS_ALT_01_below_the_pad_is_negative; test_flight_profiles.c: test_SNS_MAX_01_best_effort_above_the_sensors_range, test_SNS_ALT_01_the_reported_peak_is_not_clamped, test_SNS_ALT_01_below_the_pad_is_a_negative_altitude; Structure: `support/structure_check.py` | ✅ |
| SNS-MAX-01 | Each board shall declare its sensor's pressure range and its height for proper operation, the greatest... | test_flight_profiles.c: test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height, test_SNS_MAX_01_best_effort_above_the_sensors_range; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |

## 6. Telemetry

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-6 | The user needs real-time flight data transmitted for ground monitoring | Through SYS-TEL-01 | ✅ |
| SYS-TEL-01 | The system shall transmit flight data via serial interface during flight | test_recorded_flight.c: test_TST_02_the_recorded_flight_runs_pad_to_landed | ✅ |
| TEL-01 | The system shall output telemetry in $PYRO NMEA sentence format | test_config.c: test_config_writes_no_inert_keys; test_telemetry.c: test_TEL_01_every_sentence_is_pyro_nmea_with_a_good_checksum | ✅ |
| TEL-02 | Each telemetry sentence shall include an XOR checksum | test_telemetry.c: test_TEL_01_every_sentence_is_pyro_nmea_with_a_good_checksum | ✅ |
| TEL-03 | The system shall output one telemetry message a second in PAD_IDLE, in flight and in LANDED | test_config.c: test_config_writes_no_inert_keys; test_telemetry.c: test_TEL_09_the_sequence_counts_every_sentence, test_TEL_03_one_message_a_second_in_every_state, test_TEL_03_one_message_a_second_after_landing | ✅ |
| TEL-05 | The system shall not output telemetry during start-up or in FAULT. A faulted board shall instead send a... | test_flight_boot.c: test_FLT_BOOT_01_every_check_runs_before_the_pad, test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference; test_ground_test.c: test_GND_TEST_05_started_with_the_switch_closed_enters_ground_test | ✅ |
| TEL-06 | Each sentence shall include | test_telemetry.c: test_TEL_01_every_sentence_is_pyro_nmea_with_a_good_checksum, test_TEL_06_the_sentence_carries_the_flight | ✅ |
| TEL-07 | The state field shall map | test_telemetry.c: test_TEL_03_one_message_a_second_in_every_state | ✅ |
| TEL-08 | The flags field shall encode | test_telemetry.c: test_TEL_08_the_flags_follow_the_flight | ✅ |
| TEL-09 | The sequence number shall increment with each sentence | test_telemetry.c: test_TEL_09_the_sequence_counts_every_sentence | ✅ |
| TEL-10 | The thrust flag shall only be set during ASCENT when under thrust | test_telemetry.c: test_TEL_10_thrust_is_reported_only_in_the_burn | ✅ |
| TEL-11 | A flight event -- apogee, a fire, landing -- shall be queued and carried by the next telemetry message, so... | test_telemetry.c: test_TEL_06_the_sentence_carries_the_flight, test_TEL_11_each_event_is_carried_once_by_the_next_message, test_TEL_11_no_event_is_lost_in_an_emergency, test_TEL_11_the_queue_holds_more_than_a_second_of_events | ✅ |
| TEL-12 | The telemetry port shall accept no commands | Structure: `support/structure_check.py` | ✅ |

## 7. Pyro Protection

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-7 | The user needs protection against pyrotechnic faults that could damage the system or cause unsafe conditions | Through SYS-FAULT-01, SYS-FAULT-02, SYS-FAULT-03 | ✅ |
| SYS-FAULT-01 | Each board shall protect its pyro drive against a load that would damage it | the suite test_board_pyro_mk1c.c | ✅ |
| SYS-FAULT-02 | The system shall detect the pyro fault conditions the board can sense | the suite test_board_pyro_mk1c.c | ✅ |
| SYS-FAULT-03 | The system shall notify the user of pyro fault conditions | Through PYR-FAULT-03 | ✅ |
| PYR-FAULT-01 | A board's protection may end a pulse | test_board_pyro_mk1c.c: test_mk1c_a_bus_that_will_not_charge_is_gated_at_the_deadline, test_mk1c_a_failed_pulse_does_not_prevent_the_next | ✅ |
| PYR-FAULT-02 | A board that can sense that its protection acted during a pulse shall record it | test_board_pyro_mk1c.c: test_mk1c_a_stopped_loop_disarms, test_mk1c_a_bus_that_will_not_charge_is_gated_at_the_deadline | ✅ |
| PYR-FAULT-03 | A fault during a pulse shall be shown to the user in the flight log and on /api/status | the suite test_board_pyro_mk1c.c, test_status_json.c; Web UI: test_ui.spec.js | ✅ |
| PYR-VERIFY-01 | A board that can sense it shall record, after a pulse, whether the channel opened | test_board_pyro_mk1a.c: test_PYR_DEPLOY_02_mk1a_refuses_a_channel_out_of_range, test_PYR_VERIFY_01_mk1a_fired_channel_unknown_until_a_check_after_the_pulse; test_board_pyro_mk1b.c: test_PYR_DEPLOY_02_mk1b_refuses_a_channel_out_of_range, test_PYR_VERIFY_01_mk1b_fired_channel_unknown_until_a_check_after_the_pulse; test_board_pyro_mk1c.c: test_PYR_ARM_03_mk1c_fire_clock_ahead_of_the_loop_is_no_timeout, test_PYR_VERIFY_01_mk1c_fired_channel_unknown_until_the_next_presence_test; test_fire_rules.c: test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen, test_PYR_VERIFY_01_a_channel_that_opened_is_not_recorded, test_PYR_VERIFY_01_a_verdict_that_comes_late_is_recorded, test_PYR_VERIFY_01_nothing_is_recorded_without_a_verdict | ✅ |
| PYR-ARM-01 | Software that stops running shall leave no channel energised or armed, within the board-declared time,... | the suite test_board_pyro_mk1c.c; MK1C: about 35 ms. MK1A and MK1B end a pulse from the loop, so a stopped loop leaves it energised until the watchdog, 1 s: the 50 ms bound is not met on those boards | ⚠️ |
| PYR-ARM-03 | A board whose firing path must be made ready before a pulse shall deliver the pulse when the path is ready... | test_board_pyro_mk1c.c: test_mk1c_a_stopped_loop_disarms, test_mk1c_a_bus_that_will_not_charge_is_gated_at_the_deadline, test_mk1c_flash_waits_out_a_fire, test_PYR_ARM_03_mk1c_fire_clock_ahead_of_the_loop_is_no_timeout | ✅ |
| PYR-ARM-05 | Storing data shall not shorten, lengthen or interrupt a pulse | the suite test_board_pyro_mk1c.c; `main_hardware.c` shuts the storage window while `board_flash_ok()` is false, by inspection | ✅ |
| PYR-ARM-06 | A pulse that fails shall leave nothing latched that prevents or delays the other channel's pulse | the suite test_board_pyro_mk1c.c | ✅ |

## 8. Web Interface & Network

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-8 | The user needs to monitor, configure, and update the system from a computer without special software | Through SYS-WEB-01, SYS-WEB-02 | ✅ |
| SYS-WEB-01 | The system shall provide a web interface accessible via USB connection | Web UI: test_ui.spec.js | ✅ |
| SYS-WEB-02 | The system shall be discoverable on the network without manual IP configuration | Hardware: `support/test_network.py` | ✅ HW |
| WEB-NET-01 | The system shall present a USB network interface to the host computer | Hardware: `support/test_network.py` | ✅ HW |
| WEB-NET-02 | The system shall serve DHCP and take the address 192.168.N.1, where N is the board's own subnet (WEB-NET-06) | Hardware: `support/test_network.py` | ✅ HW |
| WEB-NET-03 | The system shall advertise its hostname via mDNS | Hardware: `support/test_network.py` | ✅ HW |
| WEB-NET-04 | The system shall advertise a DNS-SD service for automatic discovery | Hardware: `support/test_network.py` | ✅ HW |
| WEB-NET-05 | A frame the USB link cannot take yet shall be held and sent in order as soon as it can, not dropped; one... | — | ⚠️ |
| WEB-NET-06 | A board shall have a unique network address that survives restarts, without factory programming | test_mac_random.c: test_WEB_NET_06_drawn_mac_is_local_unicast, test_WEB_NET_06_subnet_is_never_0_1_or_255, test_WEB_NET_06_same_seed_different_samples_differ, test_WEB_NET_06_no_repeat_across_many_boards and 4 more | ✅ |
| WEB-NET-07 | A received frame the device cannot hold -- no buffer free, an empty frame, a frame that will not copy, or... | test_net_txq.c: test_WEB_NET_07_a_frame_is_held_until_lwip_takes_it, test_WEB_NET_07_with_no_buffer_the_frame_is_handed_back, test_WEB_NET_07_an_empty_frame_is_handed_back, test_WEB_NET_07_a_frame_that_will_not_copy_is_handed_back_and_its_buffer_freed and 2 more | ✅ |
| WEB-API-01 | The system shall serve device status as JSON at /api/status | the suite test_status_json.c; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| WEB-API-02 | The system shall serve the configuration file at /api/config (GET) | Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| WEB-API-03 | The system shall accept configuration updates at /api/config (POST) and write to persistent storage | Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| WEB-API-04 | The system shall accept firmware updates at /api/ota (POST), answer before it restarts, and answer Expect | Hardware: an update over HTTP to MK1A, MK1B and MK1C, every stored file kept | ✅ HW |
| WEB-API-05 | The system shall trigger a device restart at /api/reboot (POST) | Hardware: 200 with CORS, and the board back in PAD_IDLE | ✅ HW |
| WEB-API-06 | The system shall serve flight data as CSV at /api/flight.csv, framed by Content-Length | the suite test_flight_log.c; Web UI: test_ui.spec.js | ✅ |
| WEB-API-07 | The API shall grant no cross-origin access (no Access-Control-Allow-Origin; Cross-Origin-Resource-Policy | test_http.c: test_WEB_HTTP_04_a_post_with_no_content_length_is_411, test_WEB_API_07_a_request_naming_another_host_is_refused, test_WEB_API_07_a_post_without_the_x_pyro_header_is_refused, test_WEB_API_07_the_board_answers_to_its_names_and_its_own_address; Hardware: `support/api_check.py`, `support/http_stream_check.py` | ✅ |
| WEB-API-08 | The web API and USB shall stay live in flight | test_flight_profiles.c: test_BUZ_03_the_beep_out_is_held_on_usb_and_resumes, test_WEB_API_08_only_the_flight_record_is_stored_in_flight; test_http.c: test_OTA_06_no_sector_is_written_past_the_slot_end, test_WEB_API_08_every_route_that_touches_storage_waits_for_the_flight_log; The web server answering 423 and dropping a held transfer: by inspection; a bench check in a chamber flight is owed | ✅ |
| WEB-API-09 | The system shall erase the flight log on request at /api/flight/erase (POST), unless the log is being written | Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| WEB-API-10 | A request for a file shall be refused with 423 while the flight log is being written | test_flight_profiles.c: test_BUZ_03_the_beep_out_is_held_on_usb_and_resumes, test_WEB_API_10_other_storage_access_is_refused_until_the_record_is_safe | ✅ |
| WEB-API-11 | /api/status shall be self-consistent, taken at one instant of the flight, shall keep its keys and their... | the suite test_status_json.c | ✅ |
| WEB-API-12 | The system shall report at /api/log/space the bytes the next flight's log has room for, the size of a... | Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| WEB-API-13 | The system shall report at /api/net what the network has in use, has refused and has dropped, and the USB... | the suite test_net_stats.c | ✅ |
| WEB-API-14 | A file shall be named by /-separated names of letters, digits, ., _ and -, none of them . or ... Any other... | test_http.c: test_WEB_API_07_the_board_answers_to_its_names_and_its_own_address, test_WEB_API_14_a_path_that_climbs_out_or_hides_its_name_is_refused; Hardware: `support/http_stream_check.py` | ✅ |
| WEB-HTTP-01 | The HTTP server shall treat each connection as a byte stream | the suite test_http.c; Hardware: `support/http_stream_check.py` | ✅ |
| WEB-HTTP-02 | Every response shall be framed by Content-Length and carry Connection | the suite test_http.c; Hardware: `support/api_check.py`, `support/http_stream_check.py` | ✅ |
| WEB-HTTP-03 | The server shall read a request body only as fast as it consumes it, so that TCP flow control, not a... | the suite test_http.c; Hardware: `support/http_stream_check.py` | ✅ |
| WEB-HTTP-04 | The server shall refuse a malformed or oversized request with its HTTP status | test_http.c: test_HTTP_18_a_unit_answers_away_from_the_service_call, test_WEB_HTTP_04_a_post_with_no_content_length_is_411; Hardware: `support/http_stream_check.py` | ✅ |
| WEB-UI-01 | The web interface shall display device status in the configured units | Web UI: test_ui.spec.js | ✅ |
| WEB-UI-04 | The web interface shall display flight summary data and allow CSV download | Web UI: test_ui.spec.js | ✅ |
| WEB-UI-05 | The web interface shall support firmware upload and update checking | Web UI: test_ui.spec.js | ✅ |
| WEB-UI-06 | The Config tab shall offer the three logging plans, and shall estimate the longest flight the log holds... | Web UI: test_ui.spec.js | ✅ |

## 9. Firmware Update

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-9 | The user needs to update firmware safely without risk of bricking the device | Through SYS-OTA-01, SYS-OTA-02 | ⚠️ |
| SYS-OTA-01 | The system shall support firmware updates without physical access to the board | Through OTA-01, OTA-02 | ✅ |
| SYS-OTA-02 | The system shall recover from a failed firmware update | Through OTA-03, OTA-04, OTA-05 | ⚠️ |
| OTA-01 | The system shall support over-the-air firmware updates via HTTP | Hardware: updates to MK1A, MK1B and MK1C through `support/upload_fw.sh` | ✅ HW |
| OTA-02 | The system shall write new firmware to an inactive slot while continuing to run | Through BLD-05; Hardware: the board serves pages while the image is written | ✅ HW |
| OTA-03 | The system shall automatically revert to the previous firmware if the new firmware does not reach normal... | Hardware only: an image that never commits is rolled back by the bootloader; no scripted check | ⚠️ |
| OTA-04 | A failed or interrupted update shall not affect the currently running firmware | Through OTA-06, OTA-07; Hardware only: no scripted check | ⚠️ |
| OTA-05 | Firmware built for a different board shall not be kept | the suite test_board_selftest.c | ✅ |
| OTA-06 | An image longer than the download slot shall be refused with 413 before any of it is written, and no write... | test_http.c: test_WEB_API_14_a_path_that_climbs_out_or_hides_its_name_is_refused, test_OTA_06_an_image_larger_than_the_download_slot_is_refused, test_OTA_06_no_sector_is_written_past_the_slot_end; Hardware: `support/http_stream_check.py` | ✅ |
| OTA-07 | An update shall begin by marking the download slot invalid, so that an interrupted transfer is never... | — | ⚠️ |

## 10. Scripting

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-15 | The user needs to run auxiliary functions -- lights, cameras, payload, serial devices -- from a script,... | Through SYS-LUA-01, SYS-LUA-02 | ⚠️ |
| SYS-LUA-01 | Every board shall run an operator-supplied Lua script with access to the flight's state and to the pads... | the suite test_lua.c; MK1C-SD is built without a script: J3 carries its SPI bus | ⚠️ |
| SYS-LUA-02 | No script shall be able to block, prevent, or delay beyond its normal timing any behaviour of the board | the suite test_lua.c; Structure: `support/structure_check.py` | ✅ |
| LUA-ISO-01 | A script that loops, faults or exhausts its memory shall cost only itself | the suite test_lua.c | ✅ |
| LUA-ISO-02 | A script shall read the flight's state and never write it | the suite test_lua.c | ✅ |
| LUA-ISO-03 | A script shall have no means to fire an enabled pyro channel or to stop one firing | the suite test_lua.c; Structure: `support/structure_check.py` | ✅ |
| LUA-ISO-04 | A script shall have no access to files | the suite test_lua.c | ✅ |
| LUA-ISO-05 | No construct of the language shall let a script outlast its limits | the suite test_lua.c | ✅ |
| LUA-ISO-06 | A script's numbers shall be 32-bit integers and 32-bit floats on every build, the simulator included, and... | the suite test_lua.c | ✅ |
| LUA-SAFE-01 | Whatever script is stored, the board shall start, reach its pad state and serve its web interface, so a... | The start-up gate is in `src/lua/lua_app.c`; a commanded restart now clears its mark (`lua_app_restart_commanded()`). Bench check owed: reboot within 15 s of start, script still runs | ⚠️ |
| LUA-PAD-01 | Every pad shall have one owner, the flight software or the script, set by the pin assignment and fixed... | the suite test_lua.c, test_pin_assign.c; Web UI: test_ui.spec.js | ✅ |
| LUA-PAD-02 | A script shall reach a resource by its assigned name and kind | the suite test_lua.c | ✅ |
| LUA-PAD-03 | Each board shall declare the resources it offers to scripts | the suite test_pin_caps.c; Web UI: test_ui.spec.js | ✅ |
| LUA-RUN-01 | The enabled script shall always run | `lua_app_service()` is called every loop whatever the flight state, by inspection. Bench check owed: a script running through a bench flight and through ground test mode | ⚠️ |
| LUA-RUN-02 | The script's on_event() shall be offered every flight event the flight log records, by its logged name, in... | test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee, test_LUA_RUN_02_each_flight_event_is_offered_to_the_script; the suite test_lua.c | ✅ |
| LUA-RUN-03 | The state names a script compares against (flight.<NAME>) shall carry the flight software's own state values | the suite test_lua.c | ✅ |
| LUA-MGT-01 | The web interface shall edit, check, save and remove the script | the suite test_lua.c; Web UI: test_ui.spec.js | ✅ |
| LUA-MGT-02 | A console shall show the script's output and errors, and whether the script is running or why it is not | Web UI: test_ui.spec.js | ✅ |
| LUA-IO-01 | The web UI shall export the script to a local file and import one back, so a program survives the loss of... | Web UI: test_ui.spec.js | ✅ |
| LUA-IO-02 | An imported program shall land in the editor and not on the device, so a mis-picked file costs nothing... | Web UI: test_ui.spec.js | ✅ |

## 11. Ground Test

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-12 | The user needs to verify pyro circuits and system behavior on the ground without a computer | Through SYS-TEST-01, USB-08 | ✅ |
| SYS-TEST-01 | The system shall support a ground test of its pyro channels by a test switch | Through GND-TEST-05, GND-TEST-06, GND-TEST-07, GND-TEST-12, GND-TEST-13 | ✅ |
| GND-TEST-05 | A board started with its ground test switch closed, and held closed for 0.5 s at the end of the start-up... | test_ground_test.c: test_GND_TEST_05_started_with_the_switch_closed_enters_ground_test, test_GND_TEST_05_started_with_the_switch_open_goes_to_the_pad, test_GND_TEST_05_closed_only_briefly_goes_to_the_pad, test_GND_TEST_05_a_resumed_flight_outranks_the_switch | ✅ |
| GND-TEST-06 | Ground test mode shall be announced by three long beeps and a pause, repeating | test_buzzer.c: test_BUZ_PAT_10_usb_ok_is_one_double_chirp; test_ground_test.c: test_GND_TEST_05_started_with_the_switch_closed_enters_ground_test, test_GND_TEST_06_usb_does_not_take_the_buzzer; test_ground_test_seq.c: test_GT_mode_is_announced | ✅ |
| GND-TEST-07 | The switch opened, once it has been held closed in ground test mode, shall start the procedure | test_buzzer.c: test_BUZ_PAT_10_usb_ok_is_one_double_chirp; test_ground_test.c: test_GND_TEST_06_usb_does_not_take_the_buzzer, test_GND_TEST_07_the_procedure; test_ground_test_seq.c: test_GT_mode_is_announced, test_GT_both_channels | ✅ |
| GND-TEST-08 | Only an enabled channel shall fire | test_buzzer.c: test_BUZ_PAT_10_usb_ok_is_one_double_chirp; test_ground_test.c: test_GND_TEST_06_usb_does_not_take_the_buzzer, test_GND_TEST_07_the_procedure, test_GND_TEST_08_only_an_enabled_channel_fires, test_GND_TEST_08_a_channel_given_to_the_script_does_not_fire; test_ground_test_seq.c: test_GT_the_countdown_is_a_second_a_count, test_GT_only_channel_1, test_GT_only_channel_2, test_GT_no_channel | ✅ |
| GND-TEST-09 | The switch shall count as opened only after it has been held closed for 1 s in ground test mode, and a... | test_ground_test.c: test_GND_TEST_06_usb_does_not_take_the_buzzer, test_GND_TEST_07_the_procedure, test_GND_TEST_09_an_opening_counts_only_after_a_second_held, test_GND_TEST_09_a_bounce_is_not_an_opening; test_ground_test_seq.c: test_GT_no_channel, test_GT_a_release_before_arming_does_nothing, test_GT_a_bounce_is_not_a_release | ✅ |
| GND-TEST-10 | The switch closed again during a countdown or the tone shall stop the procedure before the next fire, and... | test_ground_test.c: test_GND_TEST_06_usb_does_not_take_the_buzzer, test_GND_TEST_07_the_procedure, test_GND_TEST_10_closing_the_switch_stops_the_procedure; test_ground_test_seq.c: test_GT_a_bounce_is_not_a_release, test_GT_reasserting_aborts_the_countdown, test_GT_reasserting_in_the_tone_stops_the_second | ✅ |
| GND-TEST-11 | Ground test mode shall last until the next start | test_ground_test.c: test_GND_TEST_10_closing_the_switch_stops_the_procedure, test_GND_TEST_11_never_flies, test_GND_TEST_11_nothing_follows_the_all_clear; test_ground_test_seq.c: test_GND_TEST_13_a_fire_that_energised_nothing_is_recorded_and_the_procedure_goes_on, test_GT_done_is_final | ✅ |
| GND-TEST-12 | The ground test switch shall be assignable as none, a switch from one pad to ground, or a switch across... | test_ground_test_seq.c: test_GT_phase_names; test_pin_assign.c: test_PIN_BUZZ_07_ini_round_trip, test_PIN_GT_01_default_is_none, test_PIN_GT_05_pads_it_cannot_take, test_PIN_GT_08_the_switch_may_join_the_buzzers_pad_to_another; Web UI: test_ui.spec.js; Hardware: `support/api_check.py`; The pulsed read on the buzzer's pad (`gts_tick()`) is platform code: bench check owed, `docs/ground_test_on_buzzer_pad.md` | ✅ |
| GND-TEST-13 | A ground test fire shall be delivered on command | test_ground_test.c: test_GND_TEST_11_nothing_follows_the_all_clear, test_GND_TEST_13_no_health_reading_withholds_a_fire, test_GND_TEST_13_a_pulse_that_energises_nothing_is_reported_not_refused; test_ground_test_seq.c: test_GT_a_busy_fire_is_asked_again, test_GND_TEST_13_a_fire_that_energised_nothing_is_recorded_and_the_procedure_goes_on | ✅ |

## 12. On USB

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-13 | The user needs a board on the bench, plugged into a computer or a charger, to stay quiet and never behave... | Through SYS-USB-01 | ✅ |
| SYS-USB-01 | While attached to USB, the system shall not detect a flight and shall not announce its status, unless the... | Through USB-01, USB-02, USB-03, USB-04, USB-05, USB-07, USB-08 | ✅ |
| USB-01 | While a USB host is attached and test mode is off, the system shall not declare launch, shall not resume a... | test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached; test_resume.c: test_FLT_BROWN_05_on_usb_is_reported; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |
| USB-02 | While a USB host is attached and test mode is off, the system shall not announce the pad verdict, a... | test_flight_boot.c: test_FLT_BOOT_17_a_board_with_its_own_stamp_or_none_reaches_the_pad, test_USB_02_a_general_fault_is_not_said_on_usb; test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached, test_USB_02_nothing_is_announced_while_attached; test_flight_profiles.c: test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound; Hardware: `support/api_check.py` | ✅ |
| USB-03 | On attach, and on leaving test mode while attached, the system shall play one OK-on-USB double chirp, and... | test_buzzer.c: test_BUZ_PAT_09_altitude_10000, test_BUZ_PAT_10_usb_ok_is_one_double_chirp; test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached, test_USB_03_one_chirp_on_attach_and_the_verdict_on_detach; Hardware: `support/api_check.py` | ✅ |
| USB-04 | On detach, the system shall resume what it would have been saying, and restart the 10 s dwell of FLT-BROWN-01 | test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached, test_USB_03_one_chirp_on_attach_and_the_verdict_on_detach, test_USB_04_the_record_waits_ten_seconds_from_the_detach; test_flight_profiles.c: test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound | ✅ |
| USB-05 | From launch to landing, the attach state shall be ignored | test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached, test_USB_05_attachment_is_ignored_in_flight | ✅ |
| USB-07 | The system shall judge a host attached only on evidence that a host is present, so that every detection... | test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached | ✅ |
| USB-08 | An operator-selected test mode shall make the system behave on USB as it does on battery | test_flight_pad.c: test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds, test_USB_01_no_launch_and_no_record_while_attached, test_USB_05_attachment_is_ignored_in_flight, test_USB_08_test_mode_flies_on_usb; Web UI: test_ui.spec.js; Hardware: `support/api_check.py` | ✅ |

## 13. Bench Flight

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-14 | The user needs to see the board fly a flight, to any altitude it may reach, before it flies one | Through SYS-SIM-01 | ✅ |
| SYS-SIM-01 | The system shall fly a scripted flight on the bench, to the top of its sensor's range, through its own... | the suite test_flight_sim.c; Hardware: `support/bench_flight.py` | ✅ |
| SIM-01 | A bench flight shall start only from PAD_IDLE with test mode on, and only a profile that can fly | test_flight_sim.c: test_SIM_01_profiles_that_cannot_fly_are_refused, test_SIM_01_a_bench_flight_starts_only_from_the_pad_in_test_mode; Hardware: `support/bench_flight.py` | ✅ |
| SIM-02 | While one flies, the profile's pressure, from the 1976 US Standard Atmosphere, carrying each reading's own... | test_flight_sim.c: test_SIM_02_the_coast_peaks_at_the_apogee_asked_for, test_SIM_02_phases_run_in_order_and_it_lands, test_SIM_02_descent_times_at_constant_rates, test_SIM_02_thin_air_speeds_the_drogue and 3 more; Hardware: `support/bench_flight.py` | ✅ |
| SIM-03 | From the start of a bench flight until the board restarts, every fire shall be mocked and logged as one; a... | test_flight_sim.c: test_SIM_04_with_both_failed_it_falls_ballistic_to_the_ground, test_SIM_03_the_channels_stay_mocked_after_a_stop; Hardware: `support/bench_flight.py` | ✅ |
| SIM-04 | The bench flight shall offer profiles in which a canopy fails, so the re-fire and emergency rules can be... | test_flight_sim.c: test_SIM_02_the_bench_ends_when_both_have_landed, test_SIM_04_the_descent_starts_from_rest, test_SIM_04_a_failed_drogue_falls_ballistic_until_the_main, test_SIM_04_a_failed_main_stays_at_the_drogues_rate and 1 more; Hardware: `support/bench_flight.py` | ✅ |

## 14. Card and High-Rate Log

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SD-01 | A card that fails to come up shall leave the board on its internal storage, and shall be retried without a... | Hardware only, on MK1C-SD: the card pulled and refitted, the board on its internal storage meanwhile | ✅ HW |
| SD-02 | While a card is mounted every file shall live on it, except the board's identity and the resume record | Hardware only, on MK1C-SD: `/api/log/space` store `sd`, config and pages served from the card | ✅ HW |
| HR-01 | A high-rate log shall open with the second before launch, and launch shall wait on nothing to start it | test_hr_log.c: test_HR_01_between_flights_the_file_is_ready_and_the_ring_keeps_half | ✅ |
| HR-02 | From launch to landing, and on the bench on request, the high-rate log shall record every accelerometer... | test_hr_log.c: test_HR_02_a_flight_is_logged_whole_with_the_second_before_it | ✅ |
| HR-04 | Every record shall carry a check of its payload, a log shall be read to its last whole record, and a log a... | test_hr_log.c: test_HR_04_a_log_a_power_cut_left_is_kept_under_a_number | ✅ |
| HR-05 | A log the card cannot keep up with shall drop whole records and count them, and nothing the logger does... | test_hr_log.c: test_HR_05_a_full_ring_drops_whole_records_and_counts_them | ✅ |
| HR-06 | A log whose file the card can no longer take shall go on in a new file from the next whole record, with... | test_hr_log.c: test_HR_05_a_full_ring_drops_whole_records_and_counts_them, test_HR_06_a_card_mounted_again_under_a_log_goes_on_in_a_new_file, test_HR_06_a_card_that_refuses_writes_for_a_while_loses_one_record | ✅ |

## 15. Power

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-11 | The user needs the flight computer to operate on battery for extended pad time | Through SYS-PWR-01 | ⚠️ |
| SYS-PWR-01 | The system shall minimise its power consumption wherever doing so affects no functional requirement | No measurement. Battery sizing is the user's, by characterisation | ⚠️ |

## 16. Project Requirements

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-10 | Contributors need to develop and test flight software without flight hardware | Through SYS-PORT-01, SYS-PORT-02, SYS-CFG-04, BLD-04, TST-08, TST-09, CODE-01, CODE-03, CODE-04, CODE-05, CODE-06, CODE-07, CODE-10 | ⚠️ |
| SYS-PORT-01 | The flight software shall be testable on a host computer without hardware | Through HAL-01, HAL-02, HAL-03, HAL-05, HAL-06, BLD-01, BLD-02, TST-01, TST-02, TST-03 | ⚠️ |
| SYS-PORT-02 | The flight software shall be runnable in a browser-based simulation | Web UI: test_sim.spec.js | ✅ |
| SYS-CFG-04 | Adding a configuration field shall require a change in one place | the suite test_config.c | ✅ |
| HAL-01 | Flight logic source files shall contain no platform-specific code or conditional compilation | Structure: `support/structure_check.py` | ✅ |
| HAL-02 | All hardware interaction shall occur through a defined HAL interface | Structure: `support/structure_check.py` | ✅ |
| HAL-03 | The HAL interface shall support at least three implementations | Three implementations build in CI: `src/hal_common/`, `test/hal_test.c`, `boards/sim/hal_sim.c` | ✅ |
| HAL-04 | The same flight logic source files shall compile unchanged for all targets | `src/flight_sources.txt` is the one list the firmware, the host suites, the simulator and the WASM build compile | ✅ |
| HAL-05 | The HAL shall be the seam for testing | Every flight suite links `test/hal_test.c` and nothing of a board | ✅ |
| HAL-06 | Each board's HAL shall be validated on hardware, independently of the flight software, with test equipment... | Through BLD-06; Not built yet: the options are in `docs/hal_validation_apps.md` | ⚠️ |
| BLD-01 | The build system shall produce firmware for every supported board | CI builds every board | ✅ |
| BLD-02 | The build system shall produce host-compiled test executables | CI builds and runs every host suite (`scripts/run_host_tests.sh`) | ✅ |
| BLD-03 | The build system shall produce a host-compiled flight simulator | CI builds `pyro_sim` and the WASM simulator | ✅ |
| BLD-04 | The build system shall generate the firmware's version from the VERSION file | CI: `support/version.py` against `VERSION` | ✅ |
| BLD-05 | The build system shall support A/B firmware images for update | Hardware: both slots written and booted on MK1A, MK1B and MK1C | ✅ HW |
| BLD-06 | The build system shall produce HAL validation applications, separate from the flight firmware | Not built yet: `docs/hal_validation_apps.md` | ⚠️ |
| TST-01 | Flight tests shall be black box | Through CODE-08, CODE-09; The flight suites drive `flight_init()`, `dispatch_state()` and the HAL only (`test/board_harness.c`); `support/structure_check.py` keeps test-only constructs out of the flight software | ✅ |
| TST-02 | Integration tests shall verify complete flight sequences using recorded trajectory data | test_recorded_flight.c: test_TST_02_the_recorded_flight_runs_pad_to_landed, test_TST_02_launch_is_detected_as_the_rocket_leaves, test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon, test_TST_02_each_channel_fires_once_at_its_event and 1 more | ✅ |
| TST-03 | Closed-loop tests shall verify flight behavior with physics simulation feedback | test_mach.c: test_TST_03_plant_atmosphere, test_TST_03_plant_mach, test_TST_03_plant_port_error, test_TST_03_plant_charge and 3 more; the suite test_plant.c | ✅ |
| TST-04 | Closed-loop tests shall cover all four pyro firing modes, and the re-fire and emergency rules with... | test_flight_profiles.c: test_PYR_MODE_01_delay_after_apogee | ✅ |
| TST-05 | Closed-loop tests shall cover flights from 100 ft to above each sensor's height for proper operation, and... | test_flight_profiles.c: test_FLT_ASC_03_the_thrust_report_ends_within_a_second_of_burnout, test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height, test_SNS_MAX_01_best_effort_above_the_sensors_range | ✅ |
| TST-06 | Closed-loop tests shall verify that chute deployment reduces descent rate | test_flight_profiles.c: test_TST_06_a_canopy_slows_the_descent | ✅ |
| TST-07 | Web UI tests shall verify status display, configuration editing, and firmware update flows | Web UI: test_ui.spec.js | ✅ |
| TST-08 | All tests shall run in CI on every push to main | `.github/workflows/build.yml` | ✅ |
| TST-09 | Tests shall be traceable to requirements | Through CODE-02; This file, written by `support/trace_matrix.py`; `support/trace_check.py` in CI | ✅ |
| CODE-01 | Code structure and naming shall be preferred to comments | By review. No automated check | ⚠️ |
| CODE-02 | Comments shall carry requirement traceability | `support/trace_check.py`: every cited ID exists; `support/trace_matrix.py`: every row comes from a citation | ✅ |
| CODE-03 | Comments shall explain to a subject matter expert the implementation decisions that the code's structure... | By review. No automated check | ⚠️ |
| CODE-04 | A clearly named function shall be preferred to a magic value, however well the value is named | `support/wait_check.py` in CI: no sleep, a wait is a deadline function | ✅ |
| CODE-05 | Comments shall never teach a topic | By review. No automated check | ⚠️ |
| CODE-06 | Functional units shall be isolated in files by the single responsibility principle | By review: one responsibility a file in `src/flight_sources.txt`; `pmccabe` bounds a function in CI | ⚠️ |
| CODE-07 | The code shall be SOLID and DRY | By review. No automated check | ⚠️ |
| CODE-08 | Tests shall be black box and traceable to requirements | As TST-01 and TST-09 | ✅ |
| CODE-09 | All code shall be reachable through the published interfaces | Structure: `support/structure_check.py` | ✅ |
| CODE-10 | Pure functions with no internal state shall be preferred | By review. No automated check | ⚠️ |

## 17. What a Board Declares

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| BRD-01 | Each board shall declare, in its theory of operation | Through BRD-02; Each `boards/<name>/THEORY_OF_OPERATION.md`, "What this board declares"; `support/structure_check.py` holds every board to the list. Values marked not measured are owed to BRD-02 | ✅ |
| BRD-02 | A value a board declares shall be confirmed by that board's HAL validation (HAL-06) | Owed with HAL-06 | ⚠️ |

---

## Summary

| Status | Count |
|--------|-------|
| ✅ Verified by a host test, a web test or a structural check | 306 |
| ⚠️ Not verified, or not wholly | 25 |
| ❌ Not implemented | 0 |
| ✅ HW (verified on hardware only) | 12 |

`support/trace_check.py --counts` computes these, and CI fails when this table
disagrees or when `support/trace_matrix.py --check` finds the file out of date.
