# Code Review 2026-09-24 — Resolution

Response to `docs/code_review_2026-09-24.md`. Worked 2026-09-25 on
`lua-all-boards` from HEAD `8eb5e22`; nothing is committed yet.

## Status

| | |
|---|---|
| Review findings | 27: **22 fixed**, **4 fixed in part**, **1 resolved as a document fix with a decision still open** (REV-13) |
| New findings | 21 found while fixing: **12 fixed**, **9 documented** (N10 is moot, N18 and N19 were already known) |
| Requirement conflicts | 10 listed below: 5 resolved by a requirement edit (flagged for your review), 5 open |
| Host tests | 354, all passing (316 at HEAD; 14 of them for the USB addendum). Every new regression test was run against the old code first and failed |
| Web tests | 55, all passing in all three mock modes (3 of them for test mode). The 13 new ones were run against the old `www/` and failed |
| Hardware | MK1A 2.1.661, MK1B 2.1.662, MK1C 2.1.663, flashed by OTA; `support/api_check.py` 32/30/32 of 32/30/32, `test/web/hw_ui_check.js` all passing on each |
| USB addendum | Launch detection and announcements off while a PC is on USB, unless test mode is on; a charger cannot be detected on these boards. See the end of this document |

"Fixed" means the defect is corrected, a test that failed on HEAD now passes,
and where a bench board can show the behaviour, it was checked on all three
boards. Flight-only behaviour (the ladder, AGL timing, launch backdating) is
verified in the closed-loop and integration suites only; a bench board cannot
be flown.

## The findings

| ID | Sev | Status | Regression test(s) | Hardware |
|----|-----|--------|--------------------|----------|
| REV-01 | Crit | **Fixed** (requirement rewritten) | `test_REV01_working_drogue_main_at_its_trigger`, `test_REV01_failed_drogue_brings_the_main_forward`; every mode suite asserts `!main_forced` | flight only |
| REV-02 | Crit | **Fixed** | `test_config_mode_none_round_trips`, `test_config_unknown_mode_serialises_as_none`, updated `test_config_reload_normalises_invalid_pyro_mode` | ✅ all three: `none` survives the POST merge |
| REV-03 | Crit | **Fixed in part**: the flight no longer claims a fire; MK1C still cannot fire | `test_REV03_refused_fire_is_not_recorded_as_fired`, `test_REV03_refused_retry_is_asked_once` | ✅ `pyro1/2_refused` on /api/status |
| REV-04 | High | **Fixed** | `test_REV04_pad_fault_after_boot_is_announced`, `test_BUZ_ACT_04_new_outcome_silences_the_old_one` | ✅ diagnosis and `beep` follow a live change, no power cycle |
| REV-05 | High | **Fixed** for AGL and FALLEN | `test_REV05_agl_drogue_fires_at_its_altitude`; every AGL channel in the mode suites within 8 m | flight only |
| REV-06 | High | **Fixed** | `test_REV06_ground_test_waits_for_the_other_channel` | no serial adapter on the bench |
| REV-07 | High | **Fixed** | `test_REV07_launch_backdates_to_first_rise`, `test_FLT_LAUNCH_03_backdate` (now exact) | flight only |
| REV-08 | High | **Fixed** | `test_REV08_fault_sends_no_state_sentence` | no board in FAULT; UART not attached |
| REV-19 | High | **Fixed** | web: *changing units converts the pyro values* | ✅ UI on all three |
| REV-09 | Med | **Fixed** | `test_REV09_flight_time_freezes_at_landing` | flight only |
| REV-10 | Med | **Fixed in part**: refresh, one source, naming, erase; numbered logs not done | web: *appears on refresh*, *names the flight*, *can be erased*, *pyro events shown* | ✅ erase endpoint and UI on all three |
| REV-11 | Med | **Fixed** | `test_REV11_launch_row_reports_the_height_reached` | flight only |
| REV-12 | Med | **Fixed**: four keys removed, two wired | `test_config_writes_no_inert_keys`, `test_REV12_telem_rate_hz_*`, `test_REV12_log_rate_hz_*`; web: *no beep mode control* | ✅ no inert key written after a save |
| REV-13 | Med | **Documents agree; decision open** | existing `test_FLT_LAUNCH_08/09` | — |
| REV-14 | Med | **Fixed** | — | — |
| REV-16 | Med | **Fixed** | `test_REV16_forced_main_is_in_the_log` | ✅ `main_forced`, `pyro1_refires` on /api/status |
| REV-17 | Med | **Fixed** | by inspection | route not reachable: every board has a pins.ini |
| REV-18 | Med | **Fixed**; wiring verified by inspection only | `test_REV18_flight_in_progress_is_launch_to_landing` | a bench board cannot be put in flight |
| REV-15 | Low | **Fixed**, and a compile-time check added | `test_config_default_name_is_not_truncated`; web: *default rocket name* | — |
| REV-20 | Low | **Fixed** | web: *shows its 8-character limit* | ✅ |
| REV-21 | Low | **Fixed**: one Save on the Config tab | web: *one save button*, *buzzer can be moved* | ✅ |
| REV-22 | Low | **Fixed** | `test_BEEP_STORE_01/02` | — |
| REV-23 | Low | **Fixed in part**: framed responses; POSTs route through a table since the stream server (DD-039), GETs through a chain of comparisons | covered by the web and bench suites | ✅ every route: status, CORS, Content-Length |
| REV-24 | Low | **Confirmed false on hardware and corrected** | — | ✅ every littlefs file byte-identical across OTA, three boards |
| REV-25 | Low | **Fixed** | — | — |
| REV-26 | Low | **Fixed** | — | — |
| REV-27 | Low | **Fixed in part**: `flight_save_csv()` and `armed_time` kept | the buzzer suite rewritten against the live path | — |

### What changed, per finding

**REV-01.** The ladder now acts on evidence of failure (DD-028): the main is
brought forward only when, once the drogue has had its 2 s, the rocket descends
faster than 35 m/s *and is not being slowed*, for 1 s. The retry keeps
`pyro1_verify_fail` as its evidence. Closed-loop numbers, main set to 500 ft
(152 m):

| Flight | HEAD | Now |
|---|---|---|
| C6-5, drogue works | 159 m, forced 2 s after drogue | 153 m, not forced |
| D12-5, drogue works | forced | 153 m, not forced |
| H73-8, drogue works | forced | 153 m, not forced |
| H73-8, Dly+AGL 200 ft | 1152 m | 62 m (trigger 61 m) |
| H73-8, Spd+AGL 200 ft | 297 m, forced at 84 s when the drogue's rate drifted into the main band | 61 m |
| H73-8, drogue fails, charge lit | forced at +2 s | forced at +4.6 s, 1071 m |

Also fixed with it: after the retry the old ladder forced the main on the next
tick, with no grace for the retried drogue. FLT-EMRG-01's old wording required
the defect; see conflict C1.

**REV-02.** `config_mode_name()` writes `none` for NONE and for anything it
cannot name. It is now the one mode-name table: the flight log header, the
ring-buffer export and all three HALs use it (see N3).

**REV-03.** `flight_pyro_energise()` treats `hal_pyro_is_firing()` read
straight after `hal_pyro_fire()` as the board's acknowledgement (DD-032). A
refusal sets `pyroN_refused`, logs `PYROn_REFUSED` once, sends no `$PYRO_FIRE`,
and is not retried every tick. No HAL function was added; the semantics were
already true of every implementation except MK1C and are now written in
`hal.h` and `pyro.h`. **Not fixed:** MK1C still cannot fire, because the
F0–F10 sequence does not exist. That is hardware bring-up, not a review fix.

**REV-04.** Pad faults (`DIAG_PAD_ANY`) are re-derived at every continuity
check, and the announcement changes whenever the outcome does.
`buzzer_play_spec()` now silences a tone it interrupts. On the bench, disabling
channel 1 moved `faults` from `[pyro1_open, pyro2_open]` to `[pyro2_open]` and
`beep` from `check_pyro_1` to `check_pyro_2` within a second; restoring it
moved them back.

**REV-05.** AGL and FALLEN compare `altitude + speed × 500 ms` on the way down
(DD-029, PYR-MODE-05). The ballistic 400 ft drogue now fires at 121 m (HEAD:
66 m). SPEED and DELAY lags are not compensated (see "Not fixed").

**REV-06.** Ground-test FIRE goes through `flight_pyro_energise()`. A FIRE
while the other channel's pulse is running answers `GT,ERR,busy` and stays
armed. A refusal answers `GT,ERR,refused,N`.

**REV-07.** T+0 is `pad_rise_ms`, the timestamp of the first sample above 50 cm
since the last one at or below it. It is exact, not an index calculation, and
not limited by the 64-entry ring. OpenRocket profile: first rise 321 ms,
detection 2161 ms, `launch_time` now 321 (HEAD: 2161).

**REV-08.** Telemetry is gated on a state set, not `>= PAD_IDLE`. There is no
`$PYRO` in boot or FAULT; FAULT sends `!FAULT <diagnosis>` every 5 s (DD-031).
`buf_add()` and `buf_tag_event()` use the same set.

**REV-09.** `flight_elapsed_ms()` stops at `landing_time`. Telemetry and
`/api/status` both use it.

**REV-10.** The Flight Data tab reads the log every time it is shown, and on
↻ Refresh. It takes everything, duration and apogee included, from the log
alone, names the flight from the log header, and shows refusals and a forced
main. 🗑 Erase flight log calls the new `POST /api/flight/erase`, which is
refused while the log is being written. **Not done:** numbered logs (conflict
C6).

**REV-11.** The LAUNCH row is written at the detection time, with the
detection altitude. From the simulator's log of a 5000 ft flight:
`1040,100949,3117,4,0,LAUNCH`, 1.04 s after the backdated T+0 and 31 m up.
HEAD wrote `0,…,0`.

**REV-12.** Removed `beep_mode`, `max_coast_s`, `log_enabled` and
`buzzer_startup`; wiring those would have broken a requirement each (DD-030).
Wired `telem_rate_hz` (the in-flight cadence, 1–50 Hz, 0 meaning 10) and
`log_rate_hz` (thins samples, never events). Old files still parse, and the
removed keys disappear on the next save; verified on all three boards.

**REV-13.** The constant, its comment, FLT-LAUNCH-01/07 and DD-016 now agree
on 100 ft plus 5 m/s. FLT-LAUNCH-06 (10 m in 2 s), which nothing implemented,
is withdrawn. The operator narrative's 50 ft and ">1 s rising" is conflict C2,
for you to decide.

**REV-14.** The brownout requirements are now FLT-BROWN-01..03, so
FLT-BOOT-11..16 are the self-test ones again. TEL-07 has the six-state
mapping. Code and tests cite the new IDs.

**REV-15.** The default is `MyRocket`, and a `_Static_assert` generated from
the X-macro table stops any string default outgrowing its field.

**REV-16.** A `MAIN_FORCED` event goes in the log. `/api/status` carries
`main_forced`, `pyro1_refires` and `pyro1/2_refused`. The status page and the
Flight Data tab show both.

**REV-17/23.** `respond()` writes one framed response with CORS and a
Content-Length; `finish()` hands the pcb to `on_sent`. The teardown idiom went
from 25 copies to 2, the streaming EOF path and `finish()` itself. `/api/pins`
without a pins.ini now answers `404` with headers. Two latent bugs went with
it: the 201 and OTA-OK replies left the freed connection slot as the pcb's arg,
so `on_sent` could free a slot another connection had since been given; and
several error replies had no CORS header. The stream server (DD-039) later
replaced `respond()` and `finish()` with `http_conn.c`'s framing, and now
routes POSTs through `post_routes[]`; `on_recv()` only queues. GETs are still
a chain of comparisons.

**REV-18.** `refused_in_flight()` answers 409 to every POST, and to the bench
capture, from launch to landing (WEB-API-08, DD-034). That covers what the
review listed plus `/api/beeps`, `/api/lua/*` and `/api/flight/erase`. See
conflict C5.

**REV-19/20/21.** Changing units converts the pyro values; a delay, in seconds,
is left alone. The id and name fields count their characters against 8. The
Config tab has one Save, which writes config.ini and, if the release or buzzer
controls changed, pins.ini.

**REV-22.** New `BEEP_ERR_TOO_LARGE` and `BEEP_ERR_STORE`, each with its own
message. `beep_store_load()` fills the caller's reason on every path.

**REV-24.** OTA was run on all three boards and every littlefs file compared
before and after: config.ini, pins.ini, the Lua script and the three /www/
files were byte-identical every time. The UI, the test comments and LUA-IO-01's
rationale now say what does lose the filesystem: a failed mount formats it, and
a flash-geometry change moves it.

**REV-25/26.** The listed narrations, and the others found, are rewritten as
forward constraints or deleted. The contradictory `[GND-CAL-02]` block in
`action_launch` is gone.

**REV-27.** Deleted `build_code_pattern()`, `buzzer_play_code()`, the shims and
the code macros. The buzzer suite now tests `buzzer_play_spec()`, the path
production uses. Deleted from the context: `pyro_firing`, `pyro_fire_start`,
`last_raw_pressure`, `filter_initialized`, `cal_count`, `cal_sum`,
`pyro2_refires`, `csv_saved`, `landed_beep_started`, `marker_ground_pa`, and
four event codes nothing emitted. **Kept, contrary to the review:**
`flight_save_csv()` is the simulator's export (`sim_cli`, the WASM build), and
`armed_time` is read by `test_FLT_APO_04` as the ordering guarantee DD-022
keeps it for. `main_forced` is now read.

## Not fixed, and why

| Item | Why |
|---|---|
| **REV-03**: MK1C cannot fire | The F0–F10 firing sequence is unimplemented hardware bring-up, out of scope. The firmware now reports the refusal honestly. |
| **REV-05**: SPEED and DELAY lag | A SPEED trigger compares a filtered speed that also trails by about τ; DELAY counts from an apogee declared a little late. Neither is a height, and the review asked only about AGL. Compensating SPEED needs an acceleration estimate from a noisy two-point speed. |
| **REV-10**: numbered logs | Conflict C6: creating a new file at launch commits a directory entry, a flash write in the launch-shock window. |
| **REV-13**: which threshold | Conflict C2: yours to decide. |
| **REV-18**: in-flight refusal on hardware | The predicate is unit-tested, but a bench board cannot be put in flight to exercise the route. |
| **REV-23**: route table | Done for POSTs by the stream server (DD-039), which moved request handling out of the lwIP callbacks. GETs are still a chain of comparisons. |
| **REV-06 / REV-08** on hardware | No USB-serial adapter is on the TRRS jack, and no board is in FAULT. Host-verified only. |

## Requirement conflicts

**Resolved by editing a requirement.** Each is a change to what the firmware
promises, so please review:

- **C1. FLT-EMRG-01 against PYR-MODE-02.** "When the drogue has been commanded
  and the descent rate has not steadied under a canopy, deploy the main early"
  requires REV-01: a working drogue has not steadied 2 s after apogee. Now:
  forced only on overspeed without deceleration after a 2 s grace.
  FLT-EMRG-03 forbids the old reading and FLT-EMRG-04 requires the trace.
  PYR-REFIRE-01 is unchanged.
- **C3. FLT-BROWN-01 (formerly FLT-BOOT-11) against FLT-LOG-05, DD-008 and
  DD-027.** It said "shall write no flash during ascent", but the flight log
  writes during ascent by design once its RAM buffer has filled. It is now
  "so that nothing needs to be written at launch". Behaviour is unchanged.
- **C4. FLT-LAUNCH-06 against FLT-LAUNCH-01.** A 10 m-in-2 s gate that nothing
  implemented, beside a 100 ft trigger. Withdrawn; FLT-LAUNCH-07 now says
  100 ft.
- **C7. CFG-SUBSYS-01 against SYS-DATA-01, SYS-STATUS-01, BUZ-CODE-08 and
  DD-022.** Wiring `log_enabled`, `buzzer_startup` or `max_coast_s` would have
  let an operator turn off the flight record, silence the pad announcement or
  reinstate the removed ascent timer. They are removed instead, and
  CFG-SUBSYS-01 now names the parameters that exist.
- **C9. LUA-IO-01's rationale.** It asserted a wipe that OTA does not perform;
  corrected.

**Open.** These need a decision, not a code change:

- **C2. Launch detection: FLT-LAUNCH-01 against the operator narrative.** The
  code, comment and requirements say 100 ft plus 5 m/s on one sample, a value
  chosen in 48d47c0 after a bench false launch under the 10 m trigger. The
  narrative expects "rising for more than 1 second and clears 50 ft". A 1 s
  sustained-rise dwell would reject drift as well as the extra height does, and
  T+0 is backdated either way, so the choice costs nothing at launch. But it is
  a new detector, and the 100 ft value has a bench incident behind it.
- **C5. WEB-API-08 against recoverability.** With the in-flight interlock, a
  board stuck in a flight state cannot be rebooted or updated from the browser.
  It can get stuck through a false launch that never arms (DD-022 removed every
  ascent timeout), a sensor that dies in flight, or N7. While it is on USB,
  `picotool reboot -f` through the reset interface still recovers it (used on
  MK1C when N21 left it in ASCENT); otherwise it needs a power cycle. The MK1B and MK1C on the bench had been in ASCENT and FALLING
  for 11–13 hours on their old firmware when this work started. The firmware
  cannot tell them from a flying board, which is the point of the interlock.
- **C6. Numbered flight logs against FLT-BROWN-01 and DD-027.** Keeping more
  than one flight means creating a file at launch, and a new littlefs file
  commits a directory entry immediately. Alternatives: rename or pre-create on
  the pad, when the marker is written; or accept the write.
- **C8. FLT-LAND-07 against a main canopy's descent rate** (new finding N7).
  "60 s after apogee and slower than 5 m/s" is true under an ordinary main, so
  LANDED is declared in the air. Changing the 5 m/s figure, or requiring the
  normal stability test without the height check, is a requirement change.
- **C10. The narrative's timings against the evidence rule.** It expects the
  forced main about 3 s after a failed drogue; the evidence rule gives 4.6 s on
  the H73 profile. The review suggested a grace of 5 s or more. Shortening the
  grace or the 1 s hold trades early deployment on failure against forcing
  mains under slow drogues.

Not conflicts, but worth knowing: the narrative's "repeat twice" is a
personality setting the Beep Codes tab already offers (BUZ-02's default is
"until launch"). The narrative also wonders why cm is a unit; CFG-03 requires
it, and it stays.

## New findings

Found while fixing the above, most by running the code or the hardware.

| # | Finding | Status |
|---|---|---|
| N1 | **The pad marker was never written on hardware.** `write_pad_marker()` ran in the PAD_IDLE detector at STAGE 3, where the flash window is always shut, so `hal_fs_write_file()` refused the write every time without counting a refusal, and the marker was flagged written and never retried. Brownout recovery (FLT-BROWN, DD-026) could never engage. The host tests passed because the test HAL has no window. | **Fixed** (DD-033): `flight_flash_service()`, called inside the window. `test_BRN_INT_05` fails on HEAD. On hardware, `GET /pad.mkr` answered 404 on all three boards before and a 16-byte marker after, recording each board's live ground pressure to within 3 Pa. |
| N2 | **The flight log was not readable until landing.** Littlefs publishes a file's size only on sync or close, and the log closed only at LANDED. A flight that never lands recorded nothing readable: a crash, a battery popping on a hard landing, a sensor dying (DD-022 says that "costs the log", and it cost all of it), N7, or a board stuck as in C5. MK1C, 13 hours in FALLING, served a 0-byte flight_log.csv. | **Fixed** (DD-035, FLT-LOG-06): `lfs_file_sync()` once a second, inside the flash window, in a window with no flush write in it. Verified on MK1C with Lua running, using an instrumented bench build that logs 60 s at 50 Hz. Baseline: 0 bytes until the close. Synced: the file grows as written, and a reset 30 s in with no close keeps every row to 0.5 s before it, intact. `flash_refusals` 0, no dropped sample. Cost: erases 20 to 81, `stage_max_us[7]` 46 to 73 ms, overruns 20 to 77, Lua ticks about 4 % fewer. |
| N3 | The flight log header named AGL as "fallen" and FALLEN as "agl", in all three HALs (hardware, test, sim). The header is the only record of what was configured. | **Fixed**: one table, `config_mode_name()`; `test_REV_NEW_log_header_names_the_configured_modes`. |
| N4 | The Flight Data tab read the event from column 5, which on a real log is the thrust flag; the mock server's CSV had no thrust column, so the web tests passed. On hardware, PYRO1 and PYRO2 were never found. | **Fixed**: columns found by name, text rows skipped, mock serves the real format. |
| N5 | A channel set to Disabled was reported as an open igniter, so a single-deploy rocket said "check pyro 1" forever. | **Fixed**: FLT-BOOT-16 now covers disabled channels; unit test and bench. |
| N6 | The simulator never flew. `sim_flight_tick()` never fed the pressure layer, so calibration timed out into FAULT. The sim HAL fed pressure every tick (1 kHz), not at the sensor's 50 Hz, so speed was quantisation noise. The CMake `sim` target did not link brownout.c, pad_claim.c or pyro_release.c. The CLI ignited during boot. Affects the browser demo (SYS-PORT-02) too. | **Fixed**; the CLI now flies boot to landing. The WASM build is not rebuilt here (no emcc). |
| N7 | **The landing timeout declares LANDED under a main.** See C8. The simulator's own log of a 5000 ft flight shows `PYRO2` at 67 m and `LANDING` 0.7 s later at 61 m. It hits any flight whose main opens more than 60 s after apogee: a 5000 ft flight with a drogue, or any main the ladder forces high. | **Not fixed**: requirement change needed (C8). |
| N8 | The OTA success reply never reached the client. `pfb_perform_update()` armed a 1 ms watchdog and spun inside the lwIP callback. | **Fixed** with the stream server (DD-039): the OTA now reboots through `pending_reset` once its reply is with lwIP. On all three boards curl receives `200 OTA OK, rebooting...`, and the update takes 2.9–5.6 s instead of 28–81 s. |
| N9 | The ground reference locks out after a step of more than 50 Pa. GND-CAL-03 then rejects every later sample, so the reference freezes and weather drift accumulates against it. This happens when a board is powered at the prep table and carried to a pad about 4 m higher. | **Not fixed**: design decision. Suggest re-seeding when the deviation has persisted, stationary, for some seconds. |
| N10 | On the old firmware an OTA to MK1C stalled for 180 s against its active flight log (786 deferrals) and needed a reboot first. | Moot now: WEB-API-08 refuses OTA in flight, and C5 applies. |
| N11 | Text rows in the log (LUA, MOCK) carry uptime; sample rows carry flight time. One column, two clocks. | **Not fixed**: Lua subsystem, excluded from the review. `lua_app_service()` has the context, so it can pass `flight_elapsed_ms()`. |
| N12 | A canopy that approaches its terminal rate from below can settle in the main band, reporting CHUTE_DESCENT under a drogue (seen in the simulator). | **Not fixed**: phase report only; the triggers and the ladder's main rung do not read it. Documented in flight_states.md. |
| N13 | After the drogue retry, the ladder forced the main on the next tick. | **Fixed** with REV-01. |
| N14 | `DIAG_CFG_RANGE` flagged a disabled channel carrying a large value. | **Fixed**: altitude modes only. |
| N15 | The 201-Created and OTA-OK replies left a freed connection slot as the pcb's arg. | **Fixed** with REV-23. |
| N16 | `/api/beeps` (writes beep.ini) and `/api/lua/check` (compiles on core0) had no state check. | **Fixed** with REV-18. |
| N17 | `hal_fs_write_file()` and `hal_fs_open()` refused outside the window without counting it, so N1 never showed on `/api/status`. | **Fixed**: counted as `flash_refusals`, which `flash_window.h` says must stay 0. Every hardware caller runs at boot, from an HTTP handler holding the window, or in the window service. It reads 0 on all three boards. |
| N18 | LANDED's 1 Hz logging never fires (already defect 9 in flight_states.md). | Not fixed; pre-existing and documented. |
| N19 | `docs/app/` (the GitHub Pages copy of the UI) was last synced in March and predates most of the UI. | Not touched; `scripts/sync_demo.sh` regenerates it. |
| N20 | Every littlefs mount shares one static set of read, program and lookahead buffers (`littlefs_driver.c`, from the `LFS_NO_MALLOC` change). The flight log holds its mount from launch to landing, so any second mount meanwhile resets the shared caches the log's instance still believes it holds. A second *writer* also overwrites the lookahead the log allocates from. | **Not fixed.** Not observed: 20 reads of the growing log during a synced bench run left it intact, and WEB-API-08 now refuses every writer in flight. What remains is a file GET while the log is open, which needs a tethered board. Options: separate buffers for the log's mount (about 8 KB of RAM), or refuse file GETs while `hal_log_active()`. |
| N21 | **A pressure read could come back before its conversion finished, and a bench board false-launched.** The MS5607 read deadline was `now + 10 ms`, with `now` taken at the top of the loop, before STAGE 1's USB and lwIP work; the conversion needs up to 9.04 ms. Under HTTP load STAGE 1 ran 3–10 ms, the read returned 0, and a zero compensates to a large negative pressure. MK1C launched on the bench, its LAUNCH row at a filtered 95 290 Pa. The arming gate is what kept it from firing (the ascent never showed a positive speed); two bad reads in a row could get past it. | **Fixed** (DD-036, SNS-PRES-05/06): the read waits 9.1 ms from the command, timed in microseconds, and a zero reading or one outside 1–120 kPa is discarded and counted (`pres_waits`, `pres_rejects` on /api/status). Under the same HTTP load, with the timing unfixed and the gate in, MK1C logged 18 impossible readings in 60 s. With both in, MK1C had 0 in 120 s of heavier load, and MK1B 0. No deferral when idle. The earlier bench false launches blamed on drift (MK1B recorded 243 m) fit this cause better. |

## Hardware verification

Three boards on USB: MK1A (BMP280, no buzzer), MK1B (both channels released to
Lua), MK1C (Lua running). None had igniters connected.

1. **Before.** The boards ran 2.1.594–596, which predate the 100 ft trigger.
   MK1B was in ASCENT and MK1C in FALLING after bench false launches 11–13
   hours earlier. Every stored file was snapshotted.
2. **OTA.** All three were flashed over HTTP. On its old firmware, MK1B took the
   image while in ASCENT; MK1C's first attempt stalled against its flight log,
   was rebooted over HTTP out of FALLING, and then flashed. Both are REV-18 as
   the review describes it. Every littlefs file was byte-identical afterwards
   (REV-24).
3. **`support/api_check.py`**, all passing on each board:
   - status fields;
   - every route's status line, CORS header and Content-Length;
   - flight-log erase;
   - `none` surviving the merge;
   - no inert key after a save;
   - the diagnosis and announcement following a live change and being
     restored.
4. **`test/web/hw_ui_check.js`**, all passing on each board: no beep mode
   control, one Save, the name limit shown, unit conversion (300 m to 984 ft),
   the Flight Data tab reading the board's log, and the erase button.
5. **N1.** `pad.mkr` was absent on all three boards on the first new images and
   present on the final ones, holding each board's ground pressure.
6. **Health on the final image (MK1C):** Lua heartbeat climbing, 0 skipped
   dispatches, 0 loop overruns, 0 flash refusals. `support/prove_core0.py`
   passes for all three images.
7. **N2, flight log sync.** A temporary bench build, never committed, logged
   60 s at 50 Hz on MK1C with Lua running. It was run without the sync, with
   it, and with a reset mid-log.
8. **N21, pressure reads.** MK1C and MK1B were loaded with parallel 45 KB
   downloads while `pres_rejects` and `pres_waits` were watched: first with the
   gate but not the timing, then with both.
9. **Not checkable on the bench:** anything that needs flight, the ground-test
   serial link, a board in FAULT, or a board without pins.ini.

The flight logs erased during the checks held nothing. MK1A had none, and
MK1B's and MK1C's were the stale false flights, 0 bytes each (N2).

## Addendum: on USB

The request (2026-09-25): no flight detection and no beep codes while on USB,
whether the USB is a PC or a charger. One OK-on-USB double chirp on attach is
allowed.

**Done** (USB-01..05, USB-07, DD-037):
- **Detection.** The main loop judges a host attached while the SIE's
  start-of-frame number has moved in the last 100 ms. Only an awake host sends
  SOF, so every detection error leaves launch detection on.
- **While attached:**
  - no launch;
  - no brownout recovery;
  - no pad marker;
  - no pad verdict, system-failure or altitude beep-out.
- **Attach** plays two bursts of five 30 ms chirps, once.
- **Detach** resumes whatever the board would have said, and restarts the pad
  marker's 10 s dwell.
- **Launch to landing:** the flag is ignored.
- **Status:** `/api/status` gains `usb_attached`, `test_mode` and
  `buzzer_active`. `beep` still names the verdict, so the screen shows what the
  buzzer would say.
- **Test mode** (USB-08, DD-038), added at your direction: a board on USB
  flies as on battery. It is set from the Status tab after a confirmation, or
  with `POST /api/test_mode/on` and `/off`. It is held in RAM, so every boot
  starts with it off, and it cannot change in flight. Switching it off while
  attached chirps, as an attach does.

**Tests:**
- 9 host tests, each run against a mutant with its guard removed. All 9 fail
  there. The first version of test_USB_06 did not, and was strengthened.
- 5 more host tests and 3 web tests for test mode. The 3 mutants (flag
  ignored, changeable in flight, no transition on toggle) each fail at least
  one of them.
- On MK1A, MK1B and MK1C:
  - `usb_attached` is true and `buzzer_active` false. MK1B had been
    repeating ok_to_fly.
  - In test mode each board announced on USB and wrote `pad.mkr`. Out of test
    mode it chirped once and went quiet. After a reboot, test mode was off.
  - No flash program or erase after 50–90 s of PAD_IDLE. The old image wrote
    the marker at 10 s.
  - `api_check.py` passes, as does the UI check.
  - MK1C's Lua heartbeat is climbing, with 0 skipped and 0 flash refusals.

**Not fixed, and why:**

| # | Item | Why |
|---|---|---|
| U1 | **A charger is not detected** (USB-06). | Hardware. On all three boards VBUS reaches only the TP4057 charger IC, and ~CHRG and ~STDBY drive LEDs only. No RP2040 pin can see that a charger is present. On a charger the board behaves as on battery: it announces and detects launches. Fix options (DD-037): a VBUS divider to a spare GPIO (MK1C GPIO2-5/9/10/13-15; GPIO24 on MK1A/B), or ~CHRG/~STDBY to two GPIOs. Firmware would OR either with SOF. Reading the data lines was rejected: a floating D- can mimic a charger's D+/D- short, which would disable deployment in flight. |
| U2 | **Detach not tried on hardware.** | Nobody unplugged a board. The chirp, and resuming on unplug, are verified by host tests only. It is a 10-second check by ear on MK1B or MK1C. |

**Requirement conflicts, for your review:**

- **U3. A sleeping PC reads as detached.** A suspended bus sends no SOF, so it
  looks the same as no cable. While the host sleeps, the board announces its
  verdict and would detect a launch. It chirps again when the host wakes. This
  is the safe direction. Holding "attached" through a suspend would need
  VBUS (U1).
- **U4. BUZ-02 against USB-02.** BUZ-02 repeats the announcement so that
  silence means a fault. On USB, silence is by design. The chirp says the
  board is alive, and `/api/status` gives the verdict.
- **U5. FLT-BOOT-12/14 against USB-02.** A board that fails its self-test
  while on USB is silent. The fault is on `/api/status` and the `!FAULT`
  UART line, and the announcement starts on unplug.
- **U6. Operator-requested sounds still play:** the Beep Codes tab's audition
  (`POST /api/beeps/play`), and the ground-test BEEP commands on the serial
  jack. The web UI is reachable only over USB, so silencing the audition would
  remove the feature. If "no beep codes" is meant to include these, it is one
  guard each.
- **U7. Bench tests that need launch detection with a PC attached.**
  Resolved by test mode (USB-08). One limit remains: brownout recovery cannot
  be tested on USB, because the reset clears test mode (DD-038).

**Found on the way:**

| # | Finding | Status |
|---|---|---|
| N22 | `POST /api/beeps/play` read its body from the first TCP segment only. A client that sent the body separately, as Python's urllib does, got a 400 ("each beep count must be 1 to 9") for a valid request. It was one symptom of a server that parsed segments rather than a stream (DD-039). | **Fixed** with the stream server: the audition with its body in a later write answers 200 on hardware. |

## Addendum: the pressure chain

`docs/pressure-filter-prompt.md` was reviewed against the code on
2026-09-25. The comparison used host experiments that run the real pressure
layer and flight states with datasheet sensor noise (MS5607, 1.2 Pa RMS);
no repo test models noise. Two findings are bugs. The work is planned, not
started, in `docs/outstanding_tasks.md` (section 4).

| # | Finding | Status |
|---|---|---|
| N23 | **Brownout recovery never engages on hardware.** `assess_recovery()` asks the pressure layer for samples in BOOT_SENSOR, but the layer only starts at BOOT_CALIBRATE, so recovery always waits out its 4 s deadline and boots cold. The level it compares also reads a filtered value that is still zero. The integration tests prime the pressure layer before booting, which hides it. Measured: 600 m up and descending with a valid marker rejoins the flight when primed, and boots cold when booted as the hardware boots. | **Fixed** (T1, DD-041), with two gaps behind it: a rejoined flight never started the pressure layer, and never read continuity, so it would have deployed nothing. |
| N24 | **One plausible glitch declares a launch.** A single reading 12 kPa or more low passes the 1–120 kPa range check (DD-036). There is no spike rejection, and launch fires on one sample. The arming gate then stops any pyro firing, but the board sits in ASCENT until power-cycled: flight log open, the in-flight lock refusing reboot and OTA, and the pad announcement stopped. A glitch during coast can declare apogee the same way. | **Fixed in part** (T2, DD-040): a median of three stops a single reading. Two in a row still reach the detectors until T3. |

Also measured, and planned in the same document:
- speed noise on the pad is 8× what the filter should give, because its
  whole-pascal state is forced to step (SNS-PRES-04);
- landing takes 18–72 s after touchdown;
- apogee is detected 0.56 s late;
- the frozen ground pressure reads 0.2–0.5 m low.

**Recorded, not scheduled:** a false-launch reversion, which would return a
board that was never armed, and is back on the ground, from ASCENT to
PAD_IDLE. At your decision; the case for it is in `docs/outstanding_tasks.md`,
section 9.

