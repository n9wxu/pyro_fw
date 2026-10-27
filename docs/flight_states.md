# The flight state machine

How the flight software runs, state by state. The requirements are in
`REQUIREMENTS.md`, the reasons in `DECISIONS.md` (DD-080 to DD-090 for the
2026-10 review), and what verifies each requirement in `TRACEABILITY.md`.

The rules everything below follows:

1. **Never fire early.** A fire needs measured evidence: no estimate and no
   timer stands in for it.
2. **Data is believed.** Only the absence of data suspends a decision.
3. **Best effort.** Once a fire is decided nothing withholds it: not health,
   not a fault, not a voltage.
4. A safe flight has two pyro systems. This is one of them.

## How it runs

The flight task runs every 10 ms (`LOOP_PERIOD_MS`, FLT-RT-04). Each period
`dispatch_state()` steps the machine once for every sample waiting, up to
eight (FLT-RATE-06). A step calls the detector of the current state, which
returns an event or none; the transition table maps (state, event) to the
next state and an action. `flight_update_outputs()` then sends what is due on
the telemetry port. `flight_storage_service()` keeps the pad record; it runs
on the storage task, in the log writer's slot at the head of core1's frame
(FLT-LOG-09). From launch to landing the flight task runs from RAM and no
flash operation stops it (FLT-RT-02, FLT-RT-03, DD-104).

The files, one responsibility each (`src/flight_sources.txt` is the list the
firmware, the host suites and the simulators all build):

| File | Holds |
|---|---|
| `flight_states.c` | the table and the dispatcher |
| `flight_boot.c` | start-up, the resume decision, the terminal fault |
| `flight_pad.c` | the pad: health, the announcement, launch, USB |
| `flight_ascent.c` | arming, the peak, apogee |
| `flight_estimators.c` | every estimator followed and logged |
| `flight_descent.c` | firing, the descent phase, landing |
| `flight_ground_test.c` | ground test mode |
| `flight_outputs.c` | telemetry, the pad record |
| `estimator.h`, `estimator_table.c` | the estimator interface, and the estimators this build carries |
| `estimator_lumped.c`, `estimator_constacc.c` (with `pressure_estimator.c`) | the two estimators: pressure, its rate, its acceleration, and whether the model explains the readings |
| `pressure_processing.c` | readings in to every estimator, samples out from the obeyed one: gaps, a stuck sensor, the ground reference |
| `fire_control.c`, `fire_plan.c` | the fire rules, and the plan the configuration and the board make |
| `launch_detector.c`, `apogee_detector.c`, `landing_detector.c`, `descent_phase.c` | one detector each |
| `ground_reference.c` | the pad's pressure |
| `pad_check.c` | the pad diagnosis and which outcome is announced |
| `flight_resume.c` | the pad record and the resume decision |
| `ground_test_seq.c` | the ground test procedure, in time |
| `telemetry.c` | the `$PYRO` downlink |
| `small_fmt.c` | the formatter for the text the flight path writes |
| `config.c` | the configuration |
| `beep_codes.c`, `beep_store.c` | what the buzzer says, and the beep table's file |
| `board_selftest.c` | whether the image is on the board it was built for |
| `atmosphere.c` | the 1976 standard atmosphere |

Every comparison is made in pressure (SNS-EST-05). An altitude is computed for
people: telemetry, the log, `/api/status`. It is relative to the pad, is not
clamped, and may be negative.

## Transition table

| From | Event | To |
|---|---|---|
| BOOT_SETTLE | 2.5 s | BOOT_SENSOR |
| BOOT_SENSOR | sensor and storage good, no flight in progress | BOOT_CONTINUITY |
| BOOT_SENSOR | a flight in progress, climbing | ASCENT |
| BOOT_SENSOR | a flight in progress, descending | FALLING |
| BOOT_SENSOR | no sensor, an image for another board, no storage, or an unreadable `config.ini` | FAULT |
| BOOT_CONTINUITY | health read | BOOT_CALIBRATE |
| BOOT_CONTINUITY | the ground test switch held closed | GROUND_TEST |
| BOOT_CALIBRATE | ten readings | PAD_IDLE |
| BOOT_CALIBRATE | none in 10 s | FAULT |
| PAD_IDLE | launch | ASCENT |
| ASCENT | apogee | FALLING |
| FALLING | the rate settled in the drogue's band | DROGUE_DESCENT |
| FALLING, DROGUE_DESCENT | the rate settled in the main's band | CHUTE_DESCENT |
| DROGUE_DESCENT | faster than the drogue's band for 1 s | FALLING |
| FALLING, DROGUE_DESCENT, CHUTE_DESCENT | landing | LANDED |

FAULT, GROUND_TEST and LANDED are left only by a restart.

## The filtered state

The obeyed estimator (below) gives the filtered state from the raw readings:
the filtered pressure, the rate and curvature of ln(pressure), and whether
its model explains the readings (`src/estimator.h`, DD-085, DD-092). Each
estimator measures the sensor's noise from the readings, and does not assume
it, but weighs no reading as surer than 0.5 m of height (DD-113). A
reading more than six of its own standard deviations from the prediction is
set aside for the first 50 ms of its run: a separation's shock changes
nothing, and a longer run is data and is followed (SNS-EST-02). Nothing
else filters, gates or reseeds.

Speeds for the descent bands and the fire rules are judged as the pad's air
would give them (FLT-AIR-01): a canopy in thin air falls faster and is not a
failed one.

## Start-up and resume

`flight_detect_boot_sensor()` brings the sensor up, checks storage, and then
decides whether a flight is in progress (`resume_assess()`): the pad record
exists, no USB host is attached, and the barometer, over at least 0.6 s of
readings, shows the board more than 30 m above the recorded ground and moving
at more than 5 m/s. Any reset cause resumes (FLT-BROWN-02). A board that is
high and still goes to the pad (FLT-BROWN-03).

A resumed flight takes its ground from the record, assumes no channel has
fired, and fires each as fresh data meets its trigger. Resumed climbing, its
estimators start again, so apogee waits for them to explain the readings
(FLT-APO-07). Resumed descending, apogee is taken as passed and a DELAY
counts in full from the resume (FLT-BROWN-06).

The record is written after 10 s on the pad, cleared when the flight's log
is closed, and cleared on the bench, with a USB host attached and test mode
off (FLT-BROWN-01, FLT-BROWN-04, FLT-BROWN-08).

## PAD_IDLE

Every second the pyro health is read and the diagnosis re-derived
(`pad_check_announcement()`): one of four outcomes, by priority general fault,
pyro 1, pyro 2, OK to fly. Only enabled channels count, and a channel whose
pads the pin assignment gives to the script is not a pyro channel
(PYR-HEALTH-02). The diagnosis by name is on `/api/status`.

The ground reference follows the weather, holds while the rocket moves, and
freezes at launch on the pad's own pressure. A board carried to another pad
reseeds after 5 s still (GND-CAL-06).

Launch is 15 m above the reference on the filtered state
(`launch_detected()`), with no speed test and no hold time. Time zero is
back-dated to the start of the rise (FLT-LAUNCH-03).

On USB, with test mode off, no launch is detected, nothing is announced and
no record is written (USB-01, USB-02).

## ASCENT

The channels arm once the rocket has passed about 30 m (0.9965 of the pad's
pressure) and, having climbed faster than 10 m/s, slows below it (FLT-ASC-07,
FLT-ASC-08).

Apogee is the obeyed estimator going over the top, once the channels are
armed (FLT-APO-04): seen climbing, then seen falling, each by more than three
times its rate's own uncertainty, with its model explaining the readings all
the way between (`apogee_detected()`, FLT-APO-07). A fall whose climb was not seen that way must last 2 s. There is
no hold time. It is declared after the true apogee, never before it: within
0.5 s to 10 km, 1.5 s at 20 km and 2.5 s at 30 km at a sensor noise of 9 Pa
(`test_FLT_APO_01_...`). The boards measure 1.9 to 2.6 Pa on a battery; 9 Pa
is the tests' margin (DD-094).

## The estimators

An estimator turns the raw readings into the filtered state. Each sits behind
`src/estimator.h`: a name and a table of functions. `estimator_table.c` lists
the ones a build carries, the default first. Every one is given every
reading. `estimator` in config.ini names the one the flight obeys, read at
start-up (SNS-EST-06).

Each reports whether its model explains the readings (SNS-EST-08). Near
Mach 1 the static ports misreport, and no rocket moves the way those readings
say, so the model does not explain them. The estimator's apogee is not
decided on such readings, and none of them is its peak (FLT-APO-07,
FLT-APO-08).

Beside the estimators runs the simple filter (`src/simple_apogee.c`): the
median of the last three raw readings, and apogee as that median standing
2 m below its lowest point for 0.5 s (FLT-APO-09). With `model_decides` true
(the default) it stands in for the estimator once its own speed has been
under 50 m/s for the last 3 s, and either the estimator does not explain the
readings and that speed is a fall, or the estimator explains them and has
seen the board still for 2 s (FLT-APO-10). With `model_decides` false it
decides alone. The log says which declared apogee (FLT-APO-11). The study is
`docs/apogee_fallback.md`.

From launch to landing the log carries an `EST` row a second for each
estimator, and a row when each one's own apogee rule is met, whichever is
obeyed (SNS-EST-07).

| Name | Model |
|---|---|
| `constacc` (default) | constant acceleration (`docs/descent_speed_estimator.md`) |
| `lumped` | one equation of motion, pad to ground, learning thrust and drag (`docs/lumped_parameter_filter.md`) |

## Descent and firing

The state names a measured rate band, not a channel: FALLING above 35 m/s,
DROGUE_DESCENT to 10 m/s, CHUTE_DESCENT below. A rate counts once it has held
in one band, steady to within a quarter of itself (at least 2.5 m/s), for
1.2 s: free fall gains about 12 m/s in that time, which no canopy's rate
tolerates (`descent_phase_settled()`). DROGUE_DESCENT returns to FALLING when
the descent is faster than 35 m/s for 1 s. CHUTE_DESCENT is left only for
LANDED.

`fire_control_step()` is the fire rules, a pure function of the plan, the
sample and its own state:

- **First fire.** Each enabled channel fires when its own trigger is met:
  DELAY after apogee, AGL at a height, FALLEN a distance from the peak, SPEED
  at a descent speed. Nothing fires before apogee (PYR-SAFE-04).
- **Re-fire.** A channel that has fired fires again every `refire_interval`
  while the descent is faster than its `pyro1_refire_speed` or
  `pyro2_refire_speed` (PYR-REFIRE-01).
- **Emergency fire.** Any time after apogee, while the descent is faster than
  `emergency_fire_speed`, every enabled channel fires, and goes on firing,
  whatever its own trigger (FLT-EMRG-01).
- A speed of zero turns its rule off. All three default to zero.
- **One at a time.** A pulse never overlaps another, and `fire_gap` of quiet
  time separates the end of one from the start of the next. A first fire goes
  before a re-fire; between re-fires the channel that has waited longer goes;
  on a tie, pyro 1 (PYR-DEPLOY-02).
- For 2 s after a pulse the state is bounded to free fall, so the
  separation's shock cannot bring a trigger forward (PYR-MODE-06).

`refire_interval` and `fire_gap` come from the configuration, within the range
the board declares; zero takes the board's default (`fire_plan_from()`,
PYR-BOARD-01). A value outside the range is brought in and reported.

Every pulse is recorded with what the board observed of it: energised or not,
a fault, and whether the channel then read open. Nothing depends on those
records (PYR-FIRE-01, PYR-VERIFY-01).

Landing is under 2 m/s for a second within 30 m of the ground or, once
`landing_timeout` has passed since apogee, under 2 m/s for a second at any
height. The peak is then beeped out, the log closed and the pad record
cleared.

## A failed sensor

A whole second of one reading to the pascal is a stuck sensor; no sample for
0.5 s is a lost one. Each is logged, shown on `/api/status` and sent as a
telemetry line. While it lasts and for a second after, nothing is decided on
it; a DELAY, which needs no data once apogee is known, still fires
(SNS-PRES-10, SNS-PRES-11). Nothing tries to recover the sensor in flight
(SNS-REC-01): the backup system is what saves that flight.

## GROUND_TEST

Entered only from start-up with the switch held closed. The switch opened
starts a countdown; pyro 1 fires at zero, then a tone, a second countdown and
pyro 2, then the all-clear (`gt_seq_step()`). A channel the configuration
does not enable is skipped, with its tone and countdown. Closing the switch
again stops it before the next fire. A fire is delivered on command: no
health reading withholds it (GND-TEST-13). The mode runs on the configuration the board
started with and never flies.

## Telemetry

One `$PYRO` message a second on the pad, in flight and after landing
(`telemetry_send()`). An apogee, a fire or a landing is queued and carried by
the next message, so it is at most a second late (TEL-11). The port accepts no
commands.

## What is not in the machine

- No fallback timer for apogee, and no fire on an estimate.
- No channel roles: "drogue" and "main" are the operator's wiring.
- No reload of the configuration: a saved change waits for the next start
  (CFG-10).
- No recovery of a failed sensor, and no refusal of a fire.
