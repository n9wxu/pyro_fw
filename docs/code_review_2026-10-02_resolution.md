# Code review of 2026-10-02: resolution

What became of each finding in `docs/code_review_2026-10-02.md`. The review
read commit 3c9eb89; its fixes were written on the branch
`fix/code-review-2026-10-02` against that commit and never merged. Main moved
on (FreeRTOS, the SD card, the estimators, the pressure collector), so each
finding was checked against main at v2.2.4 and, where still open, fixed again
there with its test written first. The old branch is kept as a reference.

Status: **fixed** here; **was fixed** on main before this work; **n/a** the
code it described is gone; **open** with the reason.

## Critical, High and Medium

| ID | Finding | Status | Proof |
|---|---|---|---|
| CR-01 | A fire pulse stamped on one clock, checked on another | fixed | `test_board_pyro_mk1a.c`, `test_board_pyro_mk1b.c`, `test_board_pyro_mk1c.c`: the pulse and the precharge are timed from the loop's clock alone |
| CR-02 | Lua `flight.*` numbers did not match `flight_state_t` | fixed | `test_flight_state_constants_are_flight_state_t`, `test_a_script_acting_on_drogue_does_not_act_in_ascent` |
| CR-03 | The emergency ladder could fire a Disabled channel | was fixed | the ladder is gone; emergency fire acts on enabled channels only (`src/fire_control.c`) |
| CR-04 | A deploy altitude wrapped at 65 535 | fixed | `test_SYS_CFG_03_*` in `test_config.c`; the board answers 400 (`support/api_check.py`); three web tests under "Saving" |
| CR-05 | `POST /www/../config.ini` overwrote any file | fixed | `test_WEB_API_14_*`; `support/http_stream_check.py` on hardware |
| CR-06 | OTA had no size bound | fixed | `test_OTA_06_*`; 413 on hardware |
| CR-07 | A failed config read wrote defaults over the file | fixed | `test_FLT_BOOT_18_an_unreadable_configuration_is_a_fault_and_the_file_is_kept`, `test_config.c` (`config_from_file`) |
| CR-08 | `config.ini` parsing did not trim; truncated at 511 bytes | fixed | `test_config.c`, `test_pin_assign.c`, `test_beep_codes.c` through the one tokenizer (`src/ini_tokenizer.h`) |
| CR-09 | MS5607 second-order compensation missing | was fixed | `src/ms5607_driver.c`, `ms5607_tests` |
| CR-10 | MS5607 PROM CRC never checked | was fixed | `ms5607_tests` (AN520 vector) |
| CR-11 | The post-fire check read a pre-fire sample | fixed | three `PYR-VERIFY-01` tests in `test_fire_rules.c`; board tests for the mark kept until a fresh check |
| CR-12 | MK1C SDK SPI defaults on FIRE_A and BIAS_A | fixed | `src/sdk_default_pins.h` asserts at build time on MK1A, MK1B, MK1C |
| CR-13 | Wrong-board guard ran only after an OTA | fixed | `test_FLT_BOOT_17_*`, `board_selftest_tests` |
| CR-14 | USB RX stalled after one failed allocation | fixed | `test_WEB_NET_07_*` |
| CR-15 | Lua host calls outside `lua_pcall` reached `abort()` | fixed | `test_strict_globals_without_init_are_an_error_not_an_abort`, `test_a_full_arena_after_load_is_an_error_not_an_abort` |
| CR-16 | The Lua budget could be defeated | fixed | `test_pcall_in_a_loop_cannot_outlast_the_budget`, `test_gc_metamethods_are_refused`, `test_an_exponential_pattern_*` |
| CR-17 | Lua C stack too small for its call depth | fixed | `test_c_recursion_stops_at_the_configured_depth`, `test_check_does_not_use_the_system_heap`; 14,180 bytes of task stack free on MK1C with a script running |
| CR-18 | `LUA_32BITS` was a no-op | fixed | `test_numbers_are_32_bit`, `test_number_parsing_never_reaches_strtod`; images shrank about 12 kB |
| CR-19 | A failed OTA was reported as success | fixed | web test "a failed OTA says it failed and why"; the scripts check the answer |
| CR-20 | `install.py` preferred the MK1C image | fixed | by reading: the board is taken from `/api/status` or `--board` |
| CR-21 | Safety tests that could not fail | was fixed | those suites were replaced by the black-box flight suites |
| CR-22 | Any web page could POST to the board | fixed | `test_WEB_API_07_*`; 403 without `X-Pyro: 1` or with a foreign Host, on hardware |
| CR-23 | SD routes bypassed the filesystem lock | fixed | `test_WEB_API_08_*`, `test_HTTP_19_*` |
| CR-24 | TX MTU included the Ethernet header | fixed | `test_WEB_NET_07_a_frame_longer_than_the_endpoint_buffer_is_not_copied` |
| CR-25 | `/api/status` was not one snapshot | fixed | by reading; `api_check.py` saw no 503 through its flash writes |
| CR-26 | A stale valid mark with a partial OTA | fixed | by reading; OTA from v2.2.4 to this branch run on all four boards |
| CR-27 | A failed firmware commit was never retried | fixed | by reading (`src/storage_task.c`); an update survives a reboot on hardware |
| CR-28 | A log that cannot stop held the filesystem | fixed | by reading (`hal_common.c`: given up after 10 s, rows counted) |
| CR-29 | Mutexes made lazily after the scheduler starts | fixed | by reading (`hal_fs_mount()`) |
| CR-30 | Serial readline wedge and echo | n/a | the serial ground test is gone |
| CR-31 | Bus recovery drove the lines push-pull | fixed | `test_bringup_recovery_never_drives_a_line_high` |
| CR-32 | A second fire accepted mid-pulse; channel not range-checked | fixed | `test_board_pyro_mk1a.c`, `test_board_pyro_mk1b.c` |
| CR-33 | A scrubbed flight left a valid pad record | fixed | `test_FLT_BROWN_08_*` |
| CR-34 | `on_event()` never called on hardware | fixed | `test_LUA_RUN_02_*`; a bench flight on MK1C printed LAUNCH, ARMED, APOGEE, PYRO2, LANDING with none dropped |
| CR-35 | Lua PWM at 0.2 Hz, 39 % at most | fixed | `test_pwm_0_and_100_are_steady_and_duty_is_exact` |
| CR-36 | `pyro_bridge.pio` could not hold a level | fixed | by reading; a scope check is owed (section 5 of `outstanding_tasks.md`) |
| CR-37 | Simulator atmosphere wrong above 20 km | fixed | `physics_tests` against the published table |
| CR-38 | Simulator plant ignored pin direction | fixed | `plant_tests` |
| CR-39 | Bootloader fetched at `master`; actions pinned by tag | fixed | pinned by commit in `CMakeLists.txt` and the workflows |
| CR-40 | A local build bumped VERSION | fixed | a local build reports `+local` and leaves the file |

## Low findings

Fixed, each with the test or the reading named in its commit: the pad check
period (1000 ms); a state that is no state is FAULT; torn pressure-trace
reads; the beep-out above 999,999 and at the smallest integer; SD bus
ownership, CMD59 and the short write; the `lfs_format` result; the Lua lows
(program space checked, one writer of the core1 state, an unusable baud
refused, a failed publish undone, the arena growing in place, dead code
removed); the HTTP lows (411 for a POST with no length, a listen backlog, a
real random seed, the audition answer, the server's large locals); the web
page's escaping, polling and version; the scripts' `ping`, picotool path and
`ifconfig`; CI (formatting is a gate, least privilege, a concurrency guard,
no expressions in shell); `cmake_minimum_required`; the WASM build of every
variant; replay of a CRLF log; the stray files; the traceability checker's
gaps; the document errors.

Was fixed on main before this work: stale `under_thrust`, MK1B's check
period, the BMP280 range, NaN in the Mach clamp, the hard-coded I2C block,
`apply_api_config` with no context, review IDs in test names, the launch
gate comments, the flight source list copied into test targets, `beep.ini`
wrap.

Open, in `docs/outstanding_tasks.md`:

| Finding | Why it is open |
|---|---|
| A ground-test abort re-runs channel 1 | GND-TEST-10 as written restarts the procedure; needs a ruling |
| The resume settle and deadline against the sensor's bring-up time | DD-086 timing; needs a decision |
| `FF_FS_LOCK 0` | setting it broke four power-cut cases in `hr_log_tests`; needs a design |
| `arm_pump.c` on `pio0`, empty `pyro_sample()`, 30 ms chirps on a 20 ms tick | left by the reference branch too; no failure shown |
| The arena's block search is linear | bounded by the arena's size; not measured as a cost |
| Linear drag in the simulator; `api_shim.js` never loaded | simulator fidelity, not firmware |
| The comment pass and the file splits (review sections 3 and 3.5) | editorial; done only in files this work touched |
| DECISIONS has no status field; three status logs | editorial; the user's call |
| cppcheck cannot fail the build | labelled informational |
