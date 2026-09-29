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
| 1 | [Commit the work](#1-commit-the-work) | done; G4 last ran 2026-09-28, on 2.1.702 |
| 2 | [Decisions](#2-decisions) | you; **HA-1: the Mach lockout above 9 km** |
| 3 | [Safety fixes](#3-safety-fixes) | done |
| 4 | [The pressure chain and the Mach lockout](#4-the-pressure-chain-and-the-mach-lockout) | done; T5's cost over its limit (T5-C) |
| 5 | [Other code defects](#5-other-code-defects) | done, except C10, C6 and U6: decided, not yet built |
| 5a | [No sleeps](#5a-no-sleeps-dd-053) | done |
| 5b | [MK1C firing path](#5b-mk1c-firing-path) | **done in code** (DD-056); the bench fire is deferred (section 9) |
| 6 | [Bench checks](#6-bench-checks) | a person or equipment, the ground test's (GT-1) among them; ID-1, two MK1Cs share one identity: fixed on the bench, **your decision** for later MK1Cs; SPI-1, the J3 SPI test board: the IMU works, the card does not come up; SD-1, the card's limits and the logger over a bench flight |
| 7 | [Board changes](#7-board-changes) | hardware design; **B-U5: MK1B cannot sense continuity**; B-BZ: MK1B's buzzer disturbs its sensor |
| 8 | [Documentation and housekeeping](#8-documentation-and-housekeeping) | every document reviewed 2026-09-28; H3: the defects and unmet requirements the review found |
| 9 | [Deferred, and not planned](#9-deferred-and-not-planned) | F1's bench fire; false-launch reversion |
| 10 | [Logging and lockups in flight](#10-logging-and-lockups-in-flight) | **your decisions**: the logging design (plan 1, R1, or plan 2, FreeRTOS), the Mach short rate, lwIP's heap |

**G4, the bench check on the four boards** (MK1A 02632D472F0C, MK1B
02E7253A34C2 and 02E72A403441, MK1C 02373331FFDE): `api_check.py`,
`http_stream_check.py` and `hw_ui_check.js`. Last run 2026-09-28 on 2.1.702:
every check passes on all four, at 50 loops a second, with no loop
overrun, no flash refusal and no pressure reject. G4-L and G4-N, which
the repeated runs found, are done; G4-M is found and waits on your
decision (section 10, M3). Section 6 lists
what still needs a person or equipment.

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
     is DD-072);
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

**G4 ran on 2026-09-27** (above).

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
| C2 | The launch trigger | **Open, not blocking.** 2026-09-27 you asked for a continuous rise in altitude for more than 1 s, past 15 m. The fitted climb speed has to hold above a threshold for that second: raw samples rise every time only above about 200 m/s. It detects 1.1-1.6 s after ignition, short burns included, but above about 25 g net the rocket passes Mach 0.85 before the second is up, and the ports can then read a fall. The open question: that rule alone, or with today's rule (100 ft, 5 m/s, 100 ms) as a fast second trigger. | — |
| C5 | In-flight lock against recoverability | **Decided 2026-09-26 (DD-058):** the API and USB stay live in flight, so a board stuck in a flight state takes a reboot; only the filesystem is locked, to the flight log, and a request for it gets 423. | — |
| C6 | Numbered flight logs | **Decided 2026-09-27:** keep 2 flights, and write nothing at launch; OK to fly sounds only once the log is ready. You then proposed a raw working log outside littlefs (1 MB on MK1A and MK1C), built into a littlefs file after landing, with the start sector rotated for wear. Open for it: the working log's size on MK1B (2 MB of flash), and whether file reads may be allowed in flight. | C6 task |
| C8 / N7 | The landing timeout declares LANDED under a main | **Adopted:** once T5 makes the stillness test reliable, the timeout needs stillness instead of "slower than 5 m/s". | N7 task |
| C10 | Forced main after a failed drogue | **Decided 2026-09-27:** force the main when the descent reaches a tunable critical speed, default 50 m/s, about 5 s of free fall from apogee, the most you will accept. Replaces today's 4.6 s. | C10 task |
| U3 | A sleeping PC counts as unplugged | **Decided 2026-09-27: leave it.** | — |
| U4 / U5 | Silence on USB against BUZ-02 and FLT-BOOT-12/14 | **Decided 2026-09-27:** on USB every announcement plays once and does not repeat. BUZ-02 becomes "When a USB host is not attached, the announcement shall repeat ...". Still to confirm: whether the attach chirp stays, and whether a verdict that changes while attached plays once more. | U6 task |
| U6 | The audition button and ground-test BEEP on USB | **Decided 2026-09-27:** they play once on USB. The audition already does; the ground-test BEEPs repeat today, so they change. | U6 task |
| T3 | How long launch and apogee must hold | **Adopted:** 100 ms and 60 ms. | T3 |
| T5-W | The fit's window | **Adopted: 1.0 s.** At 50 Hz it gives 0.19 m/s of speed noise on the pad and 4.4 Pa/s² of p̈ noise. The Mach prompt's example of 0.5 s gives 23.8 Pa/s² at 50 Hz (17.5 at 100 Hz). At 9 km, with no drag, 1 g of deceleration would then sit only 0.7σ (1.0σ at 100 Hz) above the release threshold, and the release would keep resetting. The cost of 1 s: a Mach step or an ejection keeps fits unclean for 1 s instead of 0.5 s, and so does the ignition step, which M1 handles by setting the flag on any fit. | T5 |
| T5-A | How far below the peak apogee needs | **Adopted: p ≥ 1.0001·p_min**, 0.6–0.9 m, about +0.4 s. The Mach prompt's 1.0005 means 3–5 m and puts the drogue 0.8–1.0 s after apogee; today it is +0.56 s. The fit's pressure noise is about 0.5 Pa. At 9 km, 1.0001 is 3 Pa, so noise cannot fake the drop, and an early apogee becomes impossible. | T5 |
| T6 | How long the ground tracker waits before re-seeding | **Adopted:** 5 s. The pressure-filter prompt's 30 s leaves the pad reference wrong for half a minute after the rocket is set down. | T6 |
| T8 | Flight logging rate | **Decided 2026-09-27 (DD-062, DD-064):** three plans, `log_rate`: a sample row a second by default; High rate 1, that and every sample within 1 s of an event; High rate 2, every sample, the only one that replays. Every event row is kept at its own time. The log is binary on disk and rendered as CSV when downloaded; the Config tab estimates the longest flight it holds under the plan chosen. | — |
| M1-D | Accept M1's deviations from the Mach prompt | **Adopted.** They are listed in M1. The largest: the fit is solved against each sample's own time, in floating point, not with precomputed integer coefficients. Flash stalls make the sample spacing uneven (T11), and precomputed coefficients assume even spacing. | M1 |
| P1 | MK1C's presence pulse is 20-21 ms at the 20 ms loop (DD-065); DESIGN.md S3 asks for 5-10 ms | **Decided 2026-09-27: a timer one-shot**, like the MS5607's, ends the pulse at 8 ms. More pressing since DD-065 doubled the pulse. | P1 task |
| B-U5 | MK1B until U5 is changed | **Decided 2026-09-27: leave it.** The firmware already assumes the base AP2192; an MK1B that owns its pyros reads every channel shorted and cannot fire until its U5 is refitted (section 7). | — |
| T5-C | The fit's cost | **Decided 2026-09-27: leave it.** 2.9 ms on the MS5607 boards against T5's 1.6 ms limit; the loop holds with 0 overruns. | — |
| N20 | Shared littlefs buffers | **Adopted:** refuse file GETs while the flight log is open. It can be tested on the host, the log can be read after landing, and WEB-API-08 already refuses every writer in flight. | N20 task |
| HA-1 | **The Mach lockout above 9 km** | **Open, found 2026-09-29 by the bench flight (DD-078).** Above the lockout's 9 km envelope its release cannot see 0.58 g through the fit's noise, the lock holds, and apogee comes from the fallback 100-190 m above the pad: on MK1C's 3 Pa of noise from about 15 km, on the datasheet's 1.2 Pa from about 22 km. On the bench MK1C-SD a 30 km profile fired drogue and main together at 150 m once in two flights. `docs/high_altitude_flight.md` has the evidence and the options: A, a longer fit for the release as the pressure falls, with E, a fallback on a descent held for seconds rather than on p > p_flag, is the recommendation. `test_SIM_02_30_km_with_sensor_noise_finds_apogee` reports it ignored until then. | HA-1 task |

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
| Speed noise on the pad (RMS) | 1.58 m/s | ≤ 0.3 m/s: done, 0.22 m/s (T4), 0.19 m/s on the fit | T4, T5 |
| Touchdown to LANDED, by the stillness test | on the pad's level 1.9 s, because the zero clamp hides the noise there; 5 m above it, never within 400 s, so only the 60 s timeout lands it | ≤ 3 s, at any landing height: done, 1.6 s (T4), 1.8 s on the fit | T4, T5 |
| Apogee after the true apogee | +0.56 s mean (+0.54 to +0.58) | never early; about +0.4 s: done, +0.41 s, none early in 1000 flights to 9 km | T5 |
| Ground pressure frozen at launch | reads 0.17 m (30 g) to 0.42 m (2 g) low | ≤ 0.1 m: done, 0.08 m | T7 |
| Ground tracker after a >50 Pa step | frozen for good (N9) | re-seeds: done, within 5 s | T6 |
| Sample timestamps | loop ms at the temperature read, 16 ms after the conversion, up to 127 ms after it through a stall; stalls raise the worst speed error at 100 m/s from 8.3 to 9.3 m/s | hardware timer at the pressure conversion: done (through stalls 6.5 m/s against 6.1) | T11 |
| Fit-based speed and acceleration through flash stalls | worst −294 m/s and 441 m/s² with today's stamps (earlier experiments; T11 re-measures) | RMS 1.2 m/s and 3.5 m/s², stalls or not: speed done, 0.2 m/s RMS at 100 m/s, 0.8 m/s worst calm and 0.7 through stalls | T11, T5 |
| A flight above 8 km AGL | apogee fired 14.9 s early, at the clamp (N26) | apogee at the real apogee: done | T3 |
| Supersonic flight with a static-port error | a port error that makes the boost read as a descent fired the low-drag flight's drogue 39 s before apogee, at Mach 1.27 | no drogue before the true apogee, on every M0 profile, by a lockout: done, every drogue 0.38-0.50 s after apogee; flagged by Mach 0.82, released at Mach 0.34-0.49 | M1 |
| A sensor stuck or lost in flight | a stuck value may read as apogee | never causes a deployment: done, and a stuck, gapped or lost sensor holds every decision until a whole window of new samples | M2 |
| Real flights replayable offline | no (raw pressure not logged) | yes | T8 |
| MS5607 sample rate | 50 Hz assumed; 27-35 ms measured | ~90 Hz: done in code, one conversion a loop (DD-051); the bench is owed | T9 |

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
  this, because D2's command overwrites it. Retired by DD-051: the one-shot's
  handler stamps each conversion itself, tested by `ms5607_tests`.
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

**Done 2026-09-26** (DD-047), except its bench checks (G4): `api_check.py`'s
header and a 60 s log run on MK1C. The columns sit before `event`, since a
text row's last field is free text. The replay lives in `sim/replay.c`, as
`pyro_sim --replay`. Building it found the hold bug in T11's first commit,
fixed and tested separately (c8f9b7a).

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

**Done 2026-09-26** (DD-048), except its bench checks (G4): the fit's cost and
`stage_max_us[2]`, and `fit_sigma_mpa` on each board. Before the change the
new tests failed as they should. Apogee came +0.21 s after the drop (now
+0.004 s). SPEED fired 4.7 m/s past its setting (now 0.98) and DELAY 0.71 s
late (now 0.048). The thrust flag changed 120-185 times per ascent (now once).
A 5 kPa bay charge fired the main at apogee (now within 0.6 m). Differences
from the plan:
- `test_T5_pad_speed` and `test_T5_touchdown` were already met by T4
  (`test_T4_pad_speed`, `test_T4_touchdown`), which now run on the fit: 0.19
  m/s, and 1.8 s.
- `test_T5_through_the_clamp` and `test_T5_canopy_swing` passed on the old
  code (T3 took the unclamped speed), so they are guards.
- `test_T5_under_thrust` allows 1 s, not 100 ms: a one-second fit's
  acceleration crosses zero 0.56-0.84 s after burnout (DD-048).
- `ARM_SPEED_CMS` stays at 10 m/s, now of true speed. At 20 m/s the integration
  suite's A8-3, doing 19 m/s at 100 ft, never armed. FLT-ASC-07 read 20 m/s.
- The first wiring broke T3's two-glitch tests. A burst the median lets
  through spoils every fit for a second, longer than the launch hold, so the
  launch reads the two-point speed while the fit is unclean. The same burst
  under the drogue had always fired the main early, by up to 236 m, before T5
  and through DD-029's lead. The new `test_T5_descent_glitch` found it. The
  2 s wait now covers any unclean run (PYR-MODE-06), not only one after a
  charge.
- The ejection and swing tests fly from a 15 °C sea-level pad: at the 10 °C
  pad every AGL main was 11 m low on the ISA formula's temperature error,
  whatever the estimator did.
- New: `test_T5_sigma` (σ, the marker's version 2, a recovered flight's σ),
  `test_FLT_ASC_07_arms_after_ten_metres_a_second`, and `fit_sigma_mpa` on
  `/api/status` for the bench.
- Four existing tests were adapted; DD-048 lists why.

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

**Done 2026-09-26** (DD-049, `docs/mach_lockout.md`), except `api_check.py`'s
new fields on a board (G4). Before the change, on T5's code, seven of the new
tests failed as they should. Five passed as guards: T5's clean-fit apogee had
already kept every drogue after apogee, the integer forms were a new header,
and the 100 ft launch already keeps arming above 30 m. Differences from the
plan:
- The flag is evaluated from T+0 on the pad's samples, and before the first
  release it also reads the rate over the newest 40 ms. At 66 g the launch is
  declared past Mach 1, and the first flag came at Mach 1.04-1.09. Every
  profile is now flagged by Mach 0.82, judged at the flag's own sample.
- A pad flag outlives a rise that falls back, for 10 s. The port-error sweep
  caught the first version clearing it mid-boost, at Mach 0.92, when a port
  faking a descent read the climb below the pad.
- `test_M1_mid_mach_releases` checks the flights that pass the flag. At the
  hot pad Mach 0.76 barely does, and a peak of 0.65 never would. Both pads'
  flights released within 2 s of burnout; after M2 corrected σ, 2.2-2.6 s,
  and the test allows 3 s.
- `test_M1_fallback` forces the release off with ports too noisy for any
  clean fit through the coast, not with a switch in the code.
- `test_M1_minimum_altitude_arm` and `test_M1_recovered_ascent_locked` are in
  `pressure_chain_tests`, which has the in-air boot.
- New: `test_M1_port_error_margin`, the sweep the design note's risks quote.
- The closed-loop gate test is now `test_FLT_MACH_02_fast_subsonic_flight_not_locked`,
  with its apogee held to within 1 s after the true one (was -2 to +3 s).
- The web UI's Flight Data summary still takes the log's highest row as the
  apogee, which on a locked flight can include the port error (N27).

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

**Done 2026-09-26** (DD-050), except the bench's `faults` list on a board
(G4). Before the change `test_M2_dropout_in_coast`, `test_M2_lost` and
`test_M2_reported` failed as they should. `test_M2_stuck_in_coast` passed as a
guard: T5's fit reads a stuck value as flat, and apogee wants the pressure
rising. `test_M2_out_of_range` and `test_M2_real_sensor_never_stuck` are
guards by design. Differences from the plan:
- The requirements are SNS-PRES-10 and SNS-PRES-11: T5 took SNS-PRES-09.
- A gap is an interval over 250 ms: longer than a flash stall and a sample,
  shorter than the 0.5 s dropout. One ending at the window's edge counts,
  because after a long gap the window holds only new samples.
- A suspect fit also cannot set the Mach flag, arm, or feed the emergency
  ladder. A stuck sensor coming back had flagged a supersonic climb, and the
  flight never deployed.
- Arming needs only "below 10 m/s": the 0-10 m/s window just before apogee
  closed for good after half a second of rejected readings (DD-017).
- Two corrections to T5 came with it, found by the closed-loop guard. σ is
  frozen at launch to its value from a second before T+0; the launch had
  inflated it by 15-90 % (`test_T5_sigma_ignores_the_launch`). After a
  charge, an unclean fit reading no lower than a ballistic continuation of
  the last clean fit is believed (PYR-MODE-06). The honest σ moved M1's
  release figures; `test_M1_mid_mach_releases` now allows 3 s after burnout.

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

**Done 2026-09-26 (DD-051), except the bench.** At the user's direction the
MS5607 converts once a loop: each loop takes the conversion the last one
commanded, whose one-shot alarm read it from RAM, and commands the next.
Temperature once in ten, carried to each pressure along its line.
- `test_T9_one_shot_cadence`: 900 pressures in 10 s under `mock_one_shot`, a
  20 ms gap only where the temperature was read, each taken 5 ms after its
  stamp.
- `test_T9_temperature_reuse`: 0.71 Pa RMS, 2.00 Pa worst, for a sensor
  warming at 1 °C/s; reusing the last reading as read, 12.1 Pa RMS.
- `test_T9_datasheet_example`: the compensation reproduces the datasheet's
  worked example (docs/datasheets/, page 8).
- `test_T9_drains` is not needed: one conversion finishes per loop, and the
  detectors read it in the same iteration.
- `support/prove_core0.py` proves the alarm handler RAM-closed, and fails on a
  planted flash call.
- At the user's rule, the driver provides every stamp: the MS5607's
  interrupt state machine commands, stamps and reads each conversion
  (`ms5607_tests`: the stamp survives a read held 60 ms and a loop 50 ms
  late), and `bmp280_read()` stamps its own reading.
- The history is 128 samples and the ring 64, so the fit keeps its whole
  second at ~90 Hz. At 90 Hz with the old 64 it held 0.7 s, and the pad
  speed was noisier than at 50 Hz (0.252 m/s against 0.192).
- `test_T9_same_outcomes`, at 11 ms sampling: pad speed 0.153 m/s; apogee
  -0.001 s from the drop over 100 flights, none early; SPEED within
  0.20 m/s; DELAY within 0.012 s.
- `test_T9_mach_at_90hz`: every fast profile, both pads, flagged before
  Mach 0.85 and released before apogee.

Each board runs its sensor bus as fast as its device and its PCB allow, set in
its board package (DD-052): 400 kHz on MK1A, MK1B's MS5607 and MK1C, whose
pull-ups were read from their design files; 100 kHz for MK1B's BMP280 probe,
whose SDA has none. `test_ms5607_ready_before_the_next_loop` fails at
100 kHz (ready 9.87 ms into a 10 ms loop) and passes at 400 kHz (9.29 ms).

**Bench, 2026-09-26, a second MK1B (serial 02E72A403441) flashed by the user
with 2.1.680:** the MS5607 at 400 kHz, `pres_rejects` 0 and `flash_refusals`
0 over four minutes. 47 % of loops waited: the next conversion was started
after the pressure's compensation, filter and fit (`stage_max_us[2]`
2.7 ms). Fixed by starting it first (`ms5607_async_cycle()`,
`test_ms5607_work_costs_no_samples`); to be flashed and measured again. The
same board overran its loop once a second: see D-B1 in section 6.

**Owed on the bench (G4):** bus errors and `pres_rejects` at 0 at 400 kHz on
every board; `sample_interval_us` near 10 ms, 20 ms where the
temperature was read; `stamp_lag_max_us` near 5.5 ms; `pres_waits` and
`pres_rejects` at 0; 0 loop overruns; `stage_max_us[2]`, and
`stage_max_us[7]` with the log at 90 rows a second; `flash_refusals` 0 with
Lua running on MK1C.

**T8 decided (DD-062, DD-064):** three plans under `log_rate`; only
`full` replays (DAT-08).

**Why:** every sample converts both pressure (D1) and temperature (D2), at
10 ms each. Temperature moves slowly. Converting it every tenth cycle nearly
doubles the pressure rate, which cuts the fit's noise and widens M1's release
margin.

**Tests first:**
- `test_T9_same_outcomes`: the key scenarios in `pressure_chain_tests` and
  `mach_tests`, at 50 Hz and 90 Hz, give the same outcomes within the new
  timing.
- `test_T9_short_interval`: the pressure task schedules correctly with an
  interval under 20 ms. Written, then retired with the idle time it tested:
  the one-shot has none (DD-051).
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

**Done 2026-09-26.** G5 passes with the summary table matching its counts:
220 verified, 12 on hardware, 20 not directly verified, 1 not implemented
(was 196, 12, 35, 1 when this list was written). Thirteen rows marked "—"
had tests that were never linked, or gained one:
`test_FLT_BOOT_02_reads_config_at_boot`, `test_FLT_BOOT_03_writes_default_config`,
`test_FLT_BOOT_13_no_calibration_samples_is_fault`,
`test_FLT_BOOT_14_no_filesystem_is_fault` and
`test_BUZ_CODE_12_missing_table_is_written` are new; they pass, since what they
check already worked. The 20 left are hardware only (the sensors, the HAL's
stamping and sample rate, the HTTP routes, USB and power), measured only on a
board (CPU time), or deferred (PWR-USB-01), and PWR-BUZZ-02's ENCODE state,
which the buzzer keeps private. N7, N9, N12, N23-N26 and REV-05 are marked
fixed in the resolution doc and here; `docs/flight_states.md` defects 2, 10,
14-17 and its Mach, sensor-failure, calibration and pad sections,
IMPLEMENTATION.md and `test/README.md` are brought up to date.

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
  GET answers 423 (DD-058), `/api/status` still answers, and the log is intact after
  landing.
- **Change:** `serve_file()` refuses while `hal_log_active()`.

### N12. A canopy approaching its rate from below can settle in the main band

**Done 2026-09-26, by T5.** `test_N12_drogue_from_below` flies drogues of 12
to 25 m/s opened at apogee, five seeds each, with no main configured: none is
ever reported as the main. It passes on T5's fit, whose speed does not trail
the rate the way the filter's did, so no change was needed and the test is
its guard. It could not be run on the code before T5: it uses T5's flight
options in the chain suite.

**Needs:** T5. Phase report only; triggers don't read it.
- **Tests first:** `test_N12_drogue_from_below`: a drogue that approaches its
  terminal rate from below is reported as DROGUE_DESCENT. Fails today in the
  simulator.
- **Change:** revisit the band test on T5's speed.

### N27. The Flight Data summary's apogee includes the locked interval

**Done 2026-09-26** (FLT-MACH-07). Three web tests on locked logs from the mock
server (`/api/_test/fly_locked/early|late|fallback`) failed on the old
summary, which showed the port's 13779.5 ft for a 10000 ft apogee.
`updateFlightSummary()` skips the rows between LOCK and UNLOCK or
LOCK_FALLBACK, and says "at least" after a fallback or a release within 2 s of
APOGEE.

**Needs:** M1. The web UI takes the highest altitude row in the flight log as
the apogee. On a locked flight that row can be the port's error: M0's draggy
flight with a port reading 15 % of q low logs 1976 m for a 1526 m apogee.
- **Tests first:** a web test on a mock log with LOCK and UNLOCK rows: the
  summary's apogee skips the rows between them, and says "at least" when
  UNLOCK falls within 2 s of APOGEE (FLT-MACH-07).
- **Change:** `updateFlightSummary()` in `www/app.js`.

### N7 / C8. The landing timeout declares LANDED under a main

**Done 2026-09-26** (FLT-LAND-07, DD-015). `test_N7_no_landing_under_main`
failed first: every flight under a 5 m/s main from 1.3 km was declared LANDED
in the air, 60 s after apogee. The timeout now needs stillness, under 2 m/s
held for a second on a sensor that has not failed, and lands the flight
1.7 s after touchdown, on the pad's level and 50 m above it. The host's
chain flight stands in for the simulator's 5000 ft one.

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

## 5a. No sleeps (DD-053)

The user's rule, 2026-09-26: no sleeps; the exec loop is the only clock, with
flash writes its only interruptions. `support/wait_check.py` holds the sites
still to convert, and fails CI if a new one appears or one is converted
without lowering the list.

| ID | Where | What waits | Runs |
|---|---|---|---|
| W1 | MK1B `pyro_board.c` | the continuity settle | **done**: a deadline the loop checks |
| W2 | `src/lua/lua_pio_platform.c` | the bridge's first word | **done**: the FIFO is empty, so a plain put |
| W3 | `boards/*/pressure_board.c`, `ms5607_detect()` | bus recovery clocks, pull-up settle, sensor resets | **done**: steps the loop runs during BOOT_SETTLE (`sensor_bringup_tests`, every board) |
| W4 | `ms5607_read()`, `pressure_sensor_read()` | two conversions | **done**: removed; nothing called them |
| W5 | MK1C `pyro_board.c` waveform capture | bias settle, the edge's lead-in, the DMA capture, the arm pump's FIFO | **done**: the capture is gone with the hardware checks (DD-055); the loop feeds the pump (F1, DD-056) |

Outside the check, recorded so they are not forgotten: the MS5607 one-shot's
wait for STOP inside its handler (at most 0.15 ms at 400 kHz), which an I2C
interrupt could replace; and, found on the way, MK1A's `pyro_init()` asserts
PYRO_LOW before the release claim, so a board with both channels released
keeps it asserted until Lua reconfigures the pad.

---

## 5b. MK1C firing path

### F1. Arm the firing bus and fire, on MK1C

**Done in code, 2026-09-26 (DD-056).** Added and finished the same day at the
user's request; one watchdog, the loop's, at the user's choice.

`pyro_fire()` runs DESIGN.md 7.1 and IGNITER_OPERATION.md F0-F10 as loop
steps in `boards/mk1c/pyro_board.c`, feeding the pump in
`boards/mk1c/arm_pump.c` once a loop:
1. Preconditions: the channel present on a tracking test since it last
   fired, no latched fault, the pack above 3.0 V, the bus not hot. A failure
   is a refusal with its reason.
2. Arm: the pump starts; each loop re-checks and tops the FIFO up. Five
   2 ms bursts carry it across one loop, and bound how far it runs past the
   last check.
3. Fire when the bus reaches 90 % of the measured pack; not there within
   1.5 times the ramp's time, abort and latch.
4. The pump stops with the gate; U9's enable bleeds in 9.6 ms.
5. The gate opens once the bus is flat after U9 is off, or at 30 ms.
6. The bus drains; the first tracking test after it reports the channel
   fired (open) or a misfire (still present, live). A misfire latches nothing,
   and the other channel may fire on the charged bus meanwhile.

There is no bulk capacitor (the requirement is dropped), so U9's current
limit is the pulse: about 4 A for the 9.6 ms its enable takes to bleed.

Also: no flash write during a fire (`board_flash_ok()`), and the bus-short
latch counts tracking tests, not loops (invariant 8).

`board_pyro_mk1c_tests`: 22 tests against the plant of the board as built,
on 2S and 1S. Requirements PYR-ARM-01..06 and PYR-CONT-04.

**Deferred 2026-09-27, at your direction:** "We will not do F1 anytime soon."
See section 9. What the bench fire would check:

**Owed, on the bench:** a flash (G4), then a supervised fire with a 1 ohm
pulse resistor on CN1 and the scope on the firing bus, before any real match.
Pass: `!PYRO FIRE` with the bus at 90 % of the pack or more, the gate held
no more than 40 ms (the hold ends on the loop after 30 ms: two loops at the
20 ms loop, DD-065), `!PYRO F10 ch=1 still present` (a resistor does not
open), and `pyro1_fired` on `/api/status`. Then the same with a stopped loop
(a debugger halt mid-arm): U9 off within about 35 ms (DD-065; 20 ms at the
old 10 ms loop).

---

## 6. Bench checks

Each check's pass criteria are its test. Record the result here and in the
resolution doc.

| ID | Check | Pass | Needs |
|---|---|---|---|
| U2 | Unplug a board from USB, then plug it back in | the pad announcement resumes within one repeat period (5 s); plugging back in gives exactly one double chirp | a person, 10 s, on MK1B or MK1C |
| REV-06 / REV-08 | Serial ground-test commands; a board in FAULT | FIRE while the other channel pulses answers `GT,ERR,busy`; a FAULT board sends `!FAULT <diagnosis>` every 5 s and no `$PYRO` | a USB-serial adapter on the TRRS jack |
| REV-18 | The filesystem lock on hardware (DD-058) | in test mode, once a chamber pump-down declares a launch: `POST /www/x` and `GET /www/app.js` answer 423, `/api/status` 200; a page load across the launch is reset, not left holding a mount; after LANDED the log reads back whole; `POST /api/reboot` in flight reboots | test mode, the chamber |
| — | The USB network after the Mac sleeps and wakes | the board answers `/api/status` within 10 s of wake, without replugging | a person. The v2.1.50 fix is gone, so today's behaviour is unknown |
| — | Chamber runs for T1–T7 | pump-down, hold and vent in test mode give launch, apogee and landing as the host tests predict, with no false launch during the hold | the chamber |
| T1 | Recovery reads samples on the hardware | a board with a marker, booted on battery with USB plugged in afterwards, reads "cold: at ground level" | a battery |
| T1 | Brownout recovery on the real path | a power cut during a chamber descent rejoins in FALLING; a power cut on the pad stays cold | a battery, the chamber, telemetry over serial or radio (USB forces a cold boot, and a reset ends test mode) |
| N11 | LUA and MOCK rows on the flight clock | in test mode, a script that calls `log()` once a second through a chamber flight writes LUA rows whose times fall among the sample rows', not near the board's uptime | test mode, the chamber, MK1C with Lua |
| D-C1 | MK1C's R_BLEED, open, hides from the bus level | U9's reverse path carries the bus either way (685 against 730 counts), so no level check can find it (DD-054). **Closed, not wanted:** the firmware checks only presence and shorts (DD-055) | — |
| CI-1 | Six host suites never run in CI | **Done 2026-09-28:** `pin_caps_tests`, `beep_tests`, `pin_assign_tests`, `buzzer_tests`, `config_tests` and `config_persistence_tests` now run in CI, with the ground test's two suites | -- |
| G4-L | MK1C overruns under G4's own load | **Done 2026-09-27 (DD-061).** G4 asks for 0 loop overruns on MK1C with Lua running. Under the checks' HTTP load MK1C logged 1-4, on 2.1.681 and 2.1.683 alike: STAGE 1 peaked at 7.7 ms, 7.4 ms of it HTTP handlers. HTTP now runs as work units from the slack, or on core1; the loop's head only moves bytes. All four bench boards on 2.1.688 through G4: 0 overruns, STAGE 1 peak 2.1 ms at most, every check passing | -- |
| G4-M | The pad Mach flag rises under upload load | **Cause found 2026-09-27.** Not the stamps: MK1B's MS5607 is disturbed electrically. With every conversion traced through ten G4 rounds, the scatter rose from 9 Pa to 21-25 Pa while `api_check.py` ran, in two stretches: its flash writes, and test mode, where the buzzer announces the pad verdict. A beep code alone, with no flash write, takes it to 31-38 Pa for as long as it plays (`/api/beeps/play`). The flag's short rate over two intervals then trips about once in 20 beep codes at the 20 ms loop, and nearly every code at the old 10 ms loop: G4-M's 6 in 12. Flash writes: conversions a write overlapped are now discarded (DD-068). The buzzer: MK1B's only -- MK1C shows no effect, though both switch it with a MOSFET -- so a board change (section 7, B-BZ) or **your decision** on the short rate (section 10, M3). No flag in 20 G4 rounds on 2.1.697-698, nor through G4 on all four boards on 2.1.701 | your decision |
| G4-N | A 40-60 s HTTP outage, once | **Done 2026-09-27 (DD-070).** `/api/net` on one G4 round: 40 % of frames refused at the USB endpoint and left to lwIP's retransmit timer (3 s, doubling), lwIP's heap out 27,865 times at 7,832 of 8,000 bytes. A stalled response, then refused connects while ping answers, is what that produces. Frames the endpoint cannot take yet are now held and sent (2.4 % refused on the next round; 14-43 per board through G4 on 2.1.701). Before the fix a one-hour soak of three boards under the trace poller alone (about 32,000 requests each) met no outage: it needs G4's parallel load. On the fix, a 30-minute soak of all four boards (about 16,000 requests each, five G4 rounds meanwhile on the bench MK1B) met none. lwIP's heap size is **your decision** (section 10, N1) | -- |
| S1 | The BMP280 at the loop's rate | **Done 2026-09-27 (DD-067),** at your direction: "S1 needs to make the BMP280 match the loop rate." One forced conversion a loop, x4/x1, taken at the next, stamped from its command; every transfer bounded (DD-069). MK1A traced on 2.1.701: 50.0 a second, consistently good, lag 11.9-13.1 ms | -- |
| S2 | The MS5607's noise and its temperature step | **Found 2026-09-27 (DD-063).** White noise 6.5-9.3 Pa on all three boards against the datasheet's 2.4 Pa at OSR 4096, unchanged at half the duty; and the pressure right after each temperature conversion reads about 4 Pa high, relaxing over 90 ms. Both are in the raw codes. **The step is gone (DD-066, 2.1.697):** with a temperature before every pressure each sits at the same distance from one, and the residual folds flat (MK1C 6.96 Pa to 6.43). The white noise remains, 6.4-9.1 Pa. **Your note (2026-09-27):** the sensors run in ambient air on the bench, so part of it may be ordinary pressure noise. **Found the same day:** MK1B's buzzer, sounding, takes the MS5607's scatter from 9 to 31-38 Pa, pressure and temperature codes alike, so it is electrical; flash writes do the same, less (DD-068). Worth checking: the VDD decoupling (the datasheet asks 100 nF at the pin, page 17) and bus activity during conversions (page 6) | the boards' design files, and the trace |
| D-B1 | MK1B's continuity check stalls the loop | **Passed 2026-09-27** on 2.1.681: the second MK1B, owning its pyros, 0 overruns (79,233 on 2.1.680), `stage_max_us[3]` 4.1 ms. The reading it makes is B-U5's | — |
| T5 | The fit's cost, and the pad's σ, on each board | **Measured 2026-09-27**, at rest after a reboot: `stage_max_us[2]` 2.9 ms on both MK1Bs and MK1C, 2.1 ms on MK1A, steady from the first seconds on the pad -- over the 500 µs allowance (T5-C); 0 overruns. `fit_sigma_mpa` MK1A 1495-1508, MK1C 4314-4480, both MK1Bs 5000, the ceiling (`PP_SIGMA_CEIL_PA`): the MS5607 boards read about twice the datasheet's 2.4 Pa at OSR 4096 (MS5607-02BA03 p.4) on this bench, the MK1Bs at least that | — |
| GT-1 | The ground test procedure on the bench (DD-071) | wired both ways in turn -- a switch to ground, then across two pads (MK1A's J6 18 and 19) -- and pyro modes set: powered up closed, the alert; opened after it, a count a second from five, the pulse on channel 1 at zero, 3 s of tone, a second countdown and the pulse on channel 2, three long beeps. Closed again mid-countdown, nothing fires. Powered up open, the pad as usual | a person, dummy loads, a scope or meter on the firing pads |
| N20 | No file served while the log is written | in test mode, once a chamber pump-down declares a launch, `GET /www/app.js` answers 423 and `/api/status` 200; after LANDED the log reads back whole | test mode, the chamber |
| — | The arming path independent of software | with the mechanical disconnect in, a commanded ground-test FIRE puts no current through a dummy load, on each board | a dummy load and a meter. The Mach prompt asks for this path; the operator narrative uses a mechanical disconnect, but no document says what it breaks |
| ID-1 | **The second MK1C shares the first's identity** | **Done 2026-09-29:** the second MK1C carries `/serial.txt` 02373331FF2A and answers at 192.168.42.1 beside the first at 192.168.222.1, both with TTL 255; `register_board.py` now keys a record by hw_id and MAC (it had merged the two) and lists shared flash ids. **Found 2026-09-28.** Flashed blank with 2.1.703, it enumerates as 02373331FFDE, the first MK1C's serial, and macOS gives it no network interface. Neither board has a `/serial.txt`: the XT25F128F ids match (`boards/mk1c/THEORY_OF_OPERATION.md`, "Known limits"). Fix on the bench: unplug the first MK1C, `POST /api/serial` 12 hex digits (02 first) to the second at 192.168.222.1, reboot it, plug the first back in, run `register_board.py`. Pass: both answer, on two subnets. **Your decision** for later MK1Cs: provision each with a serial, or have the firmware make one at first boot (the ring oscillator's random bits) and keep it in `/serial.txt` | your decision |
| SPI-1 | The J3 SPI test board: an SD card and an LSM6DS3 (`boards/mk1c_sd/THEORY_OF_OPERATION.md`) | the LSM6DS3 reads WHO_AM_I 0x69 with I2C_disable set; the card answers CMD0 and initialises; `flash_refusals` 0 and no loop overrun while both run; the MS5607's scatter on the trace unchanged while the card writes | **Half done 2026-09-29** on 02373331FF2A (MK1C-SD image): the LSM6DS3 reads at 1.66 kHz with no overrun. The card answers CMD0, CMD8 and CMD59, then resets 27-41 ms into ACMD41 whatever the CRC setting or poll interval, and never initialises: the card's supply is the suspect (power the module from 5 V if it has a regulator, or 10-47 uF and 100 nF at the card). `POST /api/sd/init` retries without a reboot. |
| SD-1 | The SD card's limits, and the high-rate log over a flight | `/api/sd/bench` across chunk sizes, then `BOARD_SD_SPI_HZ` raised toward 25 MHz; a bench flight (`support/bench_flight.py`) with `/api/hr` showing no dropped records and no IMU overrun, decoded by `support/hr_log.py` | SPI-1's card |

The Mach lockout can't be checked in a chamber, because it needs supersonic
flow. `mach_tests` is its only check short of a flight.

---

## 7. Board changes

| ID | What | Tests first |
|---|---|---|
| U1 | Detect a charger on USB (USB-06). VBUS reaches only the TP4057 on MK1A, MK1B and MK1C. The options are a VBUS divider to a spare GPIO (MK1C has GPIO2–5, 9, 10 and 13–15 free; GPIO24 on MK1A/B), or routing the charger's CHRG/STDBY pins to GPIOs. Firmware would OR either with the USB frame check. | Host, with a mocked VBUS input: a charger alone counts as attached; a sleeping PC (VBUS, no frames) counts as attached (U3); no input counts as detached. Bench, on the new board: a charger stops launch detection. |
| B-U5 | **MK1B cannot sense continuity.** U5 is AP2192AMPG-13 (LCSC C507872) on both JLCPCB builds: the AP2192A's output discharge, about 100 ohm while disabled, holds SENSE1 and SENSE2 near 0 V against their 100k pull-ups, so a fitted igniter, an empty connector and a short all read the same. The base AP2192, AP2192MPG-13, has no discharge and the same pinout, enable polarity and MSOP-8EP drawing (DS31569, pages 1, 4, 15): a swap on the same pads, no firmware change. DigiKey had 2,418 on 2026-09-28; its attributes list "load discharge" and 85 mOhm, the A part's figures, against the datasheet. Found on the bench 2026-09-27 (DD-059). | Bench, on a board with the base part: an empty connector reads open (about 4095), a 1 ohm load good (about 0), a short to ground shorted -- `board_pyro_tests` already holds the firmware to that |
| B-BZ | **MK1B's buzzer disturbs its MS5607.** While a beep code plays the sensor's scatter rises from 9 to 31-38 Pa, both codes, and the Mach flag's short rate trips about once in 20 codes (DD-068). MK1C shows none. Both boards switch the buzzer from VIN with a MOSFET (MK1B's Q1A, MK1C's Q2), so the difference is not the drive; the cause is not established. To look at: the buzzer's return path and VIN's decoupling near the sensor, and the sensor's own decoupling (the MS5607 datasheet asks 100 nF at its pin, page 17) | Bench, on the changed board: `scratchpad buzz_test.py`'s three beep codes leave the trace's scatter at its quiet level |
| B-FL | **MK1B's BOM names the wrong flash.** Both builds' production files give U7 as W25Q64JWUUIQ (LCSC C6604692), a 1.7-1.95 V part; the rail is +3.3 V and the boards carry a W25Q16JV. The KiCad design now names U9, an XT25F128FWOIGT-W (16 MB, 3 V), on a WSON-8 6x5 footprint. For the USON 4x3 footprint and the boot stage 2 as it is, the BY25Q64ESHIG(R) (8 MB, 3 V) suits on paper, and was the one such part DigiKey had in stock on 2026-09-28; GigaDevice's GD25Q-E parts fit the footprint but need another boot stage 2; `boards/mk1b/THEORY_OF_OPERATION.md` "Flash" | A larger chip is a board variant: its `PYRO_FLASH_SIZE_KB`, and a check of its boot stage 2 on the bench |
| REV-03 | MK1C cannot fire. **Done in code (F1, DD-056).** | The bench fire into a dummy load that sets `pyro1_fired` and shows the pulse on a scope (F1's bench check). |
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

**Done 2026-09-26.** Both tests failed first. `scripts/sync_demo.sh --check`
found `docs/app/` months behind `www/`. The headless run found the demo
unflyable: `docs/sim.html` still numbered the states as they were before the
descent phases and BOOT_SENSOR, so it enabled Launch at ASCENT (4) and
waited for LANDED at CHUTE_DESCENT (7), and a visitor could never launch. The
page now names `flight_state_t` in its order, the WASM module is rebuilt from
today's source, and `docs/app/` is resynced. CI runs both checks: "Demo
matches www" and "Browser simulator". The rebuilt module is still one a
person has to remember to rebuild; nothing checks that it matches the
source.

`docs/app/` was last synced in March (N19), and the WASM simulator was never
rebuilt after the simulator fixes (N6). `emcc` is now installed.
- **Tests first:** after `scripts/sync_demo.sh`, `docs/app/www` matches
  `www/`, with a CI step that fails when they drift; a headless run of
  `docs/sim.html` flies from boot to LANDED.
- **Change:** run `scripts/sync_demo.sh` and `scripts/build_wasm.sh`.

### H3. Found reviewing the documents, 2026-09-28

Open. The review corrected every document; these are defects in code and
tools it found, which a document cannot fix.

| ID | What | Checked |
|---|---|---|
| H3-1 | `docs/wasm/pyro-sim.js` numbers the states as they were before the descent phases (`LANDED: 7`) and calls `_sim_clear_telemetry`, which `scripts/build_wasm.sh` does not export | read |
| H3-2 | `scripts/build_wasm.sh` fails for the `sim_mk1*` variants and leaves out sources they need | reported by the review, not rerun |
| H3-3 | `sim/qemu/sync-plant.sh` does not copy `src/pyro_sense.h`, which `plant_mk1c.c` includes | read |
| H3-4 | `support/flash_picotool.sh`, `install.py`, `update_from_release.py` and `test_network.py` default to 192.168.7.1; a board's address is 192.168.N.1 with N from its unique ID | read |
| H3-5 | `support/install.py` looks for the MK1C image, then MK1B's, and never picks MK1A's | read |
| H3-6 | A release ships only the MK1B image, and an OTA upload is not checked against the board it lands on | reported by the review, not rerun |
| H3-7 | `support/pressure_trace.py` is not executable | read |
| H3-8 | `hal_pressure_fifo_get()` and its 5-sample batches in `hal_common.c` have no caller | read |
| H3-9 | **Answered 2026-09-28.** MK1B's U7 is marked Q16JVUUIQ: a 3 V W25Q16JV, 2 MB, on +3.3 V. The production BOMs name a 1.8 V W25Q64JWUUIQ, which was not fitted; B-FL | your reading of the chip |

Requirements the code does not meet as written. Each needs the requirement
rewritten to what was decided, or the code changed; `TRACEABILITY.md` still
marks the first, third and fourth verified.

| ID | Says | The code |
|---|---|---|
| DAT-01 | a ring of at least 4096 samples | `FLIGHT_BUF_SIZE` is 64; the flight record is `flight_log.bin` (DD-062) |
| FLT-RATE-02 | pressure delivered in batches of 5 | one sample a loop; the batch API is unused (H3-8) |
| FLT-RATE-03 | sampling cut to 1 Hz in LANDED | sampling stays at 50 Hz; the log keeps a row a second (N18) |
| TST-05 | closed-loop tests from 100 ft to 100 km | `test_data/rockets.json` flies 65 m to 950 m |

---

## 9. Deferred, and not planned

### The MK1C bench fire (F1)

Deferred 2026-09-27, at your direction: "We will not do F1 anytime soon." The
firing path is done in code and tested against the board's plant
(`board_pyro_mk1c_tests`); section 5b's F1 keeps the pass criteria for when
it is taken up, with DD-065's 20 ms-loop timings (gate held up to 40 ms,
disarm about 35 ms).

### False-launch reversion

Not scheduled, at your decision (2026-09-25).

A board that declares a launch but is never armed, and whose altitude returns
to near the ground and stays there for several seconds, would go back to
PAD_IDLE. This is safe because the pyros are never armed before the arming
gate.

Today such a board sits in ASCENT until it is power-cycled (defect 2, N24):
- its flight log stays open;
- the API stays live, so a reboot clears it (C5, DD-058);
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

---

## 10. Logging and lockups in flight

Opened 2026-09-27 at your direction: "There must never be a lockup in
flight. Logging must not disrupt flight operations." From an audit of the
flight path, checked against the code, and an analysis of moving the flight
code into RAM, which you asked for without changes.

**2026-09-28:** you adopted R1, with a second memory for logging on future
boards. The options, against five hardware changes (an SD card on MK1C, an
ESP32-S3, an RP2350 with a second flash or a PSRAM), are designed in
`docs/log_storage_options.md`; its section 7 lists the decisions.
You then asked for a second plan: the flight code, Lua and a log writer as
FreeRTOS SMP tasks, the writer blocking the whole system for each flash
write, as a step toward the ESP32. It is `docs/log_storage_plan2_freertos.md`;
its section 13 lists the decisions, the first being plan 1 or plan 2.

**Done the same day:** every sensor bus transfer is bounded (DD-069: the
BMP280's, the MS5607's detection and MK1B's BMP280 reset waited forever on
a held bus, and bring-up runs again after a reset in flight); conversions a
flash write overlapped are discarded (DD-068).

| ID | Risk | Evidence | Needs |
|---|---|---|---|
| L1 | A watchdog reset in flight never deploys | `WATCHDOG_MS` is 1000 (`PYRO_LOOP_WORST_MS` 500 on every board). A watchdog reset comes back cold (`test_BRN_01_software_reset_never_recovers`), and a cold board in the air needs a new 100 ft climb to launch. Nothing bounds how many erases one littlefs call makes, and each may take the flash part's worst case | the logging design |
| L2 | The log erases flash about once a second in flight | Each 1 s sync (`LOG_SYNC_MS`) makes the next write copy the partial last block into a freshly erased one (`lfs_ctz_extend`). An erase holds core0 with interrupts off: 46-73 ms measured (DD-035). It delays the loop, and so a fire decided just after it, and costs the conversions it overlaps (DD-068). No guard near apogee or deploy | the logging design |
| L3 | C6 is not implemented, and a recovery truncates the flight's own log | The log is opened with `LFS_O_TRUNC` in the first window after launch detection (`hal_common.c`); the brownout-recovery path calls `hal_log_start()` too (`flight_states.c:428`), which erases what DD-035's syncs kept. The pad announcement does not wait for the log | the logging design |
| L4 | A write failure in flight | littlefs can take part of a write and still fail it; the retry writes the part again and the CSV stops at the misaligned record. After an error `lfs_file_sync` returns success without writing. No counter | the logging design |
| L5 | USB in flight | With a host attached (test mode, the chamber) OTA erases, reboots and BOOTSEL are obeyed in flight | a decision: refuse them in flight |
| L6 | MK1C: a stall during precharge latches a fault | A core0 stall over about 35 ms during precharge times out and latches `FAULT_PRECHARGE_TIMEOUT`, refusing both channels thereafter. No flight source of such a stall once the window is shut, but no test of the latch after the loop resumes | a test |
| R1 | Moving the flight path into RAM | **Analysis, 2026-09-27:** it fits. About 38 KB of code and 2 KB of constants move per board; 50.6 KB of SRAM is free, 7-10 KB left after, once printf leaves the flight path. Recommended: from launch to landing core0 runs only RAM code, and core1 does the log's flash writes as units core0 hands it (Lua pauses during them). The USB and alarm-pool interrupts are masked while a unit is out, an MPU guard faults any missed flash access, and `prove_core0.py` proves the flight closure RAM-closed. About 13-19 engineer-days. The whole image cannot run from RAM: code and constants alone exceed SRAM | your decision |
| M3 | The Mach short rate and the buzzer | On MK1B a beep code trips FLT-MACH-02's two-interval rate about once in 20 codes (DD-068); MK1C shows no effect. docs/mach_lockout.md already accepts a false flag as the safe direction: the lock releases after a second of clean coast, and near apogee the fallback deploys a second or two late. Options: a 5-sample slope (80 ms, about 20 ms more lag, noise at 35 Pa 5.3 sigma below the threshold); discarding beep-overlapped samples (blinds the pad detector for up to 5.5 s at a time); or the board (a decoupled sensor supply). The flag's rate path exists for 66 g boosts, so its lag is the safety margin | your decision |
| N1 | lwIP's heap | 8,000 bytes against a 5,840-byte send buffer per connection: two or three streaming connections still exhaust it (4,493 refusals in a G4 round after DD-070). More heap spends RAM R1 would need | your decision |
