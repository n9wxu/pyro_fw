# Outstanding tasks

Everything still open on `lua-all-boards` as of 2026-09-26, in one place.
Items keep the IDs they were given where they were found, so each one can be
traced back:
- **C**, **N1–N24**, **REV** and **U** items:
  `docs/code_review_2026-09-24_resolution.md`.
- **N25** and **N26**: found reviewing this list on 2026-09-26; section 3.
- **T** and **M** items: section 4. The M tasks implement
  `docs/mach-lockout-prompt.md`.
- **H** items: section 8.

Tick an item off here when it lands, and mark it fixed where it was
recorded.

| # | Section | Needs |
|---|---|---|
| — | [How every task runs](#how-every-task-runs) | read first |
| 1 | [Commit the work](#1-commit-the-work) | done, except G4 |
| 2 | [Decisions](#2-decisions) | you |
| 3 | [Safety fixes](#3-safety-fixes) | code |
| 4 | [The pressure chain and the Mach lockout](#4-the-pressure-chain-and-the-mach-lockout) | code |
| 5 | [Other code defects](#5-other-code-defects) | code, some decisions |
| 6 | [Bench checks](#6-bench-checks) | a person or equipment |
| 7 | [Board changes](#7-board-changes) | hardware design |
| 8 | [Documentation and housekeeping](#8-documentation-and-housekeeping) | code/docs |
| 9 | [Deferred, and not planned](#9-deferred-and-not-planned) | nothing yet |

---

## How every task runs

Every task follows the same steps. No task closes any other way.

1. **Tests first.** Write the tests the task lists, and run them against the
   code as it stands.
   - Each must **fail**, except those marked *guard*.
   - A *guard* test protects behaviour the task must not change. It must pass
     before and after.
   - A new test that passes before the change proves nothing. Strengthen it
     until it fails, as test_USB_06 was.
2. **Change** the code, and the records in the same change:
   - `REQUIREMENTS.md`: requirements added, amended or withdrawn;
   - `DECISIONS.md`: a DD entry for each design choice (the next free number
     is DD-040);
   - `TRACEABILITY.md`: each new or amended requirement linked to its tests;
   - `docs/flight_states.md`, where a state's behaviour changes.
3. **Pass.** Every test the task lists passes, and so does the gate.
4. **Commit and push.** The task is complete when its commit is pushed.

Everything is complete when CI passes on the last push.

Bench tests are tests too: their pass criteria are written here before the
bench run. Checks that need a person or equipment (a chamber, a serial
adapter, a dummy load) are in section 6, so a task can close on the host
suites and the bench boards.

### The gate

Run at the end of every task that changes firmware. A task that changes only
documents runs G5.

| | What | Pass |
|---|---|---|
| G1 | Every host suite CI runs (`.github/workflows/build.yml`), plus `pressure_chain_tests` once T0 lands and `mach_tests` once M0 lands | all pass |
| G2 | `test/web/run_web_tests.sh`, all three mock modes | all pass |
| G3 | Build MK1A, MK1B, MK1C and the reference template; `support/prove_core0.py` on each image | builds; the proof passes |
| G4 | OTA-flash MK1A, MK1B and MK1C; run `support/api_check.py`, `support/http_stream_check.py` and `test/web/hw_ui_check.js` on each | all pass; on MK1C with Lua running, the heartbeat climbs, with 0 loop overruns and 0 flash refusals |
| G5 | `support/trace_check.py` (H1) | passes |

Every flash write runs in core0's STAGE 7 window. G4's 0 flash refusals on
MK1C with Lua running is what checks it.

**G4 is pending.** On 2026-09-26 the session's permission rules refused the
OTA flash as a production deploy. Until you allow it, tasks close on G1, G2,
G3 and G5, and each task's G4 and bench tests are listed as owed here.

---

## 1. Commit the work

**Done 2026-09-26:** e8de13c and 44d3735, pushed; CI passed. G1–G3 passed
from a clean worktree. G4 has not run: flashing the bench boards needs your
permission (see [The gate](#the-gate)).

Nothing since 2026-09-24 is committed. The working tree holds:
- the 2026-09-24 code review fixes, with their tests and docs;
- the USB policy (USB-01..07) and test mode (USB-08);
- the stream HTTP server (DD-039): `net_ring.c`, `http_conn.c`, the
  rewritten `http_server.c`, `http_tests` and `support/http_stream_check.py`;
- five untracked documents: the review, its resolution, this list, and the
  two prompts it plans from (`docs/pressure-filter-prompt.md`,
  `docs/mach-lockout-prompt.md`).

**Two commits, not one per body of work.** The stream server rewrote
`http_server.c` after the review fixes and test mode had changed it, and
`respond()` and `finish()` no longer exist. A review-only commit would need a
version of that file that is gone, and it would build and test code that
never ran.
1. The code, its tests, and the records that describe it (`REQUIREMENTS.md`,
   `DECISIONS.md`, `TRACEABILITY.md`, `docs/flight_states.md`,
   `test/README.md`).
2. The five documents.

**Tests first:** from a clean worktree of each commit, G1, G2 and G3 pass. G4
passes on images built from the first commit.

---

## 2. Decisions

Each decision blocks a task, or changes what the firmware promises. On
2026-09-26 you said to execute every task. Where a decision had a
recommendation, that recommendation is **adopted**. Each one is a constant or
a requirement, so it can be changed later. Decisions with no recommendation
are still yours; their tasks wait.

| ID | Question | Status | Blocks |
|---|---|---|---|
| C2 | The launch trigger | **Open, not blocking.** The options: (a) today's 100 ft and 5 m/s; (b) the operator narrative's "rising for more than 1 s and past 50 ft"; (c) the pressure-filter prompt's 15 m held for 8 samples with a positive speed; (d) the Mach prompt's acceleration trigger, fitted p̈ < −0.0025·p held for 50–100 ms. (d) misses launches under 2.1–2.4 g net, a thrust-to-weight below about 3.1–3.4, so it needs a height trigger behind it. The 100 ft figure came from bench false launches now blamed on N21. T3 keeps today's trigger and adds a hold, which (a), (b) and (c) all need. Choosing one later means changing constants. | — |
| C5 | In-flight lock against recoverability | **Open.** A board stuck in a flight state can't be rebooted or updated from the browser. picotool over USB, or a power cycle, recovers it. The false-launch reversion (section 9) would cover the false-launch case. | — |
| C6 | Numbered flight logs | **Open.** Keeping more than one flight means creating a file at launch, which is a flash write during launch shock. The alternatives are to create or rename it on the pad when the marker is written, or to accept the write. If taken, do it with T8's log-format change. | C6 task |
| C8 / N7 | The landing timeout declares LANDED under a main | **Adopted:** once T5 makes the stillness test reliable, the timeout needs stillness instead of "slower than 5 m/s". | N7 task |
| C10 | Forced main after a failed drogue: 4.6 s today, the narrative about 3 s | **Open.** Shortening the grace or the 1 s hold trades early deployment on a real failure against forcing mains under slow drogues. | C10 task |
| U3 | A sleeping PC counts as unplugged | **Open.** Holding "attached" through a sleep needs VBUS sensing (U1). | — |
| U4 / U5 | Silence on USB against BUZ-02 and FLT-BOOT-12/14 | **Open.** Accept the silence, or amend those requirements. | — |
| U6 | The audition button and ground-test BEEP still sound on USB | **Open.** Silencing them is one check each. The web UI only works over USB, so silencing the audition removes the feature. | U6 task |
| T3 | How long launch and apogee must hold | **Adopted:** 100 ms and 60 ms. | T3 |
| T5-W | The fit's window | **Adopted: 1.0 s.** At 50 Hz it gives 0.19 m/s of speed noise on the pad and 4.4 Pa/s² of p̈ noise. The Mach prompt's example of 0.5 s gives 23.8 Pa/s² at 50 Hz (17.5 at 100 Hz). At 9 km, with no drag, 1 g of deceleration would then sit only 0.7σ (1.0σ at 100 Hz) above the release threshold, and the release would keep resetting. The cost of 1 s: a Mach step or an ejection keeps fits unclean for 1 s instead of 0.5 s, and so does the ignition step, which M1 handles by setting the flag on any fit. | T5 |
| T5-A | How far below the peak apogee needs | **Adopted: p ≥ 1.0001·p_min**, 0.6–0.9 m, about +0.4 s. The Mach prompt's 1.0005 means 3–5 m and puts the drogue 0.8–1.0 s after apogee; today it is +0.56 s. The fit's pressure noise is about 0.5 Pa. At 9 km, 1.0001 is 3 Pa, so noise cannot fake the drop, and an early apogee becomes impossible. | T5 |
| T6 | How long the ground tracker waits before re-seeding | **Adopted:** 5 s. The pressure-filter prompt's 30 s leaves the pad reference wrong for half a minute after the rocket is set down. | T6 |
| T8 | Flight logging rate | **Open, not blocking.** Full rate makes flights replayable, at a cost in flash wear. T8 adds the columns and the replay tool at today's default rate. | — |
| M1-D | Accept M1's deviations from the Mach prompt | **Adopted.** They are listed in M1. The largest: the fit is solved against each sample's own time, in floating point, not with precomputed integer coefficients. Flash stalls make the sample spacing uneven (T11), and precomputed coefficients assume even spacing. | M1 |
| N20 | Shared littlefs buffers | **Adopted:** refuse file GETs while the flight log is open. It can be tested on the host, the log can be read after landing, and WEB-API-08 already refuses every writer in flight. | N20 task |

---

## 3. Safety fixes

Each is scheduled in section 4, and listed here so none is missed.

| ID | What | Fixed by |
|---|---|---|
| N23 | **Fixed (T1).** Brownout recovery never engages on hardware. It asks for samples before the pressure layer starts, so it always boots cold. The tests hide this by priming the layer. | T1 |
| N24 | **Fixed (T2, T3).** A single plausible glitch, 12 kPa or more low, declares a launch. The board then sits in ASCENT until power-cycled, and a glitch in coast can declare apogee. | T2, T3 |
| N25 | **Fixed (T2, T1).** **The pad marker outlives its flight, and T1 would make that dangerous.** `pad.mkr` is never deleted, and every power-on reset counts as a power event. So every battery power-up with any old marker runs recovery. The operator narrative powers the board twice per flight with its charges connected: on the bench, then on the pad. Today N23 hides the problem. Once T1 feeds recovery samples, a single glitch in its history reads as about 80 m up and climbing, and the verdict is "recovered in ascent", on the pad. If the averaging window reaches into the older half of the slope's window, a glitch there reads as high and falling: "recovered in descent", which arms the pyros at once. | T2 before T1; T1's glitch tests; T1 removes the marker at landing |
| N26 | **Fixed (T3).** **Above 8 km AGL the altitude clamp reads as a stopped rocket.** SNS-ALT-02 clamps altitude at 8000 m, and speed comes from the clamped altitude. Past 8 km the speed reads zero, so the pyros arm, the Mach gate clears after 1 s, and apogee fires while the rocket is still climbing. Supersonic flights are the ones that go this high. | T5: speed from the pressure fit, never from a clamped altitude |
| — | **A stuck sensor in coast may fire the drogue** (suspected, not yet shown). The filter settles onto a stuck value, speed reaches zero, and apogee fires if the pyros are armed. `test_M2_stuck_in_coast` shows whether it does. | M2 |

---

## 4. The pressure chain and the Mach lockout

Planned from reviews of `docs/pressure-filter-prompt.md` and
`docs/mach-lockout-prompt.md` against the code. The measurements come from host
experiments that run the real `pressure_processing.c` and `flight_states.c`
with datasheet sensor noise (MS5607 at OSR 4096: 1.2 Pa RMS, truncated to
whole pascals as `hal_common.c` does). No repo test modelled noise or flew
faster than an H73, which is why none of this showed up there. Since T0, the
"Now" column is what `pressure_chain_tests` prints; the earlier experiments
agreed with it except for touchdown, where they had not included the launch's
ground bias, and the stall figure, where they used a different stall model.

### Baseline

| Measure | Now | Target | Task |
|---|---|---|---|
| Brownout recovery, booted as the hardware boots | never engages (N23) | engages | T1 |
| Recovery on the pad with one glitch in its history | unreachable today (N23) | never resumes | T1 |
| One plausible glitch on the pad | ≥11 kPa low declares a launch (N24) | no launch at any size | T2, T3 |
| Speed noise on the pad (RMS) | 1.58 m/s | ≤ 0.3 m/s: done, 0.22 m/s | T4, T5 |
| Touchdown to LANDED, by the stillness test | on the pad's level 1.9 s, because the zero clamp hides the noise there; 5 m above it, never within 400 s, so only the 60 s timeout lands it | ≤ 3 s, at any landing height: done, 1.6 s | T4, T5 |
| Apogee after the true apogee | +0.56 s mean (+0.54 to +0.58) | never early; about +0.4 s | T5 |
| Ground pressure frozen at launch | reads 0.17 m (30 g) to 0.42 m (2 g) low | ≤ 0.1 m: done, 0.08 m | T7 |
| Ground tracker after a >50 Pa step | frozen for good (N9) | re-seeds: done, within 5 s | T6 |
| Sample timestamps | loop ms at the temperature read, 16 ms after the conversion, up to 127 ms after it through a stall; stalls raise the worst speed error at 100 m/s from 8.3 to 9.3 m/s | hardware timer at the pressure conversion: done (through stalls 6.5 m/s against 6.1) | T11 |
| Fit-based speed and acceleration through flash stalls | worst −294 m/s and 441 m/s² with today's stamps (earlier experiments; T11 re-measures) | RMS 1.2 m/s and 3.5 m/s², stalls or not | T11 |
| A flight above 8 km AGL | apogee fired 14.9 s early, at the clamp (N26) | apogee at the real apogee: done | T3 |
| Supersonic flight with a static-port error | a port error that makes the boost read as a descent fires the low-drag flight's drogue 39 s before apogee, at Mach 1.27 | no drogue before the true apogee, on every M0 profile | M1 |
| A sensor stuck or lost in flight | a stuck value may read as apogee | never causes a deployment | M2 |
| Real flights replayable offline | no (raw pressure not logged) | yes | T8 |
| MS5607 sample rate | 50 Hz | ~90 Hz | T9 |

### Order

```
H1  traceability check (section 8): first, so G5 exists for every task

T0 ─┬─ T2 ── T1
    ├─ T3
    ├─ T7
    ├─ M0
    └─ T11 ─┬─ T4 ── T6
            ├─ T8
            └─ T5 ─┬─ M1        T5 also needs T2, T3 and M0
                   └─ M2
T9   after T5 and M1
T10  last
```

| Task | What | Needs first | Size |
|---|---|---|---|
| T0 | Noise and stall test support | — | M |
| T2 | Median of 3 | T0 | S |
| T1 | Recovery sees samples, can't be fooled on the pad, marker removed at landing | T2 | M |
| T3 | Triggers held for a duration | T0 | S |
| T7 | Ground pressure from before the rise | T0 | S |
| M0 | Supersonic and failure test support | T0 | M |
| T11 | True sample times | T0 | M |
| T4 | Fractional filter | T11 | S–M |
| T6 | Ground re-seed | T4 | S |
| T8 | Raw logging and replay | T11 | M |
| T5 | One estimator: the fit | T2, T3, T11, M0 | L |
| M1 | The Mach lockout | T5 | L |
| M2 | Sensor failure in flight | T5 | S–M |
| T9 | Temperature cadence | T5, M1 | M |
| T10 | Documentation sweep | all | S |

T2 comes before T1 because of N25. T11 comes before T4, T5 and T8, because an
estimator is only as good as the time attached to each sample, and logs
recorded before T11 carry the wrong times. T5 is the riskiest task: every speed
threshold was tuned against today's estimator. M1 replaces the Mach gate, which
is safe only once T5's fit and M0's supersonic profiles exist.

---

### T0. Noise and stall test support, and a real noise baseline

**Done 2026-09-26**, except its bench test: `noise_baseline.py` needs firmware
with the new status fields on the boards (G4).

**Why:** every finding here was invisible, because the test HAL feeds a
perfectly smooth signal, primes the pressure layer before booting, and never
stalls.

**Tests first.** This task builds test support, so its tests check the
support, and then record today's numbers:
- `test_T0_noise_model`: over 10 000 readings, the mock sensor's noise has the
  configured RMS within 2 %, in whole pascals, and repeats for a given seed.
- `test_T0_stall_model`: stalls last 40–73 ms and come about 1.3 times a
  second (DD-035). A conversion commanded before a stall is read after it, as
  on the hardware.
- *guard* `test_T0_off_by_default`: with both models off, every existing
  suite's results are unchanged.
- `test_T0_unprimed_boot`: a helper boots the way the hardware does, without
  `pp_test_prime()`, and reaches PAD_IDLE.
- `api_check.py`: `/api/status` carries `pad_speed_cms` and `raw_pa`.
- A new suite, `test/test_pressure_chain.c` (target `pressure_chain_tests`,
  added to CI), holding the scenarios: noise on the pad, touchdown, coast to
  apogee, a main at 5 m/s, launches at 2, 5, 15 and 30 g, and glitch sweeps.
  Each prints the metric its baseline row names. Later tasks turn these into
  assertions.
- Bench: `support/noise_baseline.py` samples `raw_pa` on each board for 60 s
  and reports its RMS (MS5607 on MK1B and MK1C, BMP280 on MK1A).

**Change:** the noise and stall models in `test/hal_test.c`; the boot helper;
the two status fields; the suite; the script.

**Pass:** the tests pass; the suite reproduces the baseline table's "Now"
column; each board has a measured noise figure. Where a figure differs from
1.2 Pa, the tests use the measured one.

---

### T2. Reject single-sample glitches (N24)

**Done 2026-09-26** (DD-040). The coast test first passed on today's code
because the Mach gate held apogee off on its flight; it now also flies one too
slow to latch the gate, where a glitch fired apogee up to 0.4 s early. The
guard measured launch, apogee and the first deployment moving exactly +20 ms
in all 36 closed-loop flights. Second deployments moved −40 to +40 ms,
because the first one's 20 ms changed the canopy's trajectory in the closed
loop. Altitudes moved at most 2 m.

**Why:** a single reading 12 kPa or more low passes the range check (DD-036)
and declares a launch. A flipped high bit in the sensor's raw value could
produce one. The same kind of sample in coast can declare apogee.

**Tests first:**
- `test_T2_pad_glitch_sweep`: single glitches from ±2 to ±60 kPa on the pad,
  under noise: no launch. Fails today at −12 kPa.
- `test_T2_coast_glitch`: a single glitch in coast: no early apogee.
- `test_T2_calibration_glitch`: a glitch during calibration leaves the ground
  error under 1 Pa.
- `test_T2_median_timing`: a straight ramp passes through the median exactly,
  one sample late, carrying the middle sample's time.
- *guard*: the closed-loop and integration suites; deployment times move by
  no more than 20 ms.

**Change:**
- A median of 3 in `pp_feed()`, between the range check and the filter. The
  output carries the middle sample's time. On a monotonic signal the median is
  exactly the middle sample, so it costs one sample of latency and no
  distortion.
- Calibration takes the median of its 10 samples. A spike during calibration
  biases the ground reference, and with N9 it stays biased.

**Records:** new SNS-PRES-07, "a single-sample outlier shall not reach the
filter"; amend FLT-BOOT-08.

---

### T1. Brownout recovery sees real samples, and cannot be fooled on the pad (N23, N25)

**Done 2026-09-26** (DD-041), except the bench check below (G4). Two more
gaps turned up behind N23, and the tests now fly the rejoined flight to its
deployment:
- nothing started the pressure layer after a rejoin;
- a rejoin skips BOOT_CONTINUITY, so PYR-SAFE-01 refused both channels.

Either one alone meant a recovered flight deployed nothing. The speed is a
difference of two medians rather than a fitted slope: an outlier corrupts the
first fit that is meant to find it. The marker is invalidated, not removed,
because `hal.h` has no delete. `test_T1_still_board_stays_cold` and
`test_T1_glitch_on_the_pad` passed before the change, since recovery never
ran; they are guards for it.

**Why:**
- `assess_recovery()` asks the pressure layer for samples in BOOT_SENSOR, but
  the layer only starts at BOOT_CALIBRATE. Recovery always waits out its 4 s
  deadline and boots cold. The level it would compare reads a filtered value
  that is still zero.
- N25: once it sees samples, it runs at every battery power-up against
  whatever marker the last session left. A single glitch could then put a
  board on the pad into a flight state, armed if it reads as descent.

**Tests first:**
- `test_T1_rejoins_descent`: booted as the hardware boots (T0), 600 m up and
  descending with a valid marker: rejoins in FALLING. Fails today.
- `test_T1_rejoins_ascent`: 300 m up and climbing at 50 m/s: rejoins in
  ASCENT. Fails today.
- `test_T1_still_board_stays_cold`: stationary 40 m above the marker's ground,
  under noise, 1000 seeds: never resumes (FLT-BROWN-03).
- `test_T1_glitch_on_the_pad`: at the marker's ground, under noise, one glitch
  of −60 to +60 kPa at every position in the history, and every pair of
  glitches in adjacent samples: always cold.
- `test_T1_cold_reasons`: `/api/status` says which cold it is: no marker, on
  USB, at ground level, or no sample in time.
- `test_T1_marker_invalid_after_landing`: after LANDED, `pad.mkr` no longer
  holds a valid marker. Fails today.
- *guard*: the existing BRN and USB_INT tests.
- Bench, on a board with a marker, booted on USB: status reads "cold: on
  USB". Booting on battery and plugging USB in afterwards, which should read
  "cold: at ground level", is in section 6.

**Change:**
- `pressure_processing.c` keeps about 1 s of the median's output (T2) from
  power-on, in every state, including before calibration.
- Recovery takes its level from the last ~250 ms of that history, against the
  marker's ground pressure, and its speed from a slope over at least 0.5 s.
  Both must survive outliers, not just single glitches: two adjacent glitches
  pass a median of 3. The level is the median of its window, and the slope is
  fitted after discarding samples far from a first fit. Two-sample speed noise
  (1.7 m/s RMS) sits too close to the 5 m/s threshold to use.
- The deadline is unchanged. Sampling starts at power-on, so about 2 s of
  history exists when BOOT_SENSOR runs (BOOT_SETTLE is 2.5 s).
- The status reasons.
- `flight_flash_service()` removes the marker at LANDED.

**Records:** amend FLT-BROWN-02 (speed over at least 0.5 s, from statistics
that survive outliers); new FLT-BROWN-04, "the pad marker shall be removed
when the flight lands, so no later power-up recovers against it"; a DD for the
marker's lifetime.

---

### T3. Hold a trigger for a duration

**Done 2026-09-26** (DD-042). The coast test kept failing with the holds in:
two high readings pushed the filtered altitude below the pad, and the zero
clamp held it at 0 while the filter decayed, a speed of zero for 180 ms. So
T3 also took T5's "speed from the unclamped altitude", which fixed N26 as well
(`test_N26_apogee_above_8km` failed on the old code by 14.9 s).
`test_T3_latency` is a guard: it bounds what the holds may cost. Launch is up
to 100 ms later with T+0 unchanged; apogee moved from +0.59 s to +0.65 s. The
unclamped speed also lets the noise reach the landing test on the pad's own
level, which the clamp had hidden: T4's job.

**Why:** launch and apogee each fire on one sample. A median of 3 doesn't
stop two bad samples in a row.

**Tests first:**
- `test_T3_pad_two_sample_glitch`: no launch.
- `test_T3_coast_two_sample_glitch`: no apogee.
- `test_T3_latency`: detection latency grows by no more than the hold, and T+0
  is unchanged.
- `test_T3_durations_not_counts`: the same scenarios at 50 Hz and 100 Hz
  sample spacing give the same outcomes.
- *guard*: the closed-loop and integration suites.

**Change:**
- Launch needs its condition held for `LAUNCH_HOLD_MS` (100 ms). T+0 still
  comes from `pad_rise_ms`, so the log's times don't move.
- Apogee needs its condition held for `APOGEE_HOLD_MS` (60 ms). T5 changes the
  condition, not the hold.
- Both are durations on sample time, never sample counts, so T9 can change the
  rate.

**Records:** amend FLT-LAUNCH-07 and FLT-APO-01.

---

### T7. Take the launch ground pressure from before the rise

**Done 2026-09-26** (DD-043), except `ground_degraded` on a board's
`/api/status` (G4). The frozen ground now reads 0.08 m at every acceleration.
That is about 1 Pa, the HAL's truncation of each reading to whole pascals.
The degraded flag is GND-CAL-07 rather than an amended GND-CAL-05, which
still holds as written.

**Why:** the reference freezes at detection, 1–2 s after liftoff. Samples
from the first few metres of climb still pass the 50 Pa gate, so every AGL
value reads 0.2–0.5 m low.

**Tests first:**
- `test_T7_ground_error`: frozen ground error ≤ 0.1 m for launches at 2, 5, 15
  and 30 g, under noise. Fails today.
- `test_T7_early_launch_degraded`: a launch within the first second of
  PAD_IDLE keeps the current value and is flagged degraded on `/api/status`.
- *guard*: the REV-07 backdate tests and the AGL accuracy tests.

**Change:**
- Each block of the ground mean records its start time. Today only the block
  being filled keeps one.
- A pressure-layer call freezes the reference to the mean of the blocks that
  ended before a given time, and `action_launch()` passes `pad_rise_ms`.
- If no such block exists, keep the current value and flag it degraded.

**Records:** amend GND-CAL-04 and GND-CAL-05.

---

### M0. Supersonic and failure test support

**Done 2026-09-26.** The plant is `sim/mach_plant.c`, not `sim/physics.c`,
because physics.c drives the browser simulator. The board side of the chain
suite moved to `test/board_harness.c` so both suites fly the board the same
way. The low-drag profile reaches 9.5 km from the cold pad and 10.7 km from
the hot one: ten seconds above Mach 1 against gravity alone needs about
440 m/s at burnout, which carries past 9 km. The report on today's code: a
port error that makes the boost read as a descent fires the low-drag flight's
drogue 39 s before apogee, at Mach 1.27, from either pad. The gate latched on
every flight, even at Mach 0.5, and delayed clean supersonic drogues by up to
1.5 s.

**Why:** the lockout can't be tested with what exists:
- the closed-loop profiles top out at an H73;
- the physics engine's atmosphere is standard, with the pad at sea level;
- nothing models a static port's supersonic error, an ejection charge's bay
  pressure, or a failed sensor.

**Tests first**, of the support itself:
- `test_M0_atmosphere`: pads at 10 °C at sea level and at 45 °C at 2000 m
  produce the pressure and temperature of a reference table, within 0.1 %, up
  to 11 km ASL.
- `test_M0_mach`: the plant's Mach is its speed over √(γRT) at the local
  temperature.
- `test_M0_port_error`: the port error model:
  - is zero below Mach 0.85;
  - varies continuously with Mach above it, and steps at each Mach 1
    crossing;
  - works with either sign;
  - has a boost case where the error grows fast enough that the rocket seems
    to slow, stop or descend.
- `test_M0_profiles`: each profile reaches what it claims:
  - subsonic, never past Mach 0.6;
  - a peak between Mach 0.65 and 0.85;
  - draggy, reaching Mach 1.5, with fast deceleration after burnout;
  - low-drag, coasting supersonic for 10 s or more, apogee 9.5-10.7 km;
  - a 30 g boost;
  - an ejection charge at apogee that pressurises the bay (size and decay are
    parameters);
  - a sensor dropout (no samples for 0.5 s) and a stuck sensor, each in coast;
  - canopy swing: descent pressure noise several times the pad's.
- *guard*: the closed-loop suites, unchanged with the new parameters at their
  defaults.
- `test_M0_report`: `mach_tests` prints, for each profile at both pad
  extremes: whether the drogue fired, the true Mach and time to apogee at
  release, and the delay from true apogee to the drogue, as the prompt asks.

**Change:** a new plant, `sim/mach_plant.c`, with a pad temperature and
elevation, the speed of sound, the port error, the ejection pulse; the test
HAL's stuck sensor; a new suite, `test/test_mach.c` (target `mach_tests`),
added to CI.

**Pass:** the tests pass, and `mach_tests` prints its report for today's code.
That report is the baseline M1 and M2 must change.

---

### T11. Stamp every sample with the hardware timer, at its conversion

**Done 2026-09-26** (DD-046), except the bench check (G4). Differences from
the plan:
- The BMP280 stays in normal mode. Forced mode would change the driver files
  of MK1A and MK1B, which can't be checked without the boards. A free-running
  BMP280 reading is never more than one conversion old, so stamping half a
  conversion before the read bounds the error at ±7 ms through any stall.
- `hal_common.c` runs only on the RP2040. Its stamping arithmetic is a
  tested helper, and the test HAL's model of the stamping now stamps at the
  conversion too. `test_T11_stalls_change_nothing` failed under the old
  model and passes under the new.
- `test_T11_wrap` moves to T5, the first code that does microsecond
  arithmetic across samples.
- A bug in T11's first commit, found by T8's replay: each hold stored its
  start as `ts | 1`. On an even sample time the landing hold read as 4 billion
  milliseconds, so LANDED came on the first still sample, and T6's rejection
  clock had the same flaw. Fixed with tests on both parities; the harness's
  samples fall on odd milliseconds, which is why nothing caught it.
- The loop-clock test first anchored ignition to PAD_IDLE. Since the boot
  timers run on the loop clock, that let a lagged loop fly a different
  flight; it now anchors ignition to a fixed sample time. It fails on the
  committed code and passes on the new.

**Why:** a sample is stamped with the loop's millisecond clock at the top of
the iteration in which its temperature read completes. That instant differs
from when the pressure was converted in three ways:
- it is about 15 ms late, which is constant in the normal case;
- it moves with STAGE 1's 0–5 ms of USB and lwIP work;
- it moves by the whole length of any core0 stall between the conversion and
  the read. A flash erase or log sync lasts 40–73 ms, about 1.3 times a second
  in flight (DD-035).

Every dt downstream inherits that error: the filter's step, every speed, and
the flight log's time column, which is `now - launch_time` on the loop clock.
Under T0's stall model, stalls raise today's worst speed error at 100 m/s from
8.3 to 9.3 m/s. The earlier experiments put a fit-based estimator, which T5
brings in, 294 m/s wrong through a stall; T11's tests measure that again.

**Tests first:**
- `test_T11_stalls_change_nothing`: under T0's stall model, speed and
  acceleration errors match the no-stall case within 10 %, for the filter and
  for a reference fit. Fails today.
- `test_T11_d1_stamp`: with a mocked timer, D1 commanded at t and D2 at
  t + 9.2 ms, the sample is stamped t + 4.5 ms. `conv_start_us` can't provide
  this, because D2's command overwrites it.
- `test_T11_bmp280_forced`: a mocked BMP280 in forced mode stamps at its
  command.
- `test_T11_log_rows_at_sample_time`: logged row times equal the samples'
  times.
- `test_T11_loop_clock_independent`: offset the loop clock from sample time by
  a random 0–70 ms each tick; every scenario gives the outcome it gives
  without the offset. This finds every dwell and timer still on the loop
  clock: PAD_IDLE's `now - ctx->last_sample` gate, the Mach gate's settle
  time, the descent dwell, the failure and ladder windows, and the landing
  hold.
- `test_T11_wrap`: a pad wait longer than 72 minutes crosses the 32-bit
  microsecond wrap without effect.
- Bench, MK1C with Lua running and the log open: `/api/status`'s smallest and
  largest sample interval, and the largest stamp-to-read lag, stay within what
  the host stall model assumes.

**Change:**
- **MS5607:** save the hardware-timer time when D1 is commanded, beside
  `d1_raw`. Stamp the sample at that time plus half the OSR 4096 conversion
  (about 4.5 ms). The hardware timer keeps counting through a flash erase
  with interrupts off, so a sample read late is still stamped when it was
  taken.
- **BMP280 (MK1A):** it free-runs in normal mode and is read at 50 Hz, so
  every reading is 0–14 ms old by an unknown amount. Switch to forced mode:
  command a measurement, stamp the command, and read after the measurement
  time (at most 13.3 ms at x4 pressure and x1 temperature).
- **Timestamps:** the 64-bit microsecond counter. A 32-bit one wraps every
  71.6 minutes, which a long pad wait exceeds.
- **The pressure layer:** `altitude_sample_t` carries the sample's own time,
  and the filter's step uses the real dt.
- **Detectors:** every dt, dwell and timer runs on sample time. Flight-log
  rows carry sample time.
- **No timer-driven sampling:** a hardware alarm starting conversions would
  not make sampling uniform through stalls, because a flash erase runs with
  interrupts off. True stamps make uniform sampling unnecessary.
- **Observability:** `/api/status` reports the smallest and largest sample
  interval and the largest stamp-to-read lag.

**Records:** new SNS-PRES-08, "each sample shall carry the time its
conversion was made, from the hardware timer"; amend FLT-RATE-04 and DAT-02
(the log's time is the sample's).

---

### T4. Keep fractional precision in the filter

**Done 2026-09-26** (DD-044), before T11: it needed nothing from it. Filtered
noise 0.17 Pa, pad speed 0.22 m/s, touchdown to LANDED 1.6 s at both
landing heights. Two earlier tests caught side effects:
- T3's latency guard: the quiet filter delayed T+0 by its time constant,
  260 ms at 2 g. T+0 now comes from the median's unfiltered reading, within
  one sample of the truth. The guard and both backdate tests now compare with
  the truth rather than with the old code.
- T2's median test claimed a spike changes nothing. It swaps in a
  neighbouring reading, one ramp step away, which the whole-pascal filter
  had rounded off.

The pad-speed bench check is owed (G4).

**Why:** the filter state is in whole pascals, and SNS-PRES-04 forces a ±1 Pa
step whenever raw and filtered differ. With α ≈ 0.038 at 20 ms, every
difference under about 26 Pa rounds to no step, so the forced step turns the
filter into a rate limiter that passes noise through: 1.68 m/s of speed noise
instead of 0.20. T5 moves decisions to the fit, but the filter still gives
the reported and logged altitude, the ground mean and, until T5 lands, every
speed.

**Tests first:**
- `test_T4_filter_noise`: with 1.2 Pa of input noise, the filtered pressure's
  noise is ≤ 0.25 Pa RMS. (A first-order filter at τ = 500 ms and 50 Hz gives
  about 0.17 Pa.) Fails today.
- `test_T4_pad_speed`: speed noise on the pad ≤ 0.3 m/s RMS. Fails today.
- `test_T4_touchdown`: touchdown to LANDED ≤ 3 s under noise, on the pad's
  level and 5 m above it. Fails today: since T3 it never lands by stillness
  at either.
- *guard*: the AGL accuracy tests (REV-05) and the closed-loop suites.
- Bench: `noise_baseline.py`'s pad-speed RMS on all three boards is ≤ 0.3 m/s.

**Change:** the filter state in Q8 fixed point; the forced step removed;
altitude from the fractional pressure; `pp_last_filtered_pa()` rounds.

**Records:** withdraw SNS-PRES-04; amend SNS-PRES-02; write the "Pressure
Filter" section of IMPLEMENTATION.md that `pressure_processing.c` already
points to.

---

### T6. Let the ground tracker recover from a step (N9)

**Done 2026-09-26** (DD-045), except `ground_reseeds` on a board (G4). The
step test first passed at 60 Pa on the code after T4: the quiet filter lets
the mean creep through a small step. It now sweeps 60 to 300 Pa both ways,
and failed from 100 Pa up. The drift, launch and gust tests are guards; they
passed before the change.

**Why:** after a shift of more than 50 Pa (about 4 m, such as carrying the
rocket to a higher pad), the gate rejects every sample. The reference freezes,
and the pad marker records a stale ground.

**Tests first:**
- `test_T6_step_reseeds`: a 60 Pa step re-seeds within `GND_RESEED_MS`, and
  the marker is rewritten with the new ground. Fails today.
- `test_T6_launch_never_reseeds`.
- `test_T6_gusts_never_reseed`: gusts of 20–80 Pa shorter than
  `GND_RESEED_MS`.
- *guard* `test_T6_drift`: 2 hPa/h of weather drift over a 60-minute pad wait
  keeps the ground error within 1 m, with no re-seed.

**Change:** in PAD_IDLE, if every sample has been rejected for
`GND_RESEED_MS` (5 s) while the board is stationary (speed under 1 m/s),
re-seed the mean at the current filtered pressure; restart the marker dwell;
send `!GND reseed`; count re-seeds on `/api/status`.

**Records:** new GND-CAL-06.

---

### T8. Log raw pressure and temperature; replay real flights

**Why:** the log carries filtered pressure only, so a filter change can't be
checked against a real flight.

**Tests first:**
- `test_T8_columns`: `flight_log.csv` rows carry `raw_pa` (after the range
  check, before the median) and `temp_c`, in all three HALs.
- *guard* web: a log with the extra columns parses. The UI reads columns by
  name.
- `api_check.py`: the empty log's header has the new columns. Fails today.
- `test_T8_replay`: `sim_cli --replay <log>` runs a closed-loop flight's own
  log through the real pressure layer and detectors, and produces the same
  events at the same sample times.
- Bench, MK1C with Lua running, a 60 s log: 0 flash refusals, no dropped
  sample, and `stage_max_us[7]` no worse than DD-035's 73 ms. Record bytes per
  row and erases per minute.

**Change:** the two columns; a replay mode in `sim/sim_cli.c`, which already
runs the real pressure layer and detectors (N6).

**Records:** amend DAT-02; a DD for the log format, covering T11's time change
too.

---

### T5. One estimator: a fit to the pressure

**Why:**
- Speed is recomputed in three places (pad, ascent, descent) as the
  difference of two filtered altitudes 20 ms apart. It is noisy, and apogee
  lands 0.56 s late.
- The Mach lockout needs the rate of change of pressure, its second
  derivative, and a test of whether the data can be trusted. Only a fit gives
  all three. That settles T5's old choice between a least-squares slope and an
  alpha-beta tracker; the tracker would also need tuning to the vehicle, which
  the Mach prompt rules out.

**Tests first** (in `pressure_chain_tests` and `mach_tests`):
- `test_T5_fit_reference`: on random quadratics with jittered spacing and
  stall gaps, the fit's p, ṗ and p̈ match a double-precision reference.
- `test_T5_fit_noise`: with the measured σ, the ṗ and p̈ noise are within 10 %
  of σ·√Σc² for the 1 s window at 50 Hz (about 2.3 Pa/s and 4.4 Pa/s²).
- `test_T5_clean`: a fit is clean when its RMS residual is ≤ 2σ and no sample
  lies more than 4σ from it. A 10σ step makes every fit that contains it
  unclean, for one window and no longer.
- `test_T5_pad_speed`: ≤ 0.3 m/s RMS.
- `test_T5_touchdown`: LANDED within 3 s of touchdown, 5 m above the pad and
  on its level.
- `test_T5_apogee`: over 1000 seeds and apogees from 100 m to 9 km: never
  before the true apogee, and a mean delay within 0.1 s of what the 1.0001
  drop implies.
- `test_T5_ejection`: M0's ejection pulse at the drogue doesn't move the
  main. An AGL main fires within 8 m of its setting.
- `test_T5_canopy_swing`: under M0's canopy-swing noise, the main still fires
  within 8 m, and LANDED still comes within 3 s of touchdown.
- `test_T5_speed_and_delay_triggers` (REV-05): a SPEED channel fires within
  1 m/s of its setting, and a DELAY channel within 0.1 s of the true apogee
  plus its delay.
- `test_T5_through_the_clamp`: the speed stays unbiased on a landing 20 m
  below the pad, while the reported altitude stays clamped at zero.
- `test_T5_under_thrust`: `under_thrust` matches the plant's burn within
  100 ms.
- *guard*: the closed-loop and integration suites, the emergency-ladder tests,
  and the AGL accuracy tests.
- Bench: the fit costs at most 500 µs per sample on RP2040.
  `stage_max_us[2]` rises by no more than that, with 0 loop overruns.

**Change:**
- `src/pressure_fit.c`: a least-squares quadratic over a fixed 1 s window of
  the median's output (T2), fitted against each sample's own time (T11) and
  evaluated at the newest sample. At that point constant acceleration causes
  no lag. It outputs p, ṗ, p̈ and a clean flag. If 500 µs is too much, keep
  running sums, or use precomputed coefficients while the spacing is even and
  solve in full only after a gap.
- σ is measured in PAD_IDLE as the fit's residual RMS. It is clamped between
  the sensor's datasheet figure and a ceiling, so a gusty pad can't loosen the
  clean test. The pad marker stores it for recovery (marker version 2).
- One function gives every detector its vertical speed. It converts ṗ through
  the slope of the altitude formula at the fitted pressure, so it agrees with
  the altitude the detectors compare. Like T3's, it never passes through
  either clamp (SNS-ALT-04).
- AGL and FALLEN compare the fit's pressure, which doesn't lag, so DD-029's
  lead goes. SPEED uses the fit's speed. DELAY counts from the better apogee.
- After each charge fires, pressure-driven triggers wait for a clean fit, for
  at most 2 s, and then act regardless, so a noisy canopy can't hold back the
  main.
- Apogee: fitted ṗ > 0 on clean fits, held for `APOGEE_HOLD_MS` (T3), and
  p ≥ 1.0001·p_min, where p_min is the lowest fitted pressure on clean fits.
- `under_thrust` comes from p̈.
- Re-derive every threshold tuned against the old estimator:
  - `ARM_SPEED_CMS` (DD-017 assumed the filtered speed was half the true one);
  - `LAUNCH_SPEED_CMS`;
  - the descent bands and tolerances;
  - the landing thresholds.

  The Mach gate's constants carry over as true speeds until M1 replaces it.

**Records:** amend FLT-APO-01, PYR-MODE-05, and the affected FLT-DESC and
FLT-LAND entries; amend DD-017 and DD-029; a new DD for the estimator. Mark
REV-05 fixed.

---

### M1. The Mach lockout

**Why:**
- The Mach gate (FLT-MACH-01, DD-025) latches above 100 ft/s of filtered
  speed, which almost every flight exceeds. It then trusts the data after 1 s
  below that speed.
- A supersonic port error can fake exactly that. The error lasts as long as
  the rocket is supersonic, not just at the crossings, and it can make a
  climbing rocket look slow, stopped or falling. With the gate open and the
  pyros armed, apogee fires while the rocket is supersonic.
- The prompt's lockout sets its flag while the data is still clean, and
  releases only on the full signature of a subsonic coast.

**Tests first** (`mach_tests`, on M0's profiles, at both pad extremes):
- `test_M1_subsonic_never_locks`: a flight under Mach 0.6 is never locked,
  and deploys as a plain altimeter would.
- `test_M1_flag_before_mach_085`: every profile that passes Mach 0.85,
  including the 30 g boost, is flagged before Mach 0.85.
- `test_M1_mid_mach_releases`: peaks between Mach 0.65 and 0.85 are flagged,
  and released within the window plus 1.5 s of burnout.
- `test_M1_no_drogue_before_apogee`: the draggy Mach 1.5 flight, the low-drag
  10 s coast, and the persistent error of both signs, including the boost that
  looks like a descent. The drogue never fires before the true apogee, and
  fires within 1.5 s after it. Expected to fail today for the error cases.
- `test_M1_release_at_altitude`: low-drag to 9 km from a 45 °C pad at 2000 m
  releases before apogee in 1000 of 1000 seeds.
- `test_M1_fallback`: with the release forced off, the drogue fires once clean
  fits show a sustained descent back past p_flag, and not before.
- `test_M1_recovered_ascent_locked`: brownout recovery into ASCENT starts
  locked, with p_flag at the current pressure. Recovery into descent is not
  locked.
- `test_M1_minimum_altitude_arm`: no channel arms before p < 0.9965·p0 (about
  30 m) on the way up, including on a flight that peaks at 25 m.
- `test_M1_peak_outside_lock`: the reported peak comes from p_min outside the
  locked interval. It is flagged a lower bound if the lock released within
  2 s of apogee.
- `test_M1_integer_forms`: each integer comparison equals its real-valued
  threshold, without int32 overflow, from 1 to 120 kPa.
- `test_M1_design_note`: a host program derives each threshold's Mach and g
  range over 216–318 K, and the fit's noise. The tables in
  `docs/mach_lockout.md` match its output.
- `api_check.py`: `/api/status` carries `mach_lock` and `mach_flag_ms`.
- *guard*: the closed-loop and integration suites, and every T5 test.

**Change:**
- The prompt's flag, release and fallback, in the pressure domain and in
  their integer forms, on T5's fit:
  - **flag:** −ṗ > 0.029·p, on any fit, clean or not. Setting it is the safe
    direction, and a hard boost can pass Mach 0.85 before a window clears the
    ignition step.
  - **release:** 0 < −ṗ < 0.022·p and p̈ ≥ 0.0009·p on every fit, all clean,
    for 1 s continuously. Then p_min restarts at the current pressure and the
    flag clears.
  - **fallback:** while locked, fitted ṗ > 0 on clean fits for 1 s, and
    p > p_flag: fire the drogue.
- While locked there is no apogee, and p_min is not updated.
- The lock is a latch inside ASCENT, not a new state, so the state table,
  TEL-07's mapping and the UI keep their states. LOCK, UNLOCK and
  LOCK_FALLBACK are logged events and telemetry lines.
- The arming gate gains the minimum-altitude arm (p < 0.9965·p0).
- Brownout recovery into ASCENT starts locked, because the flight's speed
  history is lost.
- FLT-MACH-01's gate, `MACH_GATE_CMS` and `MACH_SETTLE_MS` go.
- `docs/mach_lockout.md` holds:
  - the prompt's explanation;
  - each threshold's derivation and category;
  - the fit's noise for the chosen window;
  - the assumptions and remaining risks, including how large and how smooth a
    port error must be to fake 1 s of the release signature during boost,
    measured with M0's harness;
  - static-port guidance for the user.

**Deviations from the prompt** (M1-D, adopted):

| Prompt | Here | Why |
|---|---|---|
| Precomputed integer fit coefficients, no floating point | Solved per sample against each sample's time, in floating point; the thresholds compare in the prompt's integer forms | Flash stalls make the spacing uneven (T11). The code already converts altitude in float. |
| Raw samples | The median of 3 (T2), with σ measured on the same stream | Single glitches never reach the fit |
| 0.5 s at 100 Hz | 1 s at 50 Hz, until T9 | The release margin at altitude (T5-W) |
| Flag on clean fits | Flag on any fit | Hard boosts |
| LOCKED is a state | A latch in ASCENT | Keeps the state table, telemetry and UI |
| Launch on p̈ | Per C2 | Misses launches under ~2.1–2.4 g net |
| Each channel fires once | PYR-REFIRE-01's one retry of a drogue whose charge did not light stays | DD-028 |
| Main on p > p_main | The configured modes (AGL, FALLEN, SPEED, DELAY), on the fit's pressure | AGL is the same comparison; the other modes are the operator's choice |

**Records:** withdraw FLT-MACH-01; new FLT-MACH-02..07 (flag, release,
fallback, no apogee while locked, minimum-altitude arm, peak outside the
lock); a DD for the lockout, superseding DD-025 and recording the deviations;
amend DD-017; rewrite `docs/flight_states.md`'s Mach gate section.

---

### M2. Sensor failure in flight

**Why:** the Mach prompt requires a defined response to a stuck, out-of-range
or lost sensor, and that a failed sensor never cause a deployment. That is
DD-022's rule too: nothing forces a deployment the sensor didn't ask for.
Out-of-range readings are already discarded and counted (DD-036). Nothing
detects the other two, and a stuck value reads as a rocket that has stopped.

**Tests first** (`mach_tests`):
- `test_M2_stuck_in_coast`: the sensor sticks in coast, armed or locked: no
  deployment while it is stuck. Expected to fail today.
- `test_M2_dropout_in_coast`: no samples for 0.5 s in coast: no decision on
  stale data. Fits count as clean only once a full window of new samples
  exists.
- `test_M2_lost`: reads failing for more than 0.5 s in flight are flagged, and
  nothing deploys on the last value.
- *guard* `test_M2_out_of_range`: readings outside 1–120 kPa in flight are
  discarded and counted, and never deploy anything.
- `test_M2_reported`: each failure sets a DIAG bit, logs an event and shows on
  `/api/status`. The flight carries on if the sensor recovers.
- `test_M2_real_sensor_never_stuck`: a sensor with the measured noise never
  trips the stuck test over a 60-minute pad wait.

**Change:** a full window of identical readings is a stuck sensor; a real one
with 1.2 Pa of noise can't produce that. A stuck, lost or dropped-out sensor
makes every fit unclean. That holds the lock and every pressure-driven
trigger, and T5's 2 s limit on waiting for a clean fit does not apply.

**Records:** new SNS-PRES-09 (stuck) and SNS-PRES-10 (lost);
`docs/flight_states.md`.

---

### T9. Read MS5607 temperature less often

**Why:** every sample converts both pressure (D1) and temperature (D2), at
10 ms each. Temperature moves slowly. Converting it every tenth cycle nearly
doubles the pressure rate, which cuts the fit's noise and widens M1's release
margin.

**Tests first:**
- `test_T9_same_outcomes`: the key scenarios in `pressure_chain_tests` and
  `mach_tests`, at 50 Hz and 90 Hz, give the same outcomes within the new
  timing.
- `test_T9_short_interval`: the pressure task schedules correctly with an
  interval under 20 ms. Fails today: its idle time,
  `sample_interval_ms - 2 * MS5607_CONV_MS`, is unsigned and wraps.
- `test_T9_temperature_reuse`: compensating D1 with a D2 up to 10 cycles old
  stays within 1 Pa of full compensation, for a sensor warming at 1 °C/s.
- `test_T9_drains`: the detectors drain every waiting sample each tick.
- Bench: the status shows about 90 Hz; `pres_waits` and `pres_rejects` stay
  at 0, with 0 loop overruns; `stage_max_us[2]`, with T5's fit, stays within
  budget.

**Change:** D2 every Nth cycle, compensating with the last temperature; the
sample ring from 32 to 64; every constant that assumes 20 ms, such as
`PAD_SAMPLE_MS` and the idle time. The fit's window is a duration, so it gains
samples. The BMP280 path (MK1A) is unchanged.

**Records:** amend FLT-RATE-01 and FLT-RATE-02.

---

### T10. Documentation sweep

**Tests first:**
- G5 passes, with no requirement left without a test.
- `support/trace_check.py --counts` matches TRACEABILITY's summary table.

**Change:**
- Mark N9, N23–N26 and REV-05 fixed in the resolution doc and here.
- Update `docs/flight_states.md` (defects 2, 10, 16 and 17, and the Mach
  section), IMPLEMENTATION.md, and `test/README.md`.

---

## 5. Other code defects

Each starts with its tests, like any other task.

### N11. Text rows in the flight log carry uptime

**Done 2026-09-26**, except its bench check. `lua_app.c` and `hal_common.c`
run only on the RP2040, so the check that their rows carry flight time is in
section 6. They now take `flight_elapsed_ms()`, which `test_REV09_*` covers
on the host.

**Needs:** T11. LUA and MOCK rows carry uptime, while sample rows carry flight
time. Fixed before T11, the Lua rows would move to the loop clock just as the
sample rows move to sample time.
- **Tests first:** `test_N11_text_rows_on_the_flight_clock`: a LUA row written
  between two samples carries a time between theirs. Fails today.
- **Change:** `lua_app_service()` passes flight time, on the sample clock.

### N18. LANDED's 1 Hz logging never fires

**Done 2026-09-26.**

`detect_landed()` compares against `last_sample`, which it updates on every
sample.
- **Tests first:** `test_N18_landed_logs_once_a_second`: after LANDED, one row
  per second for 10 s. Fails today.
- **Change:** track the last logged time separately from `last_sample`.

### N20. All littlefs mounts share one set of buffers

**Done 2026-09-26** (WEB-API-10), except its bench check. `http_tests`
covers only the HTTP engine; the routes in `http_server.c` need lwIP, so the
refusal can only be checked on a board. That check is in section 6.

A file download while the flight log is open can disturb the log (not
observed).
- **Tests first:** on a bench board in test mode with the log open, a file
  GET answers 409, `/api/status` still answers, and the log is intact after
  landing.
- **Change:** `serve_file()` refuses while `hal_log_active()`.

### N12. A canopy approaching its rate from below can settle in the main band

**Needs:** T5. Phase report only; triggers don't read it.
- **Tests first:** `test_N12_drogue_from_below`: a drogue that approaches its
  terminal rate from below is reported as DROGUE_DESCENT. Fails today in the
  simulator.
- **Change:** revisit the band test on T5's speed.

### N7 / C8. The landing timeout declares LANDED under a main

**Needs:** T5.
- **Tests first:** `test_N7_no_landing_under_main`: the simulator's 5000 ft
  flight with a drogue declares no LANDED while the main descends at 5 m/s,
  and LANDED within 3 s of touchdown, including a touchdown above the pad's
  elevation. Fails today: LANDED comes 0.7 s after PYRO2, at 61 m.
- **Change:** the timeout needs stillness, not "slower than 5 m/s".
- **Records:** amend FLT-LAND-07.

### C10, C6, U6

Waiting on their decisions (section 2). Their tests, once decided:
- **C10:** `test_REV01_failed_drogue_brings_the_main_forward` asserts the
  chosen timing; slow-drogue profiles never force the main.
- **C6:** the chosen number of flights is kept, and nothing writes flash
  between launch and the log's RAM buffer first filling (FLT-LOG-05,
  FLT-BROWN-01).
- **U6:** `test_U6_audition_silent_on_usb` and
  `test_U6_ground_test_beep_silent_on_usb`.

---

## 6. Bench checks

Each check's pass criteria are its test. Record the result here and in the
resolution doc.

| ID | Check | Pass | Needs |
|---|---|---|---|
| U2 | Unplug a board from USB, then plug it back in | the pad announcement resumes within one repeat period (5 s); plugging back in gives exactly one double chirp | a person, 10 s, on MK1B or MK1C |
| REV-06 / REV-08 | Serial ground-test commands; a board in FAULT | FIRE while the other channel pulses answers `GT,ERR,busy`; a FAULT board sends `!FAULT <diagnosis>` every 5 s and no `$PYRO` | a USB-serial adapter on the TRRS jack |
| REV-18 | The in-flight lock on hardware | in test mode, once a chamber pump-down declares a launch, every POST answers 409 until LANDED | test mode, the chamber |
| — | The USB network after the Mac sleeps and wakes | the board answers `/api/status` within 10 s of wake, without replugging | a person. The v2.1.50 fix is gone, so today's behaviour is unknown |
| — | Chamber runs for T1–T7 | pump-down, hold and vent in test mode give launch, apogee and landing as the host tests predict, with no false launch during the hold | the chamber |
| T1 | Recovery reads samples on the hardware | a board with a marker, booted on battery with USB plugged in afterwards, reads "cold: at ground level" | a battery |
| T1 | Brownout recovery on the real path | a power cut during a chamber descent rejoins in FALLING; a power cut on the pad stays cold | a battery, the chamber, telemetry over serial or radio (USB forces a cold boot, and a reset ends test mode) |
| N11 | LUA and MOCK rows on the flight clock | in test mode, a script that calls `log()` once a second through a chamber flight writes LUA rows whose times fall among the sample rows', not near the board's uptime | test mode, the chamber, MK1C with Lua |
| N20 | No file served while the log is written | in test mode, once a chamber pump-down declares a launch, `GET /www/app.js` answers 409 and `/api/status` 200; after LANDED the log reads back whole | test mode, the chamber |
| — | The arming path independent of software | with the mechanical disconnect in, a commanded ground-test FIRE puts no current through a dummy load, on each board | a dummy load and a meter. The Mach prompt asks for this path; the operator narrative uses a mechanical disconnect, but no document says what it breaks |

The Mach lockout can't be checked in a chamber, because it needs supersonic
flow. `mach_tests` is its only check short of a flight.

---

## 7. Board changes

| ID | What | Tests first |
|---|---|---|
| U1 | Detect a charger on USB (USB-06). VBUS reaches only the TP4057 on MK1A, MK1B and MK1C. The options are a VBUS divider to a spare GPIO (MK1C has GPIO2–5, 9, 10 and 13–15 free; GPIO24 on MK1A/B), or routing the charger's CHRG/STDBY pins to GPIOs. Firmware would OR either with the USB frame check. | Host, with a mocked VBUS input: a charger alone counts as attached; a sleeping PC (VBUS, no frames) counts as attached (U3); no input counts as detached. Bench, on the new board: a charger stops launch detection. |
| REV-03 | MK1C cannot fire: the F0–F10 firing sequence is unbuilt. | From the MK1C specs in `~/Documents/pyro_mk1c/`: host tests for the sequence, and a bench fire into a dummy load that sets `pyro1_fired` and shows the pulse on a scope. |
| — | An arming path independent of software, if section 6's check finds a board without one | That check. |

---

## 8. Documentation and housekeeping

### H1. A traceability check

**Done 2026-09-26.** Its first run found 72 problems; each is fixed below.

Do this first, so every task's records are tested (G5).
- **Tests first:** `support/trace_check.py`, run on today's tree. It checks
  that:
  - every requirement ID cited in `src/`, `test/` and the docs exists in
    `REQUIREMENTS.md`;
  - every DD cited exists in `DECISIONS.md`;
  - every requirement not withdrawn appears in `TRACEABILITY.md`;
  - every test `TRACEABILITY.md` names exists in `test/`;
  - every function named in backticks in the docs exists in `src/`;
  - the summary counts match.

  It will find today's mismatches, and that is its failing run.
- **Change:** fix what it finds, and add it to CI.

What its first run found, and what was done:
- 52 requirements had no traceability row, including every user need and
  BUZ-CODE-01..13. Each now has a row naming the tests that check it, or
  "—" and ⚠️ where none does: FLT-BOOT-13, FLT-BOOT-14's transition to FAULT,
  FLT-LAND-07 (N7), BUZ-CODE-12, PWR-BUZZ-02's ENCODE state, SYS-PWR-01 and
  SYS-PORT-02.
- Five rows named tests that had since been renamed or removed.
- Test comments cited PIN-BUZZ-03..07 and FLT-BOOT-10, which were test
  numbers, not requirements. They now cite the requirement each test checks.
- DECISIONS.md cites PYR-SAFE-02 and FLT-APO-05/06, which DD-021 and DD-022
  removed. They are back in REQUIREMENTS.md, marked withdrawn.
- The summary said 130/18/1/1; the tables have 186/32/1/12.
- `pressure_processing.c` pointed to an IMPLEMENTATION.md "Pressure Filter"
  section that didn't exist. It does now.

Found by reading rather than by the check, and fixed with it:
- IMPLEMENTATION.md's transition table, launch criterion (10 m), file names,
  flight-log policy (it said nothing is written in ascent), altitude formula
  and test counts were stale.
- The resolution doc called REV-23's route table not done. POSTs route through
  a table since the stream server; GETs still don't.

### H2. The browser demo

`docs/app/` was last synced in March (N19), and the WASM simulator was never
rebuilt after the simulator fixes (N6). `emcc` is now installed.
- **Tests first:** after `scripts/sync_demo.sh`, `docs/app/www` matches
  `www/`, with a CI step that fails when they drift; a headless run of
  `docs/sim.html` flies from boot to LANDED.
- **Change:** run `scripts/sync_demo.sh` and `scripts/build_wasm.sh`.

---

## 9. Deferred, and not planned

### False-launch reversion

Not scheduled, at your decision (2026-09-25).

A board that declares a launch but is never armed, and whose altitude returns
to near the ground and stays there for several seconds, would go back to
PAD_IDLE. This is safe because the pyros are never armed before the arming
gate.

Today such a board sits in ASCENT until it is power-cycled (defect 2, N24):
- its flight log stays open;
- the in-flight lock refuses reboot and OTA (C5);
- the pad announcement has stopped;
- its ground reference is frozen.

T2 and T3 make false launches much rarer, but don't recover from one that
happens. If taken up, it needs:
- a requirement;
- a DD, because it is the only transition that leaves a flight state
  backwards;
- a decision on what happens to the open flight log;
- tests covering a glitch launch, a real slow launch that never arms, and a
  real flight.

### Asked for by the prompts, not planned

Recorded so they aren't raised again. Say if you want any of them.

From `docs/pressure-filter-prompt.md`:

| Asked for | Instead | Why |
|---|---|---|
| A 30 s warm-up and a stability test (≤ 4 Pa σ and ≤ 1 Pa/s over 30 s) before launch detection | PAD_IDLE after calibration | T2, T3 and T6 cover what the stability test guards against, and launch detection is ready 30 s sooner |
| A/B ground records with a CRC, a pre-erased launch record and a landed flag | One pad marker in littlefs, written once on the pad (FLT-BROWN-01) and removed at landing (T1) | littlefs survives power loss, and nothing writes flash at launch |
| Ground records refreshed every 300 s | Written once after 10 s of PAD_IDLE, and again after a re-seed (T6) | Flash wear. A pad wait's drift is small against recovery's 30 m threshold |
| A tag-length-value Extra field on log records | Event rows in the CSV log, and named columns (T8) | The CSV describes itself in its header, and the web UI reads columns by name |
| Accelerometer launch paths | — | No MK1 board has an accelerometer |
| A 40 Pa gate, a 20 s ground filter, and a 3 s history lookback | The 50 Pa gate, the 5 s boxcar, and `pad_rise_ms` (T7) | T7 takes exactly the blocks before the rise instead of a fixed lookback |
| Fast smoothing at τ = 1 s | τ = 500 ms (SNS-PRES-02) | Decisions move to the fit (T5); the filter serves the display, the ground mean and the log |

From `docs/mach-lockout-prompt.md`: the deviations are listed in M1, and the
launch trigger is C2.
