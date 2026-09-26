# Full Code Review — 2026-09-24

## Status: RESOLVED 2026-09-25 — 27 findings: 3 critical, 6 high, 9 medium, 9 low. 22 fixed, 4 fixed in part, REV-13's threshold is a decision left open. See `code_review_2026-09-24_resolution.md`.

Reviewed at `lua-all-boards`, VERSION 2.1.631, HEAD `8eb5e22`.

Scope: the whole flight stack (`src/flight_states.c`, `pressure_processing.c`,
`buzzer.c`, `beep_codes.c`, `beep_store.c`, `config.c`, `ground_test.c`,
`telemetry_formatter.c`, `brownout.c`, `flash_window.c`, `pad_claim.c`,
`pyro_release.c`, `pin_assign.c`), the HTTP and web layer (`http_server.c`,
`www/`), the common HAL (`hal_common.c`, log and config paths), and the three
board pyro backends. The Lua subsystem (`src/lua/`, ~2400 lines) was read at
lower resolution and is not covered by the findings below.

Reviewed against `REQUIREMENTS.md`, `DECISIONS.md`, `.clang-format`, the
repo's comment rule (comments are hints for a subject-matter expert; history
belongs in git), and three operator narratives: a nominal dual-deploy flight,
a pad continuity failure, and a shredded drogue. The narratives are quoted in
the conformance table at the end.

## Method

Findings marked **proven** were reproduced by running code. The three host
suites all pass at the reviewed commit — 49 unit, 41 integration, 21
closed-loop — so every proven finding below is one the suite does not catch.
Reproduction steps are in the appendix.

## Summary

| ID | Severity | Finding | Requirement |
|----|----------|---------|-------------|
| REV-01 | Critical | Emergency ladder force-fires the main 2 s after the drogue on nominal flights (**proven**) | FLT-EMRG-01/02, PYR-MODE-02 |
| REV-02 | Critical | `pyro_mode=none` round-trips to `delay`; a disabled channel fires at apogee (**proven**) | CFG-04, PYR-SAFE-04 |
| REV-03 | Critical | MK1C cannot fire, but the log, CSV, telemetry and `/api/status` all record that it did | SYS-DEPLOY-01, DAT-04 |
| REV-04 | High | Pad faults are latched once at boot; a fault that appears later is never announced | PYR-CONT-01, BUZ-01, FLT-BOOT-15 |
| REV-05 | High | AGL triggers fire late by filter lag x descent rate (~56 m measured, ballistic) | PYR-MODE-02 |
| REV-06 | High | Ground-test FIRE bypasses the one-channel-at-a-time interlock | PYR-DEPLOY-02, GND-TEST-02 |
| REV-07 | High | Launch backdating is a no-op; the test that covers it cannot fail (**proven**) | FLT-LAUNCH-03 |
| REV-08 | High | A faulted board transmits state 0 = PAD_IDLE at 1 Hz (**proven**) | TEL-05, TEL-07, FLT-BOOT-12 |
| REV-09 | Medium | Flight time never freezes at landing | WEB-UI-04 |
| REV-10 | Medium | Flight Data tab caches the CSV for the page's life; one log slot; no erase | WEB-UI-04, DAT-06 |
| REV-11 | Medium | Launch sample logged at altitude 0 | GND-CAL-05 |
| REV-12 | Medium | Six config fields are inert, one of them a visible UI control (`beep_mode`) | CFG-02, SYS-CFG-04 |
| REV-13 | Medium | Launch threshold: code, comment, requirements and operator expectation all differ | FLT-LAUNCH-01/06 |
| REV-14 | Medium | `FLT-BOOT-11/12/13` each defined twice; TEL-07 stale | — |
| REV-15 | Low | Shipped default rocket name does not fit its field | CFG-07 |
| REV-16 | Medium | An emergency deployment leaves no trace anywhere | FLT-EMRG-01, DAT-04 |
| REV-17 | Medium | `GET /api/pins` with no `pins.ini` sends a malformed response | WEB-API-01 |
| REV-18 | Medium | PAD_IDLE interlock missing on `/api/reboot`, `/api/ota`, `/www/`, `/api/serial` | PYR-SAFE-04 |
| REV-19 | High | Changing units silently rescales both deployment altitudes | SYS-CFG-03, WEB-UI-02 |
| REV-20 | Low | Rocket name truncated to 8 characters with no hint | CFG-07, WEB-UI-02 |
| REV-21 | Low | "Save release" is one of three buttons writing two files | WEB-UI-02 |
| REV-22 | Low | Beep-table write failures reported as a digit-range error; load reason never returned | BUZ-CODE-10 |
| REV-23 | Low | `on_recv()` is 823 lines with the teardown idiom repeated 24 times | — |
| REV-24 | Low | "A firmware update wipes the filesystem" looks false | LUA-IO-01 |
| REV-25 | Low | ~20 comments narrate superseded code | comment rule |
| REV-26 | Low | `action_launch` carries two contradictory comment blocks | comment rule |
| REV-27 | Low | Dead code: the legacy buzzer path, 12 unused context fields, two CSV filenames | — |

---

## Critical

### REV-01 — The main deploys 2 s after the drogue on a nominal flight

`src/flight_states.c:712-742`.

`emergency_ladder()` escalates unless `canopy_working`, which is
`descent_settled()` — `DESC_DWELL_MS` (1200 ms, line 630) of a descent rate
stable inside a band. The grace before escalation is `EMRG_DROGUE_GRACE_MS`
(2000 ms, line 700), measured from the drogue command.

A canopy deployed at apogee starts from zero vertical speed and is still
accelerating toward its terminal rate when the grace expires. The tolerance
floor (`DESC_TOL_MIN_CMS`, 250 cm/s) breaks every ~0.26 s of free-fall
acceleration, so `desc_band_since` resets repeatedly and the dwell cannot
complete inside 2 s. "Working" is therefore unprovable in the window, and the
ladder fires pyro 2 over its configured trigger.

Measured with the closed-loop harness instrumented to report `ctx.main_forced`
at the P2 command, with a working drogue whose channel opens (`drogue_works`
and `p1_opens_on_fire` both true) and the main at 500 ft AGL:

| rocket | apogee | P1 | P2 | configured | forced |
|--------|--------|----|----|------------|--------|
| A8-3   | 190 ft | apogee | apogee | 500 ft AGL | no (apogee below trigger, PYR-DEPLOY-01) |
| C6-5   | 587 ft | apogee | 541 ft, +2000 ms | 500 ft AGL | **yes** |
| D12-5  | 1145 ft | apogee | 1100 ft, +2000 ms | 500 ft AGL | **yes** |
| H73-8  | 3930 ft | apogee | 3884 ft, +2000 ms | 500 ft AGL | **yes** |

Every escalation lands exactly 2000 ms after the drogue, i.e. on the grace
expiry rather than on any property of the descent. The repo's own `Dly+AGL`
suite shows the same behaviour today (`P2=21.1s@1152m` for an H73 with a
200 ft main) and passes, because no test asserts where the main fired when the
drogue worked.

This contradicts FLT-EMRG-01's own rationale — "a main opened high costs
drift" — by opening the main high on every flight that configures one by
altitude. It is also the operator's "long walk ... as it drifts down wind",
occurring on the nominal flight rather than the failure case.

Direction: escalate on evidence of failure, not on absence of evidence of
success. `band_exceeded(ctx, now, DESC_DROGUE_CMS)` (line 686) already
computes "descending faster than any canopy explains, held for
`DESC_FAIL_MS`", and that predicate cannot be true of a working drogue. If the
grace is kept as the trigger instead, it must exceed the time a canopy needs
to reach steady state — 5 s or more, not 2.

### REV-02 — A channel set to Disabled fires at apogee after one save

`src/config.c:26-39`. `mode_to_str()` has no `PYRO_MODE_NONE` case and its
`default` returns `"delay"`. `POST /api/config` merges the posted keys over the
running config and **re-serialises the result** (`src/http_server.c:490-493`),
so a disabled channel makes this trip:

```
pyro1_mode=none  ->  parse: NONE(0)  ->  serialize: "pyro1_mode=delay"  ->  reload: DELAY(4)
```

`pyro1_value` defaults to 0 and delay 0 means fire at apogee, so the channel
the operator disabled becomes a channel that fires at apogee.
`flight_config_reload()` applies it immediately; no reboot is needed to arm it.

Fix: add the `PYRO_MODE_NONE -> "none"` case and make `default` return
`"none"`. `parse_mode()` already returns `NONE` for anything it does not
recognise, so the round trip closes.

### REV-03 — MK1C records deployments it cannot perform

`boards/mk1c/pyro_board.c:783-789`: `pyro_fire()` emits
`!PYRO FIRE REFUSED: firing not implemented on MK1C` and returns.
`pyro_is_firing()` is hard `false` (line 837), so the PYR-DEPLOY-02 interlock
inside `fire_channel()` is inert on this board as well.

`fire_channel()` (`src/flight_states.c:112-127`) does not ask whether the
command reached hardware. It sets `pyro1_fired`, tags `EVT_PYRO1_FIRE` into
the flight log, and sends `$PYRO_FIRE`. The flight log, the CSV,
`/api/status` and the ground station therefore all report a deployment that
provably did not occur. Worse, `verify_window_open()` and
`check_post_fire_verify()` (`:154-176`) then read a channel that never opened,
set `pyro1_verify_fail`, and feed the retry branch of the ladder in REV-01.

The released-to-Lua path handles this honestly: `pyro_release.c` mocks the
channel and writes a `MOCK` row through `hal_log_mock()`. The
firing-not-implemented path writes nothing. Either give the board vtable an
answer the flight layer can check before claiming a fire, or log the refusal
the way a mocked fire is logged.

This is the board in all three operator narratives, so none of the three
flights can happen on it until the F0-F10 sequence exists.

---

## High

### REV-04 — A pad fault that appears after boot is never announced

`src/flight_states.c:419` returns before `collect_pad_faults()` once
`buzzer_started` is set, so `ctx->diag` and the announcement are computed once,
at the first continuity check after boot. Continuity itself keeps updating
every second — PYR-CONT-01 is met — but nothing acts on a change.

Two consequences:

- A rocket that sits on the pad for ten minutes with an igniter lead that
  lets go keeps chirping "OK to fly" for the rest of the countdown.
- After a fault is corrected without a power cycle, `/api/status` reports
  `pyro1_cont: true` next to `faults: ["pyro1_open"]` and
  `beep: "check_pyro_1"` — three fields describing two different instants.
  FLT-BOOT-15 asks for every fault found; it does not ask for them to be
  frozen.

Re-evaluating `diag` on each check and re-announcing on change fixes both, and
makes the pad-failure narrative work without the power cycle it currently
depends on.

### REV-05 — AGL triggers fire late by filter lag x descent rate

The pressure filter is a first-order IIR with tau = 500 ms
(`src/pressure_processing.c:131-144`), so during descent the filtered altitude
over-reads by roughly `rate x 0.5 s`. `should_fire_pyro()` compares that
filtered altitude against the AGL threshold.

Measured in the existing `AGL+AGL` suite: a 400 ft (122 m) trigger fired at
66 m true altitude, 56 m late, consistent with a ~100 m/s ballistic descent.

Under a drogue at 20 m/s the error is ~10 m and harmless. For a main that is
the only deployment — ballistic down to its trigger — a 500 ft main becomes a
~330 ft main. Either document the bias or compensate with the measured rate
(`altitude + speed x tau`) in the AGL comparison.

### REV-06 — Ground-test FIRE bypasses the shared-element interlock

`src/ground_test.c:106` and `:122` call `hal_pyro_fire()` directly. Every
flight-path fire goes through `fire_channel()`, whose stated purpose is the
`hal_pyro_is_firing()` refusal for PYR-DEPLOY-02: both igniters draw through
one common element, and on a PTC-protected board the combined draw can trip it
and fire neither.

`ARM 2` followed by `FIRE 2` can complete well inside MK1B's 500 ms
`FIRE_DURATION_MS`, energising both channels. The interlock belongs below both
callers.

### REV-07 — Launch backdating is a no-op (FLT-LAUNCH-03)

`src/flight_states.c:923-932`. Two inversions that cancel:

- the loop walks oldest to newest and breaks on the **first** sample at or
  below 50 cm, which on a pad is always the oldest sample in the buffer;
- the age term is `(buf_count - 1 - i)`, which is the age of the *newest*
  sample when `i` indexes from the newest.

Together they always yield `launch_time = now`. An isolated repro with a
buffer whose last ground sample is five samples old expects a 100 ms backdate
and produces 0 ms. Every logged time is late by the climb from 50 cm to 100 ft,
and the recorded flight duration is short by the same amount.

`test_FLT_LAUNCH_03_backdate` asserts only
`launch_time <= ascent_start_ms`, which zero backdating satisfies. The test
cannot fail; it needs to assert the interval.

### REV-08 — A faulted board transmits PAD_IDLE

`src/flight_states.c:1245` gates telemetry on `current_state >= PAD_IDLE`. The
enum appends `BOOT_SENSOR` (9) and `FAULT` (10) after `LANDED` — deliberately,
to keep the recorded state numbers stable (`flight_states.h:36-41`) — so both
exceed `PAD_IDLE` (3) and both transmit. `state_to_telem_id()` maps each to 0.

A board in terminal FAULT with `DIAG_SENSOR_FAIL` emits, at 1 Hz:

```
$PYRO,0,0,0,0,0,0,0,100000,00,0,0,0,0*09
```

State 0 is pad idle. `flight_ms` reads 100000 because `launch_time` is 0 and
the guard in `flight_update_outputs()` only special-cases `PAD_IDLE`. This
violates TEL-05 and actively reports an unflyable board as ready. Gate on a
state set rather than a numeric ordering; the same `>=` pattern appears in
`buf_add()` and `buf_tag_event()` and should be checked with it.

### REV-19 — Changing units silently rescales the deployment altitudes

`www/app.js:cfgChanged()` updates the unit label and the input's `max` when
units change, but not the value. Switching feet to meters turns a 500 ft main
into a 500 m main — a 3.3x increase — with no warning, and Save commits it.
SYS-CFG-03 asks for validation against sensor limits; this passes that check
while changing what the rocket does.

Convert on change, or refuse to save until the values are re-confirmed.

---

## Medium

### REV-09 — Flight time never freezes

`src/main_hardware.c:112` computes `now - launch_time` for as long as the board
is powered. The Flight Data tab read ten minutes after landing reports a
~600 s flight. Freeze it in `action_landing`.

### REV-10 — The Flight Data tab is stale, and cannot be cleared

Three causes behind one symptom:

- `www/app.js:284` — `if (flightLoaded) return;` caches the CSV for the life
  of the page. A flight that happens while the tab is open is never shown.
- The summary mixes sources. Duration and Apogee come from live
  `/api/status` (so REV-09 applies); the Pyro 1 and Pyro 2 rows are
  overwritten from the CSV by `updateFlightEvents()`. Two different flights can
  be on screen simultaneously.
- `hal_fs_open("flight_log.csv", false)` (`src/hal_common/hal_common.c:867`)
  opens `LFS_O_TRUNC`. There is exactly one log slot and the next launch
  destroys the previous flight without asking. There is no erase endpoint and
  no per-flight naming, though `docs/petit_fatfs_review.md` implies
  `flight_NNNN.csv` was once intended.

What the tab needs is a refresh, a header naming which flight is displayed,
and either numbered logs or an explicit erase.

### REV-11 — Launch sample logged at altitude 0

`src/flight_states.c:953` logs the launch sample as altitude `0` while the
comment eight lines above correctly states the rocket is ~100 ft up, and the
ring buffer records the real value. GND-CAL-05 asks for the height actually
reached. Pass `ctx->last_altitude`.

### REV-12 — Six inert configuration fields

`beep_mode` is written by the Config tab (`www/app.js:215`) and read nowhere in
the firmware — a visible control with no effect, and the operator has no way to
know. Also inert: `log_enabled` (logging cannot be disabled), `buzzer_startup`
(the startup announcement cannot be suppressed), `max_coast_s` (left over from
the backup apogee timer removed under DD-022), `telem_rate_hz`
(`telemetry_formatter.h:70` documents a cadence nothing reads; the rate is
hardcoded at `flight_states.c:1247`), and `log_rate_hz`.

Each is exercised by `test_config.c` round-trip tests, so the suite reports
them as working. Wire them up or drop them from `config_fields.h` and the UI.

### REV-13 — Four different launch thresholds

- `src/flight_states.c:452` — `LAUNCH_ALT_CM 3048` (100 ft);
- the comment block immediately above it — "Filtered altitude > 10m (1000cm)";
- `REQUIREMENTS.md` — FLT-LAUNCH-01 says 100 ft, FLT-LAUNCH-06 says 10 m
  within 2 s;
- the operator narrative — 50 ft, and "rising for more than 1 second", a dwell
  requirement that exists nowhere in the code.

Pick one and make the constant, the comment, the requirement and the
expectation agree.

### REV-14 — Requirement-document defects

`FLT-BOOT-11`, `FLT-BOOT-12` and `FLT-BOOT-13` are each defined twice in
`REQUIREMENTS.md` with unrelated meanings — the pad marker and brownout
recovery in one place, the power-up self-test in another. Traceability through
those IDs is ambiguous. TEL-07 still documents the four-state telemetry mapping
that `state_to_telem_id()` replaced with six.

### REV-16 — An emergency deployment leaves no trace

`ctx->main_forced` (`src/flight_states.c:741`) is written and never read: not
in the flight log, not on `/api/status`, not in telemetry. After the shredded-
drogue flight, nothing distinguishes a ladder-forced main from a configured
one. The same is true of `pyro1_refires`. Both belong on `/api/status` and in
the log, and REV-01 makes them the first thing worth looking at.

### REV-17 — `GET /api/pins` with no pins.ini sends a malformed response

`src/http_server.c:1288` passes `"No pins.ini"` as the streaming fallback,
which is written to the socket verbatim. Every other fallback in the file
begins with a status line. The client sees a protocol error rather than a 404.

### REV-18 — The PAD_IDLE interlock is applied inconsistently

`/api/config`, `/api/pins` and `/api/beeps/play` all refuse outside PAD_IDLE,
for a good stated reason: a browser must not talk over a launch.
`/api/reboot`, `/api/ota`, `/www/` uploads and `/api/serial` have no such
check. `POST /api/reboot` will reboot a flying board.

---

## Low

### REV-15 — The shipped default rocket name does not fit its field

`config_fields.h:27` ships `"My Rocket"` (9 characters) into a `char[9]`.
`config_set_defaults()` truncates it to `"My Rocke"`, and `www/app.js:186`
hardcodes `'My Rocke'` to match. Ship a name that fits.

### REV-20 — SKYSTREAK becomes SKYSTREA

`www/index.html:53` sets `maxlength="8"` with no visible hint, so a 9-character
rocket name loses its last letter as it is typed and the operator finds out on
the Status tab. CFG-07 is the constraint; the UI should state it.

### REV-21 — "Save release" is one of three buttons writing two files

The Config tab has two save buttons writing different stores — `Save` to
`config.ini`, `Save release` to `pins.ini` — and the Lua tab has a third,
`Save pin assignment`, writing the same `pins.ini`. Operator confusion here is
the design's. Label by destination, or fold the release controls into the one
Save.

### REV-22 — Beep-table errors carry the wrong class

`src/beep_store.c:81-86` returns `BEEP_ERR_DIGIT_RANGE` for both "table does
not fit beep.ini" and "could not write beep.ini". `apply_api_beeps` sends the
correct `v.what` but also `code: v.err`, so any consumer mapping that code
through `beep_codes_strerror()` reports "each beep count must be 1 to 9" for a
flash write failure. Separately, `beep_store_load()` returns at line 51 before
filling the caller's `reason` buffer, so "no beep.ini; shipped codes written"
never reaches an out-param caller.

### REV-23 — `on_recv()` is 823 lines

The `tcp_output` / `tcp_sent` / `tcp_arg` teardown appears 24 times and the
response-header idiom 33 times. That duplication is what let REV-17 hide. One
`respond(pcb, status, ctype, body)` helper and a route table would remove most
of it.

### REV-24 — "A firmware update wipes the filesystem" looks false

Asserted in `www/app.js`, `www/index.html:206`, and as the rationale for
LUA-IO-01. But `PFB_RESERVED_FILESYSTEM_SIZE_KB` reserves a block at the end of
flash, outside the A/B slots, and the only `lfs_format()` call is the
mount-failure fallback at `hal_common.c:537`. An OTA that keeps the flash
geometry should leave littlefs intact.

Export/import is still worth having — a failed mount does format, and a
geometry change moves the region — but the premise should be corrected rather
than repeated. Taken literally it also implies the entire `/www/` UI disappears
on every update, which would be a much larger problem than a lost script.
Worth confirming on hardware before relying on either statement.

### REV-25 — Comments that narrate superseded code

Against the repo rule that comments are hints for a subject-matter expert and
git holds the history. Roughly twenty instances; representative:

| Location | Narration |
|----------|-----------|
| `flight_states.c:50` | "csv_track_sample() removed: incremental logger retired" |
| `flight_states.c:184` | "check_refire() retired" |
| `flight_states.c:218`, `:280` | "what made an earlier version ..." |
| `flight_states.c:348` | "This used to force PAD_IDLE" |
| `flight_states.c:505` | "It used to be a 60-second IIR computed here" |
| `flight_states.c:601` | "The backup timer that used to force it here was removed" |
| `flight_states.c:927` | "the 10 ms loop period this assumed" |
| `flight_states.h:168`, `:226`, `:243` | "Kept after ... was removed", "used to rewrite fire_time", "retired" |
| `buzzer.c:76` | "fixing BUZ-01: chirps once" |
| `buzzer.h:24` | "64 was too small - caused silent truncation" |
| `pressure_processing.h:75` | "It used to be snapped to ..." |
| `http_server.c:839` | "used to report itself healthy here" |

Most carry a real forward constraint inside the anecdote. Keeping the
constraint and dropping the history shortens them and stops them going stale.

### REV-26 — Two contradictory comment blocks in `action_launch`

`src/flight_states.c:933-941`: a `[GND-CAL-02]` block describing a
snap-to-current-pressure the code no longer performs, immediately followed by
"Freeze the reference; do not snap it to here." A reader deciding whether an
edit is safe gets opposite answers from adjacent paragraphs. Delete the first.

### REV-27 — Dead code

- `build_code_pattern()`, `buzzer_play_code()` and the legacy shims
  (`buzzer.c:78-114`, `buzzer.h:91-97`) have no production caller; `beep_say()`
  replaced them. The comment at `buzzer.c:205` states the ten-chirp preamble
  "is gone" while that function still builds it.
- Never referenced at all in `flight_context_t`: `pyro_firing`,
  `pyro_fire_start`, `last_raw_pressure`, `filter_initialized`, `cal_count`,
  `cal_sum`, `pyro2_refires`. The last four duplicate state that
  `pressure_processing.c` owns.
- Written and never read: `csv_saved`, `landed_beep_started`, `armed_time`,
  `marker_ground_pa`, `main_forced`. Three of these carry comments claiming
  they are reported somewhere they are not (`flight_states.h:168`, `:183-196`).
- `flight_save_csv()` writes `flight.csv`; the server serves
  `flight_log.csv`.

---

## Test suite

All three suites pass at this commit. Three gaps let the findings above
through:

1. **No test asserts where the main fired when the drogue worked.** Every
   emergency-ladder test runs `drogue_works = false`; the mode suites assert
   only *that* both channels fired. A test asserting "with `drogue_works` and
   `p1_opens_on_fire` true, `main_forced` is false and P2's altitude is within
   tolerance of its trigger" fails today and is the regression guard for
   REV-01.
2. `test_FLT_LAUNCH_03_backdate` asserts a condition that zero backdating
   satisfies (REV-07).
3. `test_config.c` round-trips every field but never exercises
   `PYRO_MODE_NONE`, which is why REV-02 survives a 15-test config suite.

## Conformance against the operator narratives

| Narrative step | Verdict |
|----------------|---------|
| `pyro.local` resolves, status shows PAD_IDLE and 0.0 m | works (`net_glue.c:144,247`) |
| Name the rocket SKYSTREAK | truncated to SKYSTREA, silently (REV-20) |
| Units to feet | works; switching units later rescales the pyro values (REV-19) |
| "Beep Mode of Digits is OK" | control has no effect (REV-12) |
| Pyro 1 drogue, 0 s after apogee | works |
| Pyro 2 main, 500 ft AGL | **fires at ~700 ft on this flight** (REV-01), ~10 m late under drogue (REV-05) |
| "confused about the Save Release option" | three buttons, two stores (REV-21) |
| Flight Data tab shows stale data, no clear button | confirmed, three causes (REV-09, REV-10) |
| Beep personality default, repeat twice | works; firmware default is "until launch" (BUZ-02), not twice |
| Chirp on the pad, repeating every 5 s | works (`beep_codes.c:53-60`) |
| 5 beeps for a drogue-channel fault | works (BUZ-CODE-13); not re-announced if it appears later (REV-04) |
| Fix the e-fuse, power-cycle, chirp | works, but only because of the power cycle (REV-04) |
| Drogue at apogee, main at 500 ft, land, beep 8-2-4 in feet | beep-out correct; main altitude wrong (REV-01) |
| Overspeed with continued acceleration fires the main early | the one case the ladder gets right — and it currently fires on every flight, not just this one |
| All of the above, on an MK1C | no channel can fire (REV-03) |

## Suggested order

REV-02 first: a two-line change with a safety consequence. Then REV-01, which
needs a decision about which predicate replaces the grace timer, and the test
from gap 1 written first. Then REV-04, then REV-07 and REV-08 together since
both are state-comparison and index errors with cheap fixes.

## Appendix — reproducing the proven findings

The three suites, from a configured build directory:

```sh
cmake -B build -DPYRO_BOARD=mk1c
ninja -C build host_tests integration_tests closedloop_tests
```

The suites can also be built directly, which is how the probes below were
compiled (`$U` is `build/_deps/unity-src/src`):

```sh
gcc -w -Iboards/mk1c -Isrc -Itest -I$U \
    test/test_closedloop.c test/hal_test.c \
    src/pyro_release.c src/pad_claim.c src/brownout.c src/flight_states.c \
    src/beep_codes.c src/beep_store.c src/pressure_processing.c \
    src/ground_test.c src/config.c src/telemetry_formatter.c \
    $U/unity.c -lm -o /tmp/test_closedloop
```

**REV-01.** Copy `test/test_closedloop.c`, add `bool main_forced;` to
`sim_result_t`, capture `res.main_forced = ctx.main_forced;` where
`res.p2_fires == 1` in `run_sim_opts()`, print it from `print_summary()`, and
add a case running each rocket with
`{.enable_pyros = true, .drogue_works = true, .main_works = true, .p1_opens_on_fire = true}`
and `cfg_delay_agl()` with `pyro2_value = 500`. Three of four profiles report
the forced main. The unmodified `Dly+AGL` suite already shows the same
altitudes.

**REV-02.** Link `src/config.c` alone: `config_set_defaults()`,
`config_parse_ini("[pyro]\r\npyro1_mode=none\r\n")`,
`config_serialize_ini()`, then parse the output back. The second parse yields
mode 4.

**REV-07.** Replicate the loop at `flight_states.c:923-932` over a synthetic
64-entry buffer whose samples are `{0, 5, 20, 40, 300, 1200, 2400, 3100}` cm.
It matches index 0 and backdates 0 ms.

**REV-08.** Link the flight stack, set `ctx.current_state = FAULT` and
`ctx.diag = DIAG_SENSOR_FAIL`, call `telemetry_init(&ctx.config)` then
`flight_update_outputs(&ctx, 100000)`, and inspect `mock_uart_buf`.
