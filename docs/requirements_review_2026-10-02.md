# Requirements review, 2026-10-02: rulings and dispositions

The record of the review of `REQUIREMENTS.md` held on 2026-10-02 and
2026-10-03. `REQUIREMENTS.md` was rewritten from this record, with DD-080 to
DD-090 in `DECISIONS.md`; the tests and then the code follow.

Each item is marked **R**, ruled by the user, or **P**, proposed in the
review and not objected to, still to be confirmed.

## 1. Standing rules (all R)

1. **Never early.** A fire needs measured evidence that its event has
   happened. No estimate, no fallback timer: a timer needs a valid time,
   there is no safe default, and a proper value needs an accurate
   simulation.
2. **Data is believed.** The board cannot know its sensor is wrong. Noise is
   filtered, and filtering may delay a decision by a bounded time. While
   samples arrive the flight decides and fires. A model improves the
   estimate and never overrides the data.
3. **Only the absence of data suspends a decision.** A stuck reading is a
   chip failure and counts as no data: it is logged and the sampling goes
   on. The flight software does not reset or retry a failed sensor.
4. **Best effort.** Once a fire is decided nothing withholds the attempt.
   Health, continuity, faults and voltage gate nothing, in flight or in
   ground test. The system keeps attempting deployment while the measured
   descent shows it has not worked.
5. **A safe flight carries two pyro systems.** This unit prefers no
   deployment to an early or uninformed one; the backup covers that flight.
6. **Pad announcement.** Exactly four: GO, pyro 1 fault, pyro 2 fault,
   general fault. Silence is also a fault. One is announced at a time, by
   priority: general, pyro 1, pyro 2. A general fault goes to the
   workbench; a pyro fault may be fixed at the pad. Diagnosis is on the
   status report, never in the sound.
7. **No channel roles.** "Drogue" and "main" leave the requirements. The
   operator's configuration decides what a channel deploys.
8. **Only enabled channels count**, for firing and for diagnostics. The pin
   assignment is the source of truth: a channel whose pads belong to the
   script is not a pyro channel, and any pyro configuration left for it is
   stale and ignored.
9. **Requirements state behaviour, never mechanism.** No CPU, kernel, DMA,
   core, interrupt or algorithm is dictated. The hardware layer may use
   any of them. FreeRTOS is the only task model supported, as a design
   fact.
10. **General first, board beneath.** The pyro requirements are general.
    Board details depend on board capability. A board may redefine defaults
    and constrain values; it may never withhold a decided fire. A board
    that cannot detect a fault assumes none and fires. Sensor limits and
    log details are board constraints.
11. **Any restart resumes a flight in progress.** All resume state is
    cleared after the flight.
12. **Power** is minimised wherever that affects no functional requirement.
    Battery sizing is the user's task.

## 2. The fire rules (R unless marked)

Configuration fields:

| Key | Default | Zero |
|---|---|---|
| `pyro1_refire_speed` | from the study | pyro 1 does not re-fire on its threshold |
| `pyro2_refire_speed` | from the study | pyro 2 does not re-fire on its threshold |
| `emergency_fire_speed` | from the study | no emergency all-fire |
| `refire_interval` | 1 s, board may redefine | not allowed (P) |
| `fire_gap` | 3 s, board may redefine | not allowed (P) |

- **Re-fire.** After a channel's first fire, while the descent speed
  exceeds that channel's re-fire speed, the channel fires again every
  `refire_interval`.
- **Emergency all-fire.** At any time after apogee, when the descent speed
  reaches `emergency_fire_speed`, every enabled channel fires, whether or
  not it has fired before, and fires again until the speed drops or the
  flight lands.
- **Serialisation.** The channels are never energised together. At least
  `fire_gap` separates a pulse on one channel from a pulse on the other.
  100 ms is too fast. With both channels firing, the gap governs and each
  fires once per two gaps.
- **Speed.** The speed compared is filtered, and judged in the pad's air
  (FLT-AIR-01).
- **R:** the gap is the quiet time between pulses, from the end of the
  first pulse to the beginning of the second.
- **R:** the re-fire and emergency rules operate on the filtered state. It
  is the best knowledge the system has, so acting on it is best effort; the
  raw data behind it is noisier and knows no more.
- **R:** a first fire goes before a re-fire. The pulses are short, and the
  gap would otherwise hold a first fire back by seconds.
- **R:** on a tie between two first fires or two re-fires, pyro 1 goes first.
- **P, board rules:** each board declares the default and permitted range
  of every pyro field; a value outside the range is brought to the nearest
  permitted value and reported; the ranges are served to the web interface.

## 3. Estimator and altitude (R unless marked)

- The Mach lock sets early under boost, after a clean launch indication,
  and releases before apogee. A model filter is useless until the release;
  from there it stabilises the trajectory and tracks the deceleration to
  apogee, discarding readings off the predicted trajectory.
- The design direction: a Kalman filter tracks the extra noise at altitude
  and runs through it. The requirement stays behavioural: apogee within a
  stated lateness, and a speed with a stated noise bound.
- A modelling study is to identify which values are configuration, their
  reasonable defaults, and the maximum safe flight altitude.
  `docs/descent_speed_estimator.md`.
- Altitude processing may have limits; beyond them the system operates on
  best effort. Out of range the trajectory will be distorted and the
  deployment comes at some notion of apogee, likely by emergency fire.
- **R:** every operation is in pressure. Altitude is for operator I/O only.
  The study settles the conversions. `docs/pressure_domain_flight_math.md`.
- Temperature is logged wherever raw pressure is. Converted outputs,
  telemetry included, need none. A temperature outside its limits costs
  accuracy and discards nothing.
- **R:** the fallback cannot wait for the pressure to return to the level
  the flag was set at; that deploys near the ground on a good flight
  (HA-1). Single samples may be wild in thin air, but filtered pressure
  shows the slowing climb, apogee and descent at every height, and the
  system finds apogee from it within a stated time of the true apogee.
  The filtering may lengthen as the air thins; that is design. FLT-MACH-04
  as written is rejected.
- **R:** the Kalman filter takes raw readings, limits the correction inside
  its update, and will need tuning (`docs/kalman_launch_evaluation.md`).
  Decisions are made on its filtered state; no decision waits without
  limit for clean data while samples arrive.

## 4. Dispositions

### Section 1, recovery deployment

| Item | Disposition | |
|---|---|---|
| UN-1 | add the two-systems assumption | R |
| SYS-DEPLOY-04, -05 (new) | rules 1 to 3, and rule 4 | R |
| PYR-MODE-05 | stands; DELAY counts from the declaration when no zero crossing was seen | P |
| PYR-MODE-06 | stands; "restarting at each charge" dropped | R, P |
| PYR-SAFE-01 | becomes pad health; gates no fire | R |
| PYR-DEPLOY-01 | stands, without roles | R |
| PYR-DEPLOY-02 | rewritten on `fire_gap` | R |
| PYR-FIRE-01 | records the attempt and what the board observed; no refusals | R |
| PYR-SAFE-03 | no fire before a channel's first trigger; repeats follow the fire rules | R |
| PYR-SAFE-04 | stands | R |
| FLT-LAUNCH-01 | folded into FLT-LAUNCH-07 | P |
| FLT-ASC-07 | stands: a knowledge gate, not a health gate | P |
| FLT-LAND-01 | withdrawn | R |
| PYR-REFIRE-01 | replaced by the re-fire rule | R |
| PYR-REFIRE-02 | withdrawn | R |
| FLT-EMRG-01 | replaced by the emergency all-fire | R |
| FLT-EMRG-02 | withdrawn | R |
| FLT-EMRG-03 | withdrawn: the tests are absolute speeds | P |
| FLT-EMRG-04 | EMERGENCY_FIRE in place of MAIN_FORCED | P |
| FLT-BROWN-01..06 | become the resume rules, section 5 | R |
| FLT-MACH-04 | rejected as written; apogee from the filtered trend at every height, within a stated lateness | R |
| FLT-AIR-01 | stands; applies to all three speeds | P |
| PYR-ALT-01 | withdrawn: no setting is clamped | R |
| PYR-ALT-02 | withdrawn: not a beep | R |
| BUZ-CODE-01, -02 | "general fault"; priority general, pyro 1, pyro 2 | R |
| BUZ-CODE-13 | defaults named pyro 1 and pyro 2 | R |
| FLT-BOOT-15 | every fault on the status report; one announced | R |
| FLT-BOOT-16 | replaced by the one definition of an enabled channel | R |
| FLT-RATE-02 | withdrawn, with PWR-SAMPLE-01 and -02; confirm in code | P |
| LUA-IO, PIN-LABEL, PIN-BUZZ | moved to their own sections | P |

Everything else in the section stands, with mechanism moved to the design
record under rule 9.

### Section 2, pre-flight status

| Item | Disposition | |
|---|---|---|
| PYR-CONT-02 | ready or fault; "short" leaves the general requirement, since a short cannot be told reliably from a match | R |
| PYR-CONT-04 | board section; a diagnostic | P |
| BUZ-STATUS-01 | withdrawn | R |
| Health (new) | each board checks every enabled channel for the faults it can reliably detect, at least once a second; a board that cannot detect one treats the channel as ready | R |

MK1B cannot sense its channels and reports GO until its board fix removes
the 100 Ω bleed.

### Section 3, flight data

| Item | Disposition | |
|---|---|---|
| DAT-01 | withdrawn | R |
| Log capacity (new) | each board holds a log of the maximum flight duration and declares its capacity; log details are board constraints | R |
| Raw records (new) | any record holding unconverted pressure holds its temperature | R |
| DAT-03 | an event is a sample row at the event's own time | P |
| DAT-04 | REFUSED and MAIN_FORCED out; REFIRE per channel, EMERGENCY_FIRE, SENSOR_STUCK, SENSOR_LOST and a restart event in | P |
| BUZ-07 | withdrawn, duplicate of FLT-LAUNCH-05 | P |

### Section 4, configuration

| Item | Disposition | |
|---|---|---|
| CFG-02 | refers to the field table | P |
| SYS-CFG-03 | sensor and board limits | R |
| FLT-BOOT-03 | withdrawn, duplicate of CFG-05 | P |
| Pyro fields (new) | section 2 above | R |

### Section 5, altitude measurement

| Item | Disposition | |
|---|---|---|
| SNS-PRES-06 | discards only what the sensor cannot produce; the board declares its limits | R |
| SNS-PRES-10 | stands: logged, no recovery | R |
| SNS-PRES-11 | stands: logged, no recovery | R |
| SNS-PRES-02, -03, -07, -09, -12 | algorithm, to the design record; the behaviour kept is that one bad reading changes no decision and the speed's noise is under a stated bound | R, P |
| SNS-PRES-09's 5 Pa cap | board-declared | P |
| SNS-ALT-02 | withdrawn: nothing is clamped to 8000 m. The figure came from the limits of the altitude conversion, and every operation is now in pressure. A simple conversion may be wrong at high altitude; that is a reporting error and does not affect flight | R |
| SNS-ALT-03 | withdrawn: every altitude is relative to the launch pad, and a lower landing point may show negative | R |
| SNS-ALT-04 | withdrawn with the clamps it fenced in | P |
| Maximum altitude (new) | the height for proper operation, from the study; best effort above it | R |

### Section 6, telemetry

| Item | Disposition | |
|---|---|---|
| TEL-01 | $PYRO is the format. Another format is a script on serial pins assigned to it | R |
| SYS-TELEM-FMT-01, TELEM-FMT-01..03 | withdrawn, with the `telem_format` key | R |
| TEL-03, TEL-04 | telemetry runs at 1 Hz in every state it is sent in; the `telem_rate_hz` key is withdrawn. Logging may run faster | R |
| Events on the downlink | queued into the telemetry; the next message carries the event, so a report may be up to 1 s late | R |
| TEL-07 | names kept as labels for a rate band | P |
| TEL-08 | "continuity" becomes "ready" | P |

### Section 7, pyro fault protection

Board capability throughout (R). A board's protection may end a pulse and
shall never prevent the next attempt. PYR-ARM-02 is withdrawn. PYR-ARM-03
drives the gate at its deadline whatever the bus reads. PYR-VERIFY-01 is a
record only. The behaviour kept from PYR-ARM-01: software that stops
running leaves no channel armed.

### Section 8, web interface and network

| Item | Disposition | |
|---|---|---|
| WEB-NET-02 | the subnet follows the address, as WEB-NET-06 | P |
| WEB-HTTP-05..07, WEB-API-11's "core0" | to the design record | R |
| Reboot and firmware update in flight | no requirement and no code: neither can arrive in a real flight | R |
| The POST guard | design, not a requirement | R |

### Sections 9 to 15

- OTA stands.
- Sections 10 to 12 are project requirements, stated without a processor.
  RTOS-01 and -02 go to the design record (R).
- Section 13 is one requirement, SYS-PWR-01 as rule 12 (R). FLT-RATE-03
  goes to the design record under it.
- Section 14 is withdrawn (R).
- Section 15's field table is a project requirement (P).

### Section 16, ground test

| Item | Disposition | |
|---|---|---|
| GND-TEST-01..04 | withdrawn: the serial commands are removed | R |
| SYS-TEST-01 | ground test is by the test switch | R |
| GND-TEST-05..11 | stand; fires on command | R |
| GND-TEST-12 | the switch may connect the buzzer's pad to another pad; it may not ground the buzzer. Pad to pad, the input pad watches the buzzer's pattern, and the switch is closed when the input follows the buzzer output's state. A series of short pulses may be used to find the switch's position | R |
| Configuration (new) | configuration and pin changes take effect at start-up: the web interface only writes the file, and the file is loaded when the board starts. Ground test mode runs on the configuration it started with; a reboot is needed to change it | R |

### Sections 17 to 19

USB-06 is withdrawn (P). The card and high-rate log move under the board's
log constraints (R). The bench flight stands; its profiles gain failed
canopies (P).

## 5. Resume (R unless marked)

- After any restart the system determines whether a flight is in progress
  and resumes it.
- A resumed flight assumes no channel has fired and fires as soon as fresh
  sensor data dictates.
- All resume state is cleared after the flight.
- The state may be restored by replaying the log if that is fast enough.
  `docs/resume_from_log.md`.
- Resume is best effort. DELAY counts its full value from the resume. The
  emergency rules may trigger earlier than they would have.
- **R:** a flight is in progress when the board is above the recorded
  ground and moving.
- **P:** the restart is logged.

## 6. Lua (R unless marked)

- Lua requirements are functional requirements on every board.
- It must not be possible for a script to block any board behaviour:
  booting, the network, the flight functions.
- A script has full control of the pads assigned to it. A pyro's pads
  assigned to it could implement a pyro function, and that is the
  operator's.
- The enabled program always runs, ground test included.
- **R, open to future edits:** the drafted items: read-only flight state, no file access, one
  owner per pad, resources by name and kind, the web editor and console,
  board-declared resources, and that whatever script is stored the board
  boots and serves its web interface.

## 7. Awaiting the modelling study or the bench

- Defaults for the three speeds, and whether 1 s and 3 s stand.
- The height for proper operation, per sensor noise.
- The bound on the speed's noise, and the bounded waits for clean data.
- The apogee lateness bound, in seconds and metres, against height and
  sensor noise, and the filtering that meets it.
- Each board's `fire_gap` and `refire_interval`, from its protection part.
- Whether the sensor's pad temperature anchors the altitude conversion.
- Whether log playback fits inside a restart.

## 7a. Code requirements (R, given 2026-10-03)

Project requirements on the code itself, to sit with sections 10 to 12 and
to govern the code review.

1. Prefer code structure and naming to comments.
2. Comments are needed for requirement traceability.
3. Comments are needed to help the subject matter expert understand
   implementation decisions that are not clear from the code structure.
4. Prefer clear function names over magic values, even where the value has
   a good name: `wait_for_event()` over `sleep(EVENT_DELAY)`. The name
   states the intent and the implementation may grow behind it.
5. Comments must never be used to teach a topic. A pointer to a reference
   is right; an explanation of the subject is not.
6. Isolate functional units in files according to the single
   responsibility principle.
7. The code should be SOLID and DRY.
8. Code testing should be black box, traceable to requirements.
9. All code should be reachable through the published interfaces.
   Flight code has no dead code, no test-only functions, and no special
   test constructs that reach hidden functions.
10. Prefer pure functions with no internal state; they are easily testable.

**The HAL (R).** The HAL interface is the mockable layer that lets all
flight code be tested, and it makes porting to new hardware trivial. The
HAL itself is validated independently, on hardware, with test equipment and
dedicated HAL validation applications. So the flight code needs no test
construct of its own: the mocked HAL is the seam.

Libraries from outside this project are exempt. They are not rewritten or
modified as part of the pyro code.

## 8. For the test and code review

- Flight code reaches hardware only through the HAL, and every flight
  test drives it through a mocked HAL. Where the bench flight, test mode
  and the pressure trace enter: at the HAL, or inside the flight logic.
- What HAL validation applications exist for each board, and what the
  hardware test plan covers.

- The code requirements above, across the tree: header comments that
  teach (the long essays at the top of several headers), tests that reach
  past a published interface, code no interface reaches, and waits
  written as a delay where a named wait belongs.

- Anything that assumes altitude is never negative: the telemetry
  sentence, the ground station's parser, the CSV, the web summary, scripts.

- Whether any path applies a saved configuration to the running flight
  without a restart.

- No batching of samples remains (FLT-RATE-02).
- The I2C bus recovery runs only at start-up.
- A script left disabled by a reboot within 15 s of boot, though nothing
  failed.
- A looping script cannot starve the network or storage.
- FLT-BOOT-08's median of ten against GND-CAL-01's rolling mean.
- The serial ground test module and whatever else reads the serial input.
- MK1B's noise at the 5 Pa cap.
- One trace of MK1B's sensor through a fire pulse.
