# Design Decisions

The design decisions in force. Each records the decision, why it holds, and,
where the user gave one, the user's direction in the user's words.
Requirement IDs in brackets are what a decision implements; measurements are
of the parts and of the system as it is. Numbers are never reused, so the
sequence has gaps.

## Platform Targets

### DD-012: Pin Assignments Per-Platform
- **Decision:** each board declares its pins. `boards/<name>/board_pins.h`
  names the fixed ones, and the board's `pin_caps.h` lists every assignable
  pin with every function its hardware supports, in the vocabulary of
  `src/pin_model.h`. `pins.ini` chooses among what the board offers; a pin
  with no row is assignable to nothing.
- **Why:** pin mapping is the board's. The specification describes the
  logical interface -- a pressure sensor, two pyro channels, a buzzer, a
  serial downlink -- and configuration may only pick from what the board
  offers.

## Safety

### DD-016: Launch On Height Alone
- **Decision:** launch is a filtered pressure below the pad's pressure at
  15 m (`launch_detected()`; FLT-LAUNCH-02, FLT-LAUNCH-07), as AltOS
  declares a barometer-only launch on height alone. No launch is declared
  while a USB host is attached and test mode is off (USB-01). T+0 is the
  first sample of the rise more than 50 cm above the pad (FLT-LAUNCH-03).
- **Why no speed:** on the pad the filtered speed swings by up to 24 m/s in
  gusts (`docs/lumped_parameter_filter.md`), and the height is what holds a
  launch back; AltOS's own code ignores the barometer's speed on the pad as
  "quite noisy". Barometric drift, a weather front and a board warming on
  the pad move the pressure slowly and never by 15 m. Arming (DD-017) keeps
  a false launch from firing anything. A filtered pressure of zero is no
  estimate and no launch.
- **Why 15 m:** at the user's direction -- "1s detection delay. This should
  be worst case with faster launches detecting quicker." In the harness the
  5:1 rocket of FLT-LAUNCH-08 is declared 0.80 to 0.84 s after T+0 at 9 and
  1.2 Pa of noise, a subsonic one 0.44 to 0.48 s after. Gusts on the pad are
  real pressure, which no filter removes: 30 minutes at 40 Pa rms declares
  no launch on either sensor, 60 Pa rms does. 40 Pa rms is a 19 mph mean
  wind at 30 % turbulence on the side of a tube (Cp 1.5), and 23 mph on a
  port facing the wind (Cp 1): the NAR's 20 mph launch limit [SNS-EST-03].

### DD-017: Arming Requires Confirmed Motor Burn
- **Decision:** the pyros arm once the climb has been faster than 10 m/s and
  is now slower than that, after the filtered pressure has been below
  0.9965·p0, about 30 m above the pad, and on a sample that is not suspect
  (`arming_due()` in `src/flight_ascent.c`; FLT-ASC-04..08). Apogee is not
  declared before arming (FLT-APO-04). The speeds are the filtered state's
  (DD-085).
- **Why:** after a false launch from drift the speed is about zero. Without
  the gate the board would arm at once, see the speed at zero, declare
  apogee and fire on the ground. A rocket that only just meets the launch
  rule at 15 m is past it within a second, and a false launch never climbs
  to 30 m, so the height gate keeps arming off the pad. Arming needs only "slower than 10 m/s", so a rocket
  already descending arms: a sensor that fails near apogee delays arming and
  cannot close its window for good. A suspect sample (DD-050) arms nothing.

### DD-015: Landing Timeout for Elevation Mismatch
- **Decision:** landing is detected at any height once descent has lasted
  `landing_timeout` (default 60 s) and the rocket is still: under 2 m/s for
  1 s of sample time, on samples that are not suspect (`landing_detected()`;
  FLT-LAND-07). Otherwise landing needs the same stillness within 30 m of the
  pad (FLT-LAND-02, FLT-LAND-03).
- **Why:** a rocket that lands well above the pad -- a mesa, a hillside --
  may never come within 30 m of it, and the board must still reach LANDED and
  close its log. Stillness rather than a slow descent, because a main
  descends at 3 to 6 m/s: a timeout keyed on "below 5 m/s" would declare a
  rocket still under its main landed in the air and close its log with the
  descent unrecorded.

### DD-109: A Simple Filter Beside The Estimator, Standing In Or Deciding Alone
- **Decision:** at the user's direction -- "Need a simple filter (no mach
  lockout) that will ALWAYS detect apogee for simple flights", "Need the
  kahlman filter that always runs as an estimator but can be disabled from
  the decision loop", "Even when the kahlman filter is active (and in the
  decision loop) it can detect when it is not converging and allow the
  simple filter to save the day", and "A simple mach lockout is just
  watching for the measured speed to drop below a threshold for a few
  seconds":
  - The simple filter (`src/simple_apogee.c`) takes the median of the last
    three raw readings the sensor is believed on, from launch. Its apogee is
    that median standing 2 m below the lowest median since launch for 0.5 s,
    once a median has been 30 m above the pad (FLT-APO-09). Its top, for
    the peak and for DELAY, is the last time the median stood at its lowest
    (FLT-APO-08, PYR-MODE-05).
  - Its speed is the change in the median's height over 0.5 s, taken every
    100 ms. Its lockout is that speed under 50 m/s for the last 3 s; a
    faster speed closes it again.
  - `model_decides` in config.ini, true by default: the obeyed estimator's
    apogee (FLT-APO-07) is the flight's. The simple filter's stands in once
    the lockout is open and either the estimator does not explain the
    readings and the speed is a fall, or the estimator explains them and
    has seen the board still for 2 s (FLT-APO-10, DD-113). False: the simple
    filter's alone. The estimators run and log either way.
  - The log carries an `EST simple` row a second and `EST simple APOGEE`
    when its rule is met, and `EST apogee by <name>` at the declaration
    (FLT-APO-11).
- **Why:** a hand-run pressure chamber flight on an MK1B-8MB climbed and
  fell faster than any flight, the estimator never explained it, and the
  board declared neither apogee nor landing. A flight the model cannot
  explain still has a top. The study (`docs/apogee_fallback.md`,
  `sim/study/simple_apogee.py`) measured the choices:
  - The median of three and the 0.5 s hold were early on none of the
    subsonic sim flights at 9, 20 and 35 Pa of noise, with and without an
    ejection spike, 1.0 to 1.2 s late. The hold is set by noise: under
    0.5 s, 35 Pa makes it early. At the user's word, "The ejection pressure
    spike will decay instantly when the rocket separates", a spike is not
    what sets it.
  - Standing in on "not explained" alone hands over early on port-error
    flights, which leave the estimator unexplained for up to 18 s. With the
    lockout it was early on none of the 696 sim flights under the board's
    ceiling.
  - The lockout closes again on a fast speed and the stand-in needs a fall:
    in `mach_tests` a port error twice the model's showed a fake top, a
    hover that opened a latched lockout, then a fast fake fall 28 s before
    the true apogee.
  - The board still for 2 s: a fall faster than any the model explains,
    to the ground, opens the lockout only once the board is still. By then
    the model explains the readings and sees no fall, and without this the
    flight would declare neither apogee nor landing (issue 78). A real fall
    under gravity is one the model explains, so this is a chamber's case.
- **Proven by:** `simple_apogee_tests`; `chamber_flight_tests` (the chamber
  flight declares apogee 0.5 s after its top with the simple filter alone,
  and on its fall with the estimator deciding; a 5 g fall to the ground has
  the simple filter's apogee once the board is still; all land);
  `mach_tests` unchanged.

### DD-110: The Log Starts On The Pad
- **Decision:** at the user's direction -- "We need to show a log starting
  at ground level. This needs a pre-launch ring-buffer large enough to
  detect all flights and still have a ground record. Changing the log
  format is fine.":
  - Each PAD_IDLE sample goes into a ring of 110 (`src/log_pad_ring.c`),
    nine bytes each: the filtered pressure, the raw reading as its
    difference, the low 16 bits of its time and its temperature as a
    difference from the ring's first. A gap of a second empties it.
  - At launch the HAL freezes the ring with T+0 and the ground. The storage
    task writes the header and then the ring's rows, timed from T+0 and in
    PAD_IDLE, before the log ring's rows [DAT-11]. They never pass through
    the 4 kB log ring, so the launch's holdoff on flash writes [DD-027] is
    unchanged.
  - The format is version 2, `PYL2`: a sample's time is signed, and the
    header carries the estimator obeyed and `model_decides`, rendered as
    `# Estimator:` and `# Apogee decided by:` [DAT-07, DAT-08]. `PYL1` logs
    still render.
  - The replay takes the estimator and the deciding filter from the header,
    and feeds a log that starts on the pad from its first row, with no
    climb made up to the launch.
- **The slowest rocket:** at the user's direction -- a 1 g climb "will
  not reach a speed where the fins can stabilize it before it runs out of
  launch rail (approximately 1.7m)": the design is for 5:1 thrust to weight
  or more, 11.5 m/s at the end of a 1.7 m rail [FLT-LAUNCH-08]. The launch
  rule (DD-016) declares such a rocket within 1 s of T+0.
- **Why 110:** 1 s of climb and 0.5 s of pad at the fastest sensor's 73
  readings a second. With the flight path in RAM (DD-104) MK1C has little
  RAM below its stacks; 110 entries are 1 kB, and hold 2.2 s at the
  MS5607's 50 readings a second.
- **Proven by:** `log_pad_ring_tests`; `flight_profile_tests`
  (`test_FLT_LAUNCH_08_the_slowest_rocket_is_seen_leaving_within_1_s`,
  `test_DAT_11_the_log_starts_on_the_ground` with a 5:1 rocket, and the
  replay); `flight_log_tests` (signed times, the version 2 header, a
  version 1 log).

### DD-111: The Design's Ceiling Is 30,000 ft
- **Decision:** at the user's direction -- "The 30,000 ft ceiling is the new
  hardlimit for these designs. This is limited by the noise budget. At
  higher altitudes the noise needs more filtering than the time allowed.":
  a board's height for proper operation (SNS-MAX-01) is the lesser of its
  sensor's range and 9,144 m (`DESIGN_CEILING_M`, `src/pressure_sensor.h`).
  The MS5607 boards report 9,144 m on `/api/limits`; the BMP280 keeps its
  own 9,000 m. FLT-APO-01's bound is 0.5 s to that height. Above it the
  flight goes on with the data it has, never declaring apogee early.
- **Why:** at 9 Pa of noise a speed taken over 0.5 s carries about 27 m/s of
  noise at 20 km and 133 m/s at 30 km, where a pascal is 7.7 m
  (`docs/apogee_fallback.md`). The speed lockout and the apogee rules need
  more filtering there than the time near apogee allows.
- **Proven by:** `flight_profile_tests`
  (`test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height`
  to the ceiling and just past it;
  `test_SNS_MAX_01_above_the_ceiling_apogee_is_never_early` at 20 and
  30 km).

### DD-113: The Filters Tuned Against AltOS
- **Decision:** at the user's direction on the comparison with AltOS
  (issue 78) -- "take the recommendations." -- and on what the replay of
  AltOS's flights showed -- "drogue fire cannot pressurize the sensor bay.
  There is a pressure bulkhead. Fix the model.":
  - `constacc` is the default estimator; `lumped` logs beside it (DD-092).
  - No reading is weighed as surer than 0.5 m of height
    (`PEST_HEIGHT_NOISE_M`), whatever the sensor's own noise: what the model
    leaves out is metres, not the sensor's decimetres. The tracked noise
    stays the sensor's (`noise_pa`).
  - A reading 6σ from the prediction is set aside for the first 50 ms of
    its run, however many readings that is; a longer run is data
    (`pest_bad_run_rejects()`, both estimators; SNS-EST-02). A reading set
    aside says nothing of the model: `constacc` does not count it against
    explaining the readings.
  - Apogee (FLT-APO-07): the fall of AltOS's rule, and its error check,
    added to the model's: the readings must track the filtered height, the
    average of their squared difference decaying with a time constant of
    0.16 s staying under (10 m)², or the square of three times the sensor's
    noise where the air is thin. A reading set aside is no difference. The
    climb and the fall are judged at 2σ of the rate.
  - The simple filter also stands in when the model explains the readings
    and has seen the board still for 2 s (FLT-APO-10, DD-109).
  - Launch is on height alone (DD-016).
  - `apogee_lockout`, seconds after T+0, holds any apogee, as AltOS's
    does (FLT-APO-12).
  - The sim's charge is a separation's shock: readings off by up to 5 kPa
    either way for up to 50 ms, at the board's own fire or another board's
    (`mp_shock_t`, PYR-MODE-06). The sensor sits behind the bay's pressure
    bulkhead, so no charge pressurises its air. After its own pulse
    `lumped` sets readings aside for those 50 ms.
- **Why 0.5 m:** host flights with the floor and the fall margin varied,
  all suites:

  | Floor | Margin | Result |
  |---|---|---|
  | 0.3 m | 2σ | the chamber flight has no apogee; two Mach cases fail |
  | 0.4 m | 2σ | one port-error flight deploys nothing |
  | **0.5 m** | **2σ** | **every suite passes** |
  | 0.7 m | 2σ | the thrust report ends 1.08 s after burnout |
  | 1 m | 2σ | as 0.7 m, and a still board's speed is past 1 m/s |
  | 6 m (AltOS's) | 1σ to 3σ | landing, canopy, thrust and below-pad tests fail; at 2σ and 3σ apogee is past 0.5 s |
  | any | 0 | early on every subsonic profile, by 0.01 to 0.31 s |

  The sensor on a battery is 2.3 to 2.6 Pa, about 0.2 m. The 6σ gate is
  widened by the floor, not removed: removed, one reading of any value
  would move a decision (SNS-EST-02).
- **Why the model's checks stay beside AltOS's:** AltOS's rule alone, fall
  below zero and the error check, was early on every subsonic profile; with
  port errors near Mach 1, up to 2 s early, and 30 s early on a low-drag
  flight, because an error that grows smoothly never trips a 10 m check.
  Without "explains" the 30 s case stays; without the climb seen, a port
  error twice the model's fires 32 s early. Baro-only AltOS boards lean on
  the user's lockout for those flights.
- **Why the stand-in stays:** with the simple filter removed, port-error
  flights the model never explains deployed nothing at floors of 0.4, 0.7
  and 1 m. Standing in without the "not explained" condition fires 1.3 to
  2 s early on port errors; on a model that has seen the board still for
  less than 2 s, 0.15 s early at a real top.
- **Why 50 ms:** AltOS's 100-a-second logs show the separation, by its own
  charge or the motor's, as a shock of 40 to 90 g with the pressure off by
  1 to 5 kPa for 10 to 40 ms, then back on the arc. Set aside by count, two
  readings, a 40 ms shock at the fastest sensor's 73 a second is three.
  Another board's separation at the true top, a backup altimeter's, held
  apogee 3 s with the shock counted against the model, and holds it at
  most 0.15 s now (`flight_profile_tests`). At the user's word, a shock under
  the gate that leaves the model unexplained for about 1 s is acceptable:
  "1s is OK".
- **AltOS's flights (`altos_flight_tests`):** fifteen TeleMetrum logs
  (`test_data/altos/`, GNU GPL version 2 or later, kept as AltOS ships
  them). Launch is declared on every one; apogee from 0.08 s before AltOS's
  own, from its accelerometer, to 3.9 s after, 7 of 15 within 1 s. The
  later ones follow a separation at the top whose shock is under the gate
  on that sensor's 15 Pa of noise. Past AltOS's charge its log thins to ten
  rows a second, and a replay places an apogee there only to within
  seconds.
- **A stuck sensor from the first reading:** the estimators are put back to
  where a run of repeated readings began. A run that began with the first
  reading put them back to nothing, a filtered pressure of zero, and the
  board never launched; the snapshot is taken at the start.
- **The cost of the floor:** a ballistic impact settles the speed later;
  LANDED comes within 4 s of touchdown, at the user's word: "4s for landing
  detection is fine."
- **Proven by:** `estimator_tests`, `estimators_tests`
  (`test_SNS_EST_09_a_separation_shock_moves_no_estimate`),
  `apogee_detector_tests`, `launch_detector_tests`, `flight_pad_tests`
  (`test_SNS_PRES_10_a_sensor_stuck_from_its_first_reading_keeps_that_reading`),
  `flight_profile_tests`
  (`test_FLT_APO_01_another_boards_separation_at_the_top_does_not_delay_apogee`,
  `test_FLT_APO_12_apogee_waits_for_the_users_lockout`), `mach_tests`,
  `chamber_flight_tests`, `altos_flight_tests`, `lumped_port_check`.

## Configuration & Features

### DD-018: Lua Resources Are One Interface Table, Not Four APIs
- **Decision:** Lua reaches hardware through one table. Whoever configures
  the hardware publishes `{name, kind, vtable, ctx}` entries into
  `lua_iface.h`'s table, and Lua resolves by name **and kind**
  (`lua_iface_find()`). `lua_platform.h` declares no family of output, input,
  serial or pixel calls. Boards add their own features through the weak
  `board_lua_publish()` hook rather than by widening the header.
- **Why:** the kind is part of the lookup, so a wrong combination is
  unreachable rather than refused -- `output.set()` on a pad published as an
  input finds nothing, because no output vtable was ever installed for it.
  Dimmability lives in the vtable for the same reason. The half-bridge PIO
  program makes shoot-through unencodable on the same argument: a check can
  be forgotten, a missing pointer cannot be called.
- **The proof sees the table:** `prove_core0.py` builds its graph from `bl`
  instructions, so a call through a pointer is invisible to it. It folds the
  entries of every table named `*_vt` into the Lua task's graph, and fails an
  image that links `lua_iface_publish` with no `*_vt` symbol, or
  `http_work_run` without `http_unit_vt`. The naming is load-bearing.

### DD-019: A Released Pyro Channel Is Mocked, Not Guarded
- **Decision:** the flight software's per-channel pyro operations (`fire`,
  `get`, `fault`) are a table installed once at boot from the pin assignment
  (`src/pyro_release.h`). A channel released to Lua gets a mocked table that
  touches no hardware; the real operations are not reachable for it. The two
  shared entry points (`hal_pyro_sample()`, `hal_pyro_update()`) return early
  once both channels are released, because they drive the common.
- **Why:** a released pad belongs to Lua on core1, and core0 writing it would
  fight core1 for the same SIO register. A check at each call site has to be
  remembered at every new one; a pointer that does not go there cannot be
  got wrong. Same argument as DD-018.
- **Nothing is silent:** every mocked operation is written to the flight log
  as a `MOCK` row beside the event that commanded it, sent on the telemetry
  downlink, and counted on `/api/status`. A log showing `PYRO1` with no note
  beside it would record an ignition that did not occur.
- **A released channel reports open, not good.** There is no igniter circuit
  the flight software controls there.

### DD-020: A Pad Has One Owner, and the Claim Is What Yields the vtable
- **Decision:** `src/pad_claim.c` holds one owner per pad.
  `pin_store_claim_pads()` walks the assignment once and writes each pad's
  owner -- one array, one write per pad. Afterwards, `pyro_release_claim()`
  installs a channel's real methods only if it can claim that channel's pads,
  and `lua_iface_publish()` refuses a resource unless Lua can claim every pad
  it drives. Claims are all or nothing.
- **Why:** a pad in both the pyro table and the Lua table would be two cores
  driving one FET gate. With the claim as the currency, the second table's
  entry cannot be created at all: nothing compares the tables, there is one
  pad and one owner, and the claim is already spent. Same argument as DD-018
  and DD-019, applied to the thing those two tables share.
- **All or nothing** is what stops a half-claimed bridge: a resource driving
  a free pad and a held one gets neither, rather than one gate it owns and
  one it does not.
- **Reported:** `/api/status` carries what the claim decided (`pyro1_real`,
  `pyro2_real`) beside what the assignment asked for, so a disagreement is
  visible rather than inferred.

### DD-021: Both Channels May Deploy on One Event
- **Decision:** both channels may deploy on a single flight event
  [PYR-DEPLOY-01]: a low flight may need drogue and main out together. They
  are never energised at the same instant, and `fire_gap` of quiet separates
  a pulse on one from a pulse on the other [PYR-DEPLOY-02, DD-082].
  `hal_pyro_is_firing()` is what the fire rules read to keep them apart.
- **Why the pulses stay apart:** the two channels share one common element.
  Two igniters in parallel draw through one FET and one protection part. On
  MK1B that is F2, a 1.5 A self-resetting PTC, and the combined draw can trip
  it and fire **neither**. On MK1A it is F1, an 8 A fuse with more headroom
  and no recovery. `hal_pyro_is_firing()` enforces a current limit; it is not
  to be simplified away.
- **Tested** by `test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event`:
  both deploy, close enough together to be one event.

### DD-022: No Timer Forces Apogee
- **Decision:** no timer forces apogee, and there is no timeout on ASCENT.
  Apogee is the obeyed estimator saying the rocket has gone over the top,
  and nothing else (`apogee_seen()` in `src/flight_ascent.c`; FLT-APO-01,
  DD-092). `max_coast_s` is not a configuration key (DD-030).
- **Why:** a timer helps only when its value is right, and a wrong value
  fires during ascent. A backup that can destroy the rocket it is backing up
  is worse than no backup, and a fire on a timer breaks DD-081's rule that a
  fire is decided only on evidence. If the sensor dies mid-flight the machine
  stays in ASCENT: that costs the log, not the rocket. Any timeout would need
  the sensor to notice that the sensor has failed.

### DD-023: Descent Phase Comes From The Rocket, Not The Firing Log
- **Decision:** `FALLING`, `DROGUE_DESCENT` and `CHUTE_DESCENT` advance on the
  descent speed holding steady in a band, never on which channel was
  commanded [FLT-DESC-01]: under 10 m/s is a main's band, under 35 m/s a
  drogue's, both judged in the pad's air (DD-079). A speed must hold within a
  quarter of itself (at least 2.5 m/s) for a 1.2 s dwell (`src/descent_phase.c`).
  A drogue descent that speeds up out of its band, held 1 s, returns to
  FALLING. Landing is detected in all three [FLT-DESC-02]. The state numbers
  are fixed, so every flight log reads with the same table (TEL-07).
- **Why a rate and a dwell:** a working canopy is a descent rate that has
  stopped changing; a failed one is a rate that has not. Free fall gains
  about 12 m/s over the 1.2 s dwell, which breaks the tolerance at every rate
  a canopy could explain -- that is what stops the fall through the main band
  just after apogee from reading as a deployed main.
- **Why not the firing log:** a channel with no igniter, a channel set to
  `NONE`, or the two firing out of order would otherwise park the machine
  for the rest of the flight, and a log that never reaches LANDED is never
  closed.
- **A phase never cancels a configured deployment.** The fire rules run in
  all three descent states (`descend()` in `src/flight_descent.c`). A rocket
  whose descent merely *looks* main-like -- a big drogue, a light airframe --
  still fires the main its configuration asks for: the firmware does not
  override the operator on the strength of an inference.

### DD-026: Brownout Recovery Needs A Marker, Because The Silicon Cannot Help
- **Decision:** after 10 s of PAD_IDLE with no USB host attached, the board
  writes a pad record, `pad.mkr`, holding the ground pressure and the sensor
  noise measured on the pad [FLT-BROWN-01]. After a restart it recovers the
  ground reference from that record rather than calibrating -- but only when
  the barometer shows the board both above the recorded ground and moving
  (DD-086). The record is cleared once a flight lands, and on the pad with a
  USB host attached and test mode off [FLT-BROWN-04, FLT-BROWN-08].
- **Why a record at all:** a brownout is electrically a power cycle. The
  RP2040's brown-out detector drives POR, so `HAD_POR` is set exactly as when
  somebody connects the battery, and RAM and the watchdog's scratch registers
  are gone. The reset cause cannot tell a brownout from a battery being
  connected; the record and the barometer answer the rest.
- **Why not calibrate:** calibrating defines the current altitude as zero. Do
  that at 600 m and the rocket has no altitude left to deploy against.
- **The motion test is the safety-critical part.** Weather can move the
  pressure by more than 30 m of altitude between the record being written
  and the board being switched on again. Without measured motion, a drifting
  barometer on the pad would read as airborne -- and a resumed descent fires
  on its triggers. A high-but-still board is `RESUME_STILL` and starts as a
  cold boot: a stationary board does not need a parachute (FLT-BROWN-03).
- **Kept before launch, not at it:** nothing is stored at launch, when a
  connector bouncing is likeliest. The record is cleared by writing an
  invalid one, so `hal.h` needs no delete and a new board need not implement
  one.
- **The log restarts.** The flight the log was recording went with the RAM
  that held it, so a resume opens a new log whose T+0 is the moment of the
  resume, with a RESUMED row [FLT-BROWN-07].

### DD-027: The Flight Log Holds Off Flash Through The Launch Shock
- **Decision:** the flight log's first write waits until its 4 kB RAM ring
  has filled once, or 2 s from the log's start, whichever comes first
  (`launch_holdoff` in `src/hal_common/hal_common.c`). A full ring and a stop
  always write, so nothing is dropped to keep the flash quiet. After the
  holdoff the ring is written every 200 ms.
- **Why:** launch shock -- a battery connector bouncing -- is the likeliest
  cause of the brownout DD-026 exists to survive, and a flash write in
  progress is the worst moment to lose power. 4 kB is 186 samples of 22
  bytes, over 2.5 s at the fastest sensor's 73 a second, so the ring spans
  the shock and the 2 s timer ends the holdoff.
- **The trade:** a brownout before the first write loses the flight so far,
  up to the ring. The data is worth less than the flight.
- **Not verified by test.** The holdoff is a timing property of the storage
  task, which the host harness does not model.

### DD-030: Every Configuration Key Is Read By Something
- **Decision:** every key in `config.ini` has an effect [CFG-SUBSYS-01]. A
  file carrying a key the firmware does not read still parses [CFG-08] and
  loses the key on its next save.
- **Why:** a key in `config.ini` is a promise that changing it changes the
  board. A key that would contradict a requirement if wired is not offered:
  there is no `log_enabled` (the log always runs, SYS-DATA-01), no
  `buzzer_startup` (the pad verdict is always announced, BUZ-CODE-08), no
  `max_coast_s` (DD-022), no `beep_mode`, no telemetry format or rate
  (DD-088) and no log rate (DD-101).

### DD-031: A Faulted Board Sends No State Sentence
- **Decision:** no `$PYRO` in the start-up states or FAULT; a `!FAULT <names>`
  line every 5 s instead [TEL-05].
- **Why:** the ground-station contract has six states and no FAULT, and
  state 0 means "on the pad, ready". A board that cannot fly would otherwise
  report itself ready once a second.

### DD-037: A PC On USB Is Detected From Start-Of-Frame, And A Charger Cannot Be Detected
- **Decision:** a USB host is attached while the SIE's last start-of-frame
  number (`usb_hw->sof_rd`) has changed within the last 100 ms
  (`usb_host_active()` in `src/main_hardware.c`). The flight task asks before
  each step and passes the answer to `flight_set_usb_attached()`. While
  attached, and test mode is off, the board declares no launch, says nothing
  but one double chirp on attach, keeps no pad record and resumes no flight
  (USB-01..04).
- **Why SOF:** a host sends a start-of-frame every millisecond while it is
  awake, and nothing else does. So a false "attached" needs a real host, and
  every other error means "not attached", which leaves launch detection on
  (USB-07). A false "attached" in flight would mean no deployment.
- **Not used:**
  - *VBUS.* On MK1A, MK1B and MK1C, USB VBUS goes only to the TP4057 charger
    (VCC, its capacitor, the two LED resistors and a test point). No RP2040
    pin sees it, and TinyUSB forces the SIE's VBUS-detect override anyway.
  - *`tud_ready()` / suspend.* An unplugged cable reads as a suspend only if
    the idle line reads as J. D- floats with no host, so that is not assured,
    and the error would be "attached" on a battery.
  - *Data-line state for chargers.* A BC1.2 charger shorts D+ to D-, which
    reads as SE1 through the device's pull-up. A floating D- with no cable
    can read the same, so it could false-trigger in flight.
- **What it cannot do:** see a charger, a sleeping PC or a suspended bus. All
  three read as detached. The board then announces and detects launches as
  if on battery, and chirps again when the host wakes.
- **To detect a charger** needs a board change (task U1):
  - A VBUS divider to a spare GPIO. MK1C has GPIO2-5, 9, 10 and 13-15 free.
    GPIO24 is unconnected on MK1A/B. RP2040 pins are not 5 V tolerant.
  - Or the TP4057's open-drain ~CHRG and ~STDBY to two GPIOs: either one low
    means the charger has input power. Those nets also drive the LEDs, and
    with VBUS present an idle one sits near VBUS minus the LED drop, so check
    that voltage against the pin's limit.
  Either signal would be ORed with SOF in `usb_host_active()`.

### DD-038: Test Mode Flies On USB, And Lives Only In RAM
- **Decision:** `POST /api/test_mode/on` and `/off` set a RAM flag. While it
  is on, a board on USB behaves exactly as on battery: it detects a launch,
  fires, keeps the pad record and announces (USB-08). Every start begins with
  it off. It does not change from launch to landing: the flight layer
  ignores the request then, and the answer reports the mode kept
  (`flight_set_test_mode()`). The web UI asks for confirmation before turning
  it on, and shows a warning while it is on.
- **Why:** a chamber flight watched in the web UI, and a bench soak for
  false launches, both need the flight machine with a PC on the port, where
  USB-01 otherwise turns it off. A PC on USB means a bench, never a flight,
  so this is as safe as flying on battery. The igniters are the operator's
  business, and the prompt says so.
- **Why RAM:** nobody should plug a board in and find it still in test mode
  from a session someone forgot about. With the flag in a file, whether "USB
  means grounded" held would depend on that file.
- **The cost:** a restart ends test mode, including one mid-test. For the
  same reason a resume on USB can never run in test mode: the restart clears
  the flag before the resume decision. Resuming has to be tested on battery.
- **On and off go in the path:** the request carries no body.
- **Chirp:** switching test mode off while attached is an attach, so it
  chirps. Switching it on resumes the pad announcement, which confirms it by
  ear.

### DD-108: A File Is Written Only Once The Cleanup Is Done
- **Decision:** at the user's direction -- "The file API and the UI should
  understand when erase is over so it is safe to write.":
  - A request that writes a file to the flash store (`/www/<file>`,
    `/api/lua/script`, `/api/config`, `/api/pins`, `/api/beeps`,
    `/api/serial`; the routes marked `writes` in `src/http_routes.h`) is
    answered 503 while the cleanup has sectors left to erase, before any
    of it is written: `{"error":...,"erasing":true}` and `Retry-After: 1`
    [WEB-API-21]. In flight the 423 of DD-058 comes first.
  - Deletes, the flight log's erase and firmware updates go through: none
    needs an erased page.
  - The page's every POST goes through one function, `post()`. A refusal
    for the erase is held there: the page shows that it waits for the
    flash to be erased, reads `/api/fs` until the cleanup is done, and
    sends the same request again [WEB-UI-08].
  - `support/pyro_http.py`'s `post()` and `support/upload_www.sh` wait and
    send again the same way.
- **Why:** a write takes only erased pages (DD-100), and an upload takes
  them twice, its part file and then the copy a rename makes. Started
  while the cleanup still has sectors to erase, it can run out partway and
  fail, leaving the file as it was (WEB-API-18). The cleanup has the most
  to erase on a store whose area held no alfs files, all of which it erases
  (DD-100), and after an upload past the room.
- **Why "the cleanup is done" and not "the file fits":** the board cannot
  tell the free pages that are erased from those awaiting the cleanup
  without reading them (DD-107), and an upload's size counts twice. The
  cleanup's own state is known at no cost, and once it is done every free
  page is erased.
- **What it costs:** every close leaves the cleanup a pass to make, which
  reads the store to find nothing left: 0.9 s on MK1B-8MB and 2.6 s on
  MK1C after a 100-byte upload. A write behind another waits that long.
- **Measured, 2026-10-20:**

  | | MK1C | MK1B-8MB |
  |---|---|---|
  | The cleanup's pass after a 100-byte upload | 2.6 s | 0.9 s |
  | `upload_www.sh`, four files | 19 s | 12 s |
  | After an upload past the room, a write refused for the erase; the cleanup then done | 1,888 sectors in 49 s | 833 sectors in 30 s |
  | `support/api_check.py` | 81/81 | 81/81 |
  | `support/failed_write_check.py` | 12/12 | 12/12 |

### DD-107: The Cleanup At The Flash's Pace On USB; A Filesystem Tab
- **Decision:** at the user's direction -- "Erase at full speed when on
  usb. Provide a filesystem maintenance tab on the web ui.":
  - On USB with test mode off, where no launch can be declared (USB-01),
    the pad's cleanup takes the next sector at each pass of the storage
    task, one after another. Otherwise it erases one a second
    [USB-10] (`vfs_flash_clean_wait_ms()`).
  - `GET /api/fs` reports the flash store: its size, the bytes in files,
    the bytes free (pages the cleanup has yet to erase among them), the
    cleanup's state (`done`, or erasing `fast` or `slow`) and the sectors
    it has erased since the start; the store's sectors, their pages and the
    file bytes a page holds; and each file's path, its size once closed,
    and whether it is the board's own (WEB-API-17). It answers from the
    file table and the cleanup's own count, and is refused with 423 in
    flight [WEB-API-19].
  - `GET /api/fs/scan?sector=K&count=N` reads up to 128 sectors and gives
    how many pages of each read erased, all 1s: a check of the flash, not
    of alfs's format [WEB-API-20].
  - The Filesystem tab shows the report, downloads a file, deletes one once
    the operator confirms (none of the board's own), and reads `/api/fs`
    again every 2 s until the cleanup is done. At the user's direction --
    "the web ui can scan the alfs and keep track of the status in the ui
    memory" -- it scans the store 128 sectors a request when first shown,
    when asked and once the cleanup is done, keeps the result in the page,
    and splits the free bytes into erased and not yet erased, counting each
    sector the cleanup erases since the scan as erased [WEB-UI-07].
- **Why one a second off USB:** an erase on the pad stops both cores: 50 to
  70 ms with no pressure reading, launch detection and the ground
  reference held. The longest erases the datasheets allow, 400 ms
  (BY25Q64ES) and 800 ms (XT25F128F), back to back, would come near the
  watchdog's 1 s.
- **Why the page scans:** `alfs_fs_status()` tells erased pages from pages
  awaiting the cleanup only by reading every erased page whole, in one
  call: 5.8 s on MK1C and 2.0 s on MK1B-8MB, holding the store's lock and
  the network task. Read a chunk a request, the same pages take 2.4 s on
  MK1C in 16 requests, each under a quarter of a second, and no request
  waits behind the whole.
- **Why the cleanup does not wait for transfers:** uploads are small --
  configuration and Lua scripts -- and the big transfers are log
  downloads, during which alfs's cleanup waits, a reader being open.
- **Measured, 2026-10-19:**

  | | MK1C | MK1B-8MB |
  |---|---|---|
  | After a refused upload, sectors erased | 1,891 in 49 s | 810 in 29 s |
  | The same at one a second | 32 minutes | 14 minutes |
  | `GET /api/fs` | 8 ms | 7 ms |
  | A scan of every sector | 2.4 s, 16 requests | 0.8 s, 8 requests |

  No sensor fault latched on either through the erases. An upload made
  while the cleanup erases runs slower: 1 MB to MK1B-8MB took 30.7 s with
  the cleanup idle and 49.7 s while it erased. Every upload leaves its part
  file's pages for the cleanup (a rename is a copy, DD-100), so an upload
  close behind another meets it.

### DD-106: Every Reading Used; One A Flash Operation Ran Beside Counted
- **Decision:** at the user's direction -- "Take a note of the noise, but
  keep all samples. For now let us assume the noise is mitigated with the
  filters." -- a conversion a flash erase or program ran beside is used like
  any other [SNS-PRES-14]. The flash layer advances `flash_op_seq` with every
  erase and program, before interrupts return; the collector notes it as a
  conversion starts and compares it as the conversion is read
  (`src/hal_common/collector_bus.h`). A changed count marks the reading,
  which is counted (`pres_flashed` on `/api/status`) and traced ('F' a
  pressure, 'G' a temperature), and nothing else. An MS5607 cycle's
  temperature always goes on the line, and its pressure is always
  compensated and fed (`ms5607_cycle()` in `src/ms5607_driver.h`).
- **The noise it lets in,** from `docs/pressure_collector.md`: departures
  from a 9-point running median, the MS5607s on a battery, the BMP280 on USB
  alone.

  | Sensor | At rest, rms | Beside a flash operation, rms | Worst beside one |
  |---|---|---|---|
  | MK1B, MS5607 | 2.3 to 2.4 Pa | 8.5 to 17.0 Pa | 43 Pa |
  | MK1C, MS5607 | 2.4 to 2.6 Pa | 3.4 to 3.9 Pa | 12 Pa |
  | MK1B, BMP280 | 2.5 Pa | 2.3 to 3.0 Pa | -- |

- **Taken on trust:** that the flight's filters absorb it (task EST-7). The
  host flights put 1.2 and 9 Pa rms on every reading (`test/board_harness.h`)
  and no disturbed reading among them. A bench flight replaces the sensor's
  pressure with the profile's, so it shows every reading arriving, not what
  their noise does. MK1B's MS5607 is the case that matters, and no board on
  the bench has it and flies: both MK1B-8MBs carry BMP280s.
- **Why no reading is dropped:** every reading is a log row [FLT-LOG-07].
  Each second's sync programs pages in consecutive periods, so dropping the
  readings beside them would leave a step in the log about once a second,
  and a minute of uploads would latch `sensor_lost` [SNS-PRES-17].
- **Measured, 2026-10-19,** `support/bench_flight.py --apogee 500` on
  MK1B-8MB (BMP280, a reading each 14.5 ms):

  | | Readings beside a flash operation | Log rows | Widest step between rows |
  |---|---|---|---|
  | Each one used | 1,074 | 4,898 | 15 ms |
  | Each one dropped | 931 | 3,988 | 363 ms, 3 s after launch; 73 to 146 ms, 5 to 10 readings, about once a second |

  MK1C's 3 km flight: 1,477 readings beside a flash operation, 9,422 rows,
  the widest step 19 ms, one reading. A minute of uploads
  (`support/failed_write_check.py`) latched no fault: 1,857 readings beside
  the writes on MK1B-8MB, and 14,307 beside MK1C's 2.3 minutes of them;
  dropped, the same uploads latched `sensor_lost` on MK1B-8MB.
- **An erase on the pad stops both cores** (DD-107), so no conversion runs
  during it: a gap in the readings of up to about 60 ms, once a second while
  the cleanup has sectors to erase.

### DD-105: A File Deleted; A Failed Write Let Go
- **Decision:** at the user's direction ("add a delete api ... add support
  to clean up a failed write"):
  - `POST /api/delete/<path>` removes a file from every store that holds it
    [WEB-API-17]: 200; 404 for none; 409 while it is open; 400 for a name
    WEB-API-14 refuses; 403 for the board's identity (`serial.txt`) and its
    pad record (`pad.mkr`), which the board keeps on its own flash whatever
    a card holds. In flight it is refused with 423, as every storage route.
  - A writer whose close fails stays open in alfs, and only a mount lets it
    go. `src/vfs_flash.c` counts the handles open, and once none is it
    mounts the store again, as a restart mounts it: the failed file is as a
    power cut would have left it, a log read to its last synced page, and
    an upload's part file, which the upload then removes, goes
    [WEB-API-18]. A remove asked for while another file is open is done at
    that mount. A rename whose copy fails is let go the same way, and
    removed where it would have been the name's only copy, so the file the
    upload was for reads as it did.
- **Why a mount:** alfs has no call that gives up a writer, and the library
  is used as it is. A mount reaches the state a restart reaches, which
  alfs's power-loss tests cover.
- **Measured, 2026-10-19:** `support/failed_write_check.py` uploads past the
  room the board reports. MK1C refused it after 7.9 MB, twice; MK1B-8MB
  after 4.0 MB. On each the part file was gone, its name took a new upload
  within a second, and the board did not restart. The upload's pages wait
  for the pad's cleanup (DD-107).

### DD-104: The Flight Path In RAM, Core1 In Slots
- **Decision:** at the user's direction -- "Move all flight code (launch to
  landing) to RAM so no flash executed after launch until after landing",
  and then: "lua can run from flash in flight. Logging has a small flash
  interruption. If erases are needed in flight (insufficient reservation)
  then the erase will use suspend/resume to keep the lua code steady. Give
  the logger 3ms of every 10 and lua 6.5ms of every 10 so the total
  non-critical flight code takes 9.5ms giving 0.5ms of buffer. Critical
  flight code runs in ram so it cannot be disturbed by the flash
  operations."
  - The flight task, every interrupt handler core0 takes, the idle tasks and
    the kernel paths they take run from RAM [FLT-RT-02].
  - From the first period airborne until landing a flash operation runs on
    core1 with core1's interrupts off, and nothing else: no priority raised,
    no lockout of core0 [FLT-RT-03]. Core1's tasks -- the log writer, Lua,
    the network, the timer daemon -- run from flash and wait while it lasts.
  - The period is 10 ms [FLT-RT-04], set in `src/loop_period.h`, and core1
    runs in its frames: the log writer from the period's start for 3 ms,
    then Lua's tick, at most 6.5 ms and done by 9.5 ms; 0.5 ms spare
    [FLT-LOG-09, LUA-RUN-04].
  - A flash operation starts in the writer's slot only if, by the chip's
    datasheet, it ends or can be suspended before the slot does. One that
    waits for a slot starts at the slot's start, from an alarm interrupt on
    core1: the writer's task shares core1 with the network task at its
    priority, which a notification does not preempt, and would wake up to a
    millisecond into the slot. From launch to landing an erase comes only
    when the log has no erased page left, and only on a part whose erase
    ends in pieces; it is suspended at the end of each slot and resumed in
    the next [FLT-LOG-10].
  - A chip holding an erase suspended takes no other erase, and holds it
    through a reboot of the RP2040. An operation the chip holds suspended is
    run to its end at every start, and on the ground by the first operation
    after a flight that gave one up [FLT-LOG-11]. An erase it ignores reads
    as ended at once, so a piece started afresh never lets go of what is
    held; only the held operation resumed to its end does
    (`flash_slot_held()`).
- **The period:** the collector runs free of it (DD-093), so a period steps
  none or one sample. MK1C's arm pump is sized from the period
  (`boards/mk1c/arm_pump.h`).
- **Placement:** `support/flight_ram_link.py` runs every firmware link. It
  links once, has `support/prove_flight_ram.py` find the flight path in
  that image -- every function the roots reach by direct branch, through
  the tables they read and through the pointers named for run time -- and
  the read-only sections those functions refer to by relocation, then links
  again with those sections renamed `.time_critical.flight.*`, which the
  linker script copies to RAM at start-up. Objects of the SDK, the kernel
  and the C library are renamed in copies; their sources are not touched.
  A C library built without a section per function, as Ubuntu's newlib
  is, keeps each function in its member's `.text`; such a member is
  renamed whole when every function in it is on the path. The SDK's
  assembly float, double, divider and memory shims go whole, by their
  `PICO_*_IN_RAM` switches. CI proves every image with `--strict`:
  no function of the path, no read-only byte it reads and no indirect
  branch it makes is left in flash or uncovered.
- **What the path gives up to fit:** MK1C's path is 299 functions and
  42.5 kB of code. To fit beside the rest of RAM: telemetry, the
  estimator rows, the fire diagnostics and the script's status line are
  written by `src/small_fmt.c`, an integer formatter whose output is the C
  library's character for character, so the library's printf stays in
  flash; `/api/status` is taken by the flight task as numbers and named by
  the net task; MK1C claims its presence alarm and loads its arm pump's
  program at start-up; the sample ring holds 16 (no more than four ever
  wait, FLT-RATE-06); the host log's ring holds 4 KB, 2.5 to 3 s of log;
  and the SDK's default alarm pool, whose interrupt would run on core0, is
  off, since nothing schedules in it. Room left in main RAM: MK1C 3.7 kB,
  MK1B-8MB 4.8 kB with its bench flight, MK1B 6.7 kB, MK1A 8.2 kB.
- **What flight refuses:** every flash operation but the writer's; a
  mailbox call (`flight_call()`) whose function lives in flash; a reset
  asked for over USB, which waits for landing. Entering flight stops the
  buzzer, silent from launch already (FLT-LAUNCH-05). Flight begins at the
  head of the first period that starts airborne with the log running, and
  the log opens only then; landing ends it, waiting out a piece in
  progress, before the flight task runs from flash again.
- **The chips:** `src/flash_timing.c` holds the worst-case figures of the
  four fitted parts. W25Q16JV and W25Q128JV suspend a program or an erase,
  tSUS 20 us. XT25F128F does too, but a suspend may not follow a resume by
  less than 500 us, and an erase can take 800 ms. BY25Q64ES's datasheet
  suspends only an erase, and on MK1B-8MB that erase, resumed and suspended
  in each slot, had not ended after 2,000 slots: it does not erase in
  flight, and its log has the room erased before launch. Its program,
  2.4 ms at worst, starts only in a slot's first 300 us. A part
  not in the table programs whole from a slot's start and does not erase in
  flight. A piece also pays about 100 us a command through the SDK's
  `flash_do_cmd()`: the plan keeps 300 us for a start and 500 us for a
  suspend.
- **Measured on MK1C, 2026-10-18,** bench flights in test mode on the flash
  log: from launch to landing no lockout, no loop overrun, the loop at most
  97 us late, Lua's tick every period (100 a second). A 5 km flight with
  too little erased room: 60 sector erases in flight in 690 suspended
  pieces, the longest piece 2.95 ms; Lua lost no tick. Pressure conversions
  a flash operation ran beside (DD-106): 6 to 11 a second of the 106,
  pressures and temperatures together.
- **Measured on MK1B-8MB, 2026-10-19,** 500 m bench flights on five
  starts: every slot waited for ran its piece, 1,264 to 1,273 programs a
  flight, nothing dropped.
- **Measured on MK1C, 2026-10-19:** a 3 km flight after an upload had
  filled the filesystem, 52 erases in flight in 488 suspended pieces,
  nothing dropped, no loop overrun.
- **Open:** the writer's own processing between pieces can run past its
  slot -- about four frames a second in a flight, and most of a frame when
  alfs searches a nearly full filesystem for a free page -- and Lua shares
  core1 with it then: on MK1C with the filesystem full, 33 Lua ticks given
  no time. FLT-LOG-11's finish is tested on the host only.
  `/api/log/space` counts pages awaiting the cleanup as room, which a part
  that does not erase in flight cannot use: 2,272 s reported on MK1B-8MB
  with a few sectors erased.

### DD-103: The Flight Log To The Host In Test Mode
- **Decision:** at the user's direction ("Give the USB flight test mode the
  option to prevent logging to flash and log to network"), test mode set
  with `POST /api/test_mode/on?log=host` holds the next flight's log in RAM
  for the host in place of the flash [USB-09]. The choice lives with test
  mode: it starts off at every start, a log already running keeps where it
  goes, and `/api/status` reports it (`test_log`).
- **How the host reads it:** the storage task hands the log's ring to a
  ring of its own (`src/log_live.c`, 4 KB, DD-104) in place of the file; `GET
  /api/flight/live?since=N` returns the offset of the first byte, the offset
  the log has reached, then the bytes [WEB-API-16]. A host that falls more
  than the ring behind finds the oldest bytes gone and can count them. A
  poll is an ordinary request, framed by Content-Length [WEB-HTTP-02].
- **The bench:** `support/bench_flight.py --host-log FILE` polls it once a
  second and saves the binary log; `pyro_sim --csv FILE` renders it with the
  board's own renderer, and `pyro_sim --replay` flies it again.
- **Measured on MK1C, 2026-10-18,** a 3 km bench flight: 223,622 bytes to
  the host, none lost, 9,416 sample rows, the widest gap 19 ms; no flash
  program or erase from launch to landing; the loop's lateness at most
  0.6 ms, against 12 ms with the log written to the flash. Six programs
  follow at LANDED. The replay of the saved log decides apogee 19 ms from
  the log's and pyro 2 and landing at the same millisecond.

### DD-102: smallest_tcp Is The Network Stack
- **Decision:** at the user's direction ("Replace lwip with smallest_tcp.
  This should reduce flash and ram usage"), the USB network runs on
  smallest_tcp v0.1.12 (github.com/n9wxu/smallest_tcp), IPv4 only. The image
  carries no other TCP/IP stack, and no DNS server.
- **Where:** `src/net_glue.c` carries TinyUSB's frames to and from the stack
  and runs its one-client DHCP server and its mDNS responder;
  `src/http_transport.c` is HTTP's sixteen links, the stack's buffer
  operations written over each exchange's own rings, so a byte is copied
  once each way and a sent byte stays in the tx ring until it is
  acknowledged; `src/http_server.c` lends its four exchanges through
  `http_exchanges_t`. The transport is tested on the host against the real
  stack, with a second instance of it as the browser
  (`test/test_http_transport.c`).
- **Memory:** the stack holds no heap and no pools: two 1,514-byte frame
  buffers, the links' records and a 256-byte transmit queue.
- **Held frames [WEB-NET-05, WEB-NET-07]:** the stack sends a refused SYN,
  data or FIN again at its next pass, but not a bare acknowledgement or a
  window update, and a lost window update leaves a host at a shut window
  until its own probe, seconds later. So a frame the USB endpoint cannot
  take yet is copied into 256 bytes of held frames (`src/net_txq.c`) and
  sent in order as soon as it can. A full segment never fits; TCP has it.
- **What the stack lacks, worked around here** (smallest_tcp issues 7 to 11,
  open): one segment in flight, so the transport shows the stack one segment
  at a time and each carries PSH (macOS otherwise delays one acknowledgement
  in fifteen by 100 ms); `NET_API_PREFIX` renames the firmware's own
  `net_init()`, `http_conn_init()`, `http_reason()` and `http_server_init()`
  in files that include the stack, which `src/net_stcp.h` undoes; the stack
  wants a receive buffer it never writes.
- **Randomness:** TCP's initial sequence numbers and mDNS's probe delays
  must differ from boot to boot, not be secret. They are seeded from the
  ring oscillator's random bit (`rp2040-datasheet_2025-02-20.pdf`, section
  2.17.5, page 223) and the timer.
- **For the host:** `pyro.local` resolves by mDNS alone; there is no DNS
  answer on port 53. DHCP serves the one host on the link 192.168.N.2.
  `/api/net` reports TCP's links by state, retransmissions, the transport's
  counts and the frames each way [WEB-API-13].
- **TinyUSB's RNDIS replies** take one constant from an Ethernet header;
  `src/net_shim/netif/ethernet.h` gives it, and TinyUSB is unchanged.

### DD-101: Every Sample Logged
- **Decision:** at the user's direction ("The 1hz and 50hz log capacity is
  roughly the same. Drop the 1hz logging and log every flight cycle"), the
  flight log keeps a row for every sample, every flight cycle, and every
  event row at its own time [FLT-LOG-07]. There is no log rate to set.
- **Why the capacities are close:** the log syncs every second
  [FLT-LOG-06], and on alfs a sync stores its partial page (DD-100), so a
  second of log takes at least a page however few its rows. Every sample
  takes about six: measured on MK1C on 2026-10-18, 1,042 pages over a
  177.5 s bench flight, 5.9 a second, for 1,210 bytes a second of samples
  (50.9 a second), estimator rows and the script's rows. A BMP280 gives 69
  samples a second at MK1B's 100 kHz bus, so a second there takes about
  seven pages. MK1C holds about 1.5 hours of flight in its 8 MB of files,
  MK1B-8MB about 39 minutes in its 4 MB, and MK1B, with an MS5607 in 984 kB,
  about 11 minutes.
- **The format:** the header keeps its plan byte, always 2, and the CSV its
  `# Log rate: every sample` line, which the replay reads; a CSV whose
  header says it kept a row a second is refused [DAT-08]. A `config.ini`
  that names `log_rate` loads whole, and its next save drops the key
  [CFG-08].
- **`/api/log/space`** reports `rate_hz`, the samples a second, and
  `seconds`: the room over what a second of samples takes on the store once
  synced, whole pages on the flash (`vfs_synced_cost()`). The Config tab
  shows that time [WEB-UI-06, WEB-API-12].
- **In flight** the flight steps on every sample it has [FLT-RATE-06], and
  every reading is used, a flash operation beside it or not (DD-106).

### DD-100: The Board's Files On alfs; Only The Cleanup Erases
- **Decision:** at the user's direction ("build and test this version.
  Integrate into the pyro_fw and test on hardware"), the board's own files
  live on alfs (github.com/n9wxu/alfs, format 6) in the area at the top of
  flash. `src/vfs_flash.c` puts alfs behind `vfs.h`; `src/fs_flash.c` is its
  driver and the store's lock.
- **What a file is:** a chain of pages found by its name (at most 31
  bytes). A write replaces a file whole: the new copy is written beside the
  old and takes its place when it closes, so a write that dies part-way
  leaves the old file. A rename is a copy. A sync stores the partial page
  too, at the cost of the rest of that page.
- **The flight log syncs once a second** while it has unsynced bytes, in a
  pass of the storage task that wrote nothing else (`LOG_SYNC_MS` in
  `src/hal_common/hal_common.c`): a flight that never lands -- a crash, a
  battery that lets go on impact -- loses at most the last second
  [FLT-LOG-06].
- **Erases:** a write takes only erased pages, and only `vfs_clean()`
  erases: a sector a pass of the storage task (DD-107), never while the
  flight log holds the filesystem, so a log in flight appends without an
  erase (the user's rulings of 2026-10-14). The flight log's open removes
  the last flight's log first, so a reset in flight finds this flight's.
- **Programs are exact:** alfs programs only the bytes it means, which
  `flash_range_program()`'s whole pages cannot do, so
  `flash_op_slot_program()` sends the page program itself, from RAM. Its
  driver asks again when `flash_op()` refuses for a board's pyro sequence
  or a helper that would not park, up to a deadline; alfs takes a failed
  program for a bad sector.
- **Mount:** reads every page's header once, at boot, before the scheduler;
  writes nothing, so the reads before `hal_fs_mount()` use it read-only. An
  area holding no alfs files -- blank, or another format -- mounts with no
  file, and the cleanup erases what it holds. A board whose area holds
  another format therefore starts with no configuration, pin or beep
  tables, web pages, script, assigned MAC or flight log; they are saved off
  the board before such an install and put back after (`install.py` for
  the pages).

### DD-099: Identity From The Flash Chip's Whole Serial
- **Decision:** at the user's direction ("update the random MAC to use the
  full serial numbers to ensure a fully unique serial number across all
  flash devices"; "use the board flash ID's to seed the subnet byte"), the
  MAC, and with it the USB serial and the subnet, is derived from the flash
  chip's whole unique ID (DD-098) [WEB-NET-06]: the ID's bytes are mixed
  through splitmix64's finaliser, so every input bit reaches every output
  bit (`mac_from_uid()` in `src/mac_random.c`). 0x02 leads (locally
  administered, unicast). The subnet is the MAC's last byte,
  192.168.<last>.1, never 0 or 255, which are awkward subnets, nor 1, the
  commonest home LAN's. `/api/status` reports `mac_source` `flash` or
  `assigned`.
- **An assigned MAC wins:** one an operator assigned through `/api/serial`
  is kept in `/serial.txt` and used, because it is how two boards that meet
  on one subnet are parted. A file whose second line is `rng`, a MAC drawn
  at random and kept, is ignored: the flash ID gives the MAC.
- **Why:** the SDK's 8 bytes of the ID are the half two chips can share
  (DD-098); the whole IDs differ. A derived identity needs no storage: a
  blank flash keeps its address, and no web page has to assign one.
- **What it does not fix:** the subnet is one byte, so two boards share one
  at the birthday rate, about 4 % at 5 boards and 16 % at 10.
  `register_board.py` reports it, and `/api/serial` parts them.
- **The bench boards' addresses:** MK1C 02BB65F06D47 (192.168.71.1),
  MK1B-8MB 0237CA1FB1A0 (192.168.160.1) and 0265BA5C7AFC (192.168.252.1).
  MK1A (13) and MK1C-SD (42) carry assigned MACs.
- **No address is fixed anywhere,** at the user's direction ("do not
  hardcode the MAC. I cannot promise to plug the same HW into the test
  fixture"). `test_mac_random.c` checks, on made-up IDs, that every bit of
  an ID moves the MAC and that IDs differing only in their last bytes spread
  evenly over the subnets. `install.py` and `update_from_release.py` take
  the one board attached unless `--host` names another (`attached_board()`
  in `support/pyro_http.py`), and `flash_picotool.sh` waits for whichever
  link comes up new. So no test fails if the derivation changes, and a
  change moves every board's address: it is a review matter.

### DD-098: The Flash Chip Says Its Size And Its Whole Unique ID
- **Decision:** at boot, before the scheduler, the firmware reads the flash
  chip's JEDEC ID (9Fh) and its unique ID (4Bh), and `/api/status` reports
  them as `flash_jedec`, `flash_kb` and `flash_uid` (`src/flash_chip.c`,
  `src/flash_chip_read.c`). The JEDEC ID's third byte is the log2 of the
  capacity; outside 10h to 19h it is no size, and the chip is unknown. The
  unique ID is read at its maker's length: 8 bytes for Winbond
  (`W25Q16JV_RevI_2024-12-24.pdf`, p. 46), 16 for XTX
  (`XT25F128F-W_Rev0.6_2022-02-26.pdf`, p. 23) and BYTe
  (`BY25Q64ES_Rev2.9_2024-10-29.pdf`, p. 45).
- **A chip smaller than the image's flash map makes the image another
  board's** (FLT-BOOT-17). The self-test fails; the filesystem is neither
  mounted nor formatted (FLT-BOOT-19); `/api/ota` answers 409 (OTA-08); the
  board is in FAULT, named `board_mismatch`, checked before the filesystem;
  after an update the image declines to commit and rolls back, and after a
  `picotool load` it stays down. The rollback is not tried on hardware, by
  the user's decision ("leave the rollback untested", 2026-10-14).
- **Why the chip and not the stamp:** the stamp lives in the filesystem,
  which on such a chip lies past its end; the chip wraps the address onto
  its start, so a mount that failed and formatted would write over the
  bootloader. The download slot wraps there too, so an update cannot be
  taken either.
- **An unknown chip decides nothing,** as a missing stamp decides nothing: a
  part whose third byte is not a size (Adesto's AT25SF128A answers 1F 89 01)
  must not stop a board.
- **Why the whole unique ID:** the SDK reads 8 bytes
  (`FLASH_UNIQUE_ID_SIZE_BYTES`). XTX's and BYTe's IDs are 128 bits, unique
  to each device by their datasheets, and the 8 bytes kept are the half two
  chips can share: both MK1Cs read 41503459373331FF, ASCII "AP4Y731". The
  board's identity comes from the whole ID (DD-099).
- **Read on the bench, 2026-10-13:**

  | Board | `flash_jedec` | `flash_kb` | `flash_uid` |
  |---|---|---|---|
  | MK1A | EF4018 | 16384 | E46214B0632D472F |
  | MK1C-SD | 0B4018 | 16384 | 41503459373331FF FFFFFF1500640027 |
  | MK1B-8MB | 684017 | 8192 | 35343239330594F3 FFFCA50EFF851D20 |
  | MK1B-8MB, the second | 684017 | 8192 | 35343239330604F4 FFFC82D6FF361D20 |
  | MK1C | 0B4018 | 16384 | 41503459373331FF FFFFFF15003D0025 |

  The two MK1Cs share the 8 bytes the SDK reads and differ in the last
  four of their 16: the whole ID tells them apart.

### DD-097: The Refitted MK1B Is A Board Of Its Own
- **Decision:** an MK1B refitted with an 8 MB BY25Q64ESHIG at U7 and the
  base AP2192 (AP2192MPG-13) at U5 is the board `mk1b_8mb`, "Pyro
  MK1B-8MB". It builds MK1B's sources against its own SDK header, which
  gives the 8 MB flash, keeps 4096 kB of files and two 1900 kB application
  slots, and defines `BOARD_PYRO_U5_DISCHARGES_OUTPUTS` 0, so the continuity
  check judges each channel (B-FL, B-U5).
- **Why a board and not a setting:** the flash map is fixed when the
  bootloader is linked. The 8 MB image keeps its files from 4 MB; on MK1B's
  2 MB chip that address wraps to 0. The two images must not be confused,
  and every check that keeps images apart keys on the board id; DD-098 makes
  a chip too small for the image a FAULT.
- **The web page reads a board id with an underscore:** its filename check
  parses `fw_mk1b_8mb_fota.bin` and MK1C-SD's images, and refuses an image
  named for another board. `update_from_release.py` and `install.py` name
  images by the id the device reports.
- **The build refuses a flash size its SDK header does not give.** The
  bootloader's slots come from the header's `PICO_FLASH_SIZE_BYTES`, so a
  `PYRO_FLASH_SIZE_KB` that disagrees with it fails the configure rather
  than printing a slot map the bootloader would not use.
- **4096 kB of files:** half the chip, as MK1A and MK1C keep half of
  theirs.
- **Measured 2026-10-13 on the first board (0244102D6F98):** it boots on the
  W25Q080 boot stage 2; both empty connectors read open, 3451 and 3511
  counts; `api_check.py` 52 of 52, `http_stream_check.py` 20 of 20,
  `pressure_trace.py` good with 7.3 Pa rms on USB alone, no flash refused,
  no loop overrun.
- **Open:** a 1 ohm load reading good and a short reading shorted (task
  B-U5).
- **Released** as `fw_mk1b_8mb.uf2`, `fw_mk1b_8mb_bootloader.uf2` and
  `fw_mk1b_8mb_fota.bin`, at the user's direction ("Add MK1B-8MB to the
  release").

### DD-095: The Review Of 2026-10-02, Applied To Main
What the code review of 2026-10-02 settled:
- **Two more start-up faults.** An image built for another board is FAULT at
  every boot (FLT-BOOT-17). A `config.ini` that is there and cannot be read
  is FAULT and the file is kept (FLT-BOOT-18): only a missing file is
  written with defaults. Both sound the general fault.
- **A configuration value its field cannot hold is refused** (SYS-CFG-03).
  The field keeps what it had; a post carrying one is answered 400 and
  nothing is stored. Board limits still clamp (PYR-BOARD-02): refusal is for
  what cannot be represented.
- **Same origin.** A POST needs `X-Pyro: 1` and a Host that is the board's;
  no response grants another origin access (WEB-API-07). A browser page
  from elsewhere cannot set the header without a preflight the board does
  not answer.
- **Flight events reach a script through the flight's own record.** The
  flight keeps its last eight events and the Lua service follows them
  (LUA-RUN-02); the flight software calls nothing in Lua (SYS-LUA-02).
- **The script checker's arena is 16 kB**, static: MK1C's RAM does not hold
  20. A script needing more to compile is refused at save. Nothing in the
  firmware calls `malloc`.
- **The bench's side of the application is built for size**: the HTTP
  server, the status and configuration text.

### DD-094: Rulings Of 2026-10-03 On The Open Decisions
The user's rulings of 2026-10-03 on open questions; the work still to do is
in `docs/outstanding_tasks.md`.
- **Any fault found before launch sounds a fault (SNS-PRES-17).** A sensor
  that stops or sticks on the pad is a general fault, announced and named,
  until the board is restarted: a board must not go on saying OK to fly
  with its sensor gone.
- **The tests' 9 Pa is margin (NS-1).** Boards on a battery measure 1.9 to
  2.6 Pa; 9 Pa is MK1B on USB alone. The tests fly at it.
- **MK1B's slot (SL-1):** while the image fits, nothing changes; MK1B-8MB
  (DD-097) has the room.
- **The three speed defaults stay 0 (R-1).**
- **Every pyro pulse is ended by hardware (R-2):** a hardware timer, or a
  fully defined PIO one-shot, for all pyro operations on every board. To
  build: MK1A, MK1B and MK1B-8MB end a pulse from the loop.
- **Every build runs a script (R-3):** the default script is a hello world
  that prints the flight's events as they change and claims no I/O, so
  MK1C-SD, whose J3 carries its SPI bus, runs one too. To build.
- **The flight log is prepared on the bench (C6).** A Log tab downloads,
  erases and prepares the log. Preparing creates an empty binary log for a
  chosen duration, 2 minutes by default: the file is opened, erased and
  closed on the bench. On the pad it is opened before OK to fly sounds, so
  the pad is quick. A start-up is the pad unless USB is found; then every
  file is closed and the board is USB's. In test mode the log records the
  test. A written log stays until the user clears it on the bench. To build.

### DD-093: A Free-Running Pressure Collector That Recovers Its Own Bus
- **Decision:** `src/pressure_collector.c` is one interrupt state machine for
  either sensor [SNS-COL-01..06]. It commands a conversion, waits out the
  part's worst-case conversion time on an alarm, reads, and commands the
  next, with no start from a task. Each cycle is the part's raw bytes and
  the time of each conversion. A queue holds four; a fifth pushes the
  oldest out, counted. The flight task takes them each period, does the
  arithmetic, and steps once for every sample waiting (FLT-RATE-06). A
  sensor is a table (`MS5607_PART`, `BMP280_PART`): its commands, its
  conversion times, where its result is read, and its reset.
- **Why:** a read can only follow its own command's alarm, so a zero code
  (MS5607 datasheet page 11) is impossible by construction, not caught
  afterwards; and no task's lateness moves a conversion.
- **The MS5607's cycle** is a pressure conversion and then a temperature
  conversion, both at OSR 4096, each read 9.1 ms after its command (the
  datasheet's worst case is 9.04 ms, page 3) and stamped at its middle.
  Each pressure is compensated with the temperature at its own time, on the
  least-squares line through the last four temperatures, carried no more
  than 200 ms past the newest (`ms5607_temps_at()`). Never a temperature
  reused as read: with the datasheet's example coefficients a temperature
  1 °C stale moves the pressure about 240 Pa. A line through several
  readings rather than two, because a two-point slope carries both
  readings' noise into every pressure.
- **The BMP280's cycle** is one forced conversion, x4 pressure and x1
  temperature, no filter, read in one burst of status and data. The
  datasheet recommends forced mode for "host-based synchronization"
  (section 3.6.2, page 16): every reading is a conversion of its own and
  its time is known. Its stamp is the command's time plus 7.5 ms, the
  middle of the pressure's measurement: Table 13 (page 18) gives the whole
  at 11.5 ms typical and 13.3 ms at most, and its rows add 2 ms typical a
  pressure oversample, so x4's pressure is the last 8 ms.
- **Rates:** MS5607 at 400 kHz, 18.8 ms a cycle, 53 a second. BMP280,
  13.7 ms at 400 kHz and 14.5 ms at MK1B's 100 kHz. At the 10 ms period a
  period steps none or one sample (DD-104).
- **No wait in the handler:** a transfer is queued whole (the controller's
  TX FIFO holds sixteen) and its outcome is read at the next alarm.
- **Recovery is states of the same machine:** a failed transfer is counted by
  cause and the cycle starts again 1 ms on. A transfer that did not finish is
  aborted (RP2040 datasheet 4.3.10). Three failures in a row: the pads are
  taken from the controller, the controller is reset, nine clocks and a STOP
  go out at 10 us an edge (UM10204 3.1.16), the controller is set up again as
  it was, the part's reset command is sent, and its start-up is waited out
  (MS5607 page 12). About 4 ms. The flight software does not recover a
  sensor; the collector recovers its bus (SNS-REC-01, SNS-COL-05).
- **No judgement of a value** (SNS-PRES-06). `support/compensation_range.py`
  shows neither sensor's arithmetic bends, wraps or overflows on any code.
  What stays is the logarithm's domain: a pressure of zero or less is not
  fed to the estimators.
- **Accuracy, in the task:** the MS5607's second-order compensation below
  20 C (datasheet page 9; 277 Pa at 0 C and sea level) [SNS-PRES-15], and
  its PROM's CRC checked at detection (AN520) [SNS-PRES-16].
- **The handler runs from RAM** (SNS-COL-06), which `support/prove_core0.py`
  proves of the image: it is an if-chain compiled without jump tables,
  builds each cycle in its queue slot rather than by struct assignment, and
  wraps its index by comparison, so no jump table, `memcpy` or library
  division puts it in flash. Detection uses the SDK's calls, before the
  collector has the bus.
- **The bench profile carries the sensor's noise (SIM-02):** each reading's
  own departure from the ground reference rides on the profile, since a
  profile of one value repeated is a stuck sensor.
- **On the bench, 2026-10-03:** MK1C, MK1C-SD and MK1B (MS5607) at 53.0 a
  second, 18.87 ms apart to within 0.1 ms; MK1A (BMP280) at 72.8 a second.
  No zero, no failed transfer, no cycle pushed out, all three PROMs pass
  their CRC. The recovery states have run on the host's fake bus only (task
  REC-1).
- **Every value is followed:** half a second of readings 1500 hPa high a
  second before apogee is data, and the drogue comes 8.6 s after apogee on
  that test, never before it (task EST-3).

### DD-092: Estimators Behind One Interface, All Flown, One Obeyed
- **Decision:** the filtered state comes from an estimator behind
  `src/estimator.h`: a table of functions and a name. The build carries the
  ones `src/estimator_table.c` lists. Every one is fed every reading. The
  flight obeys the one `estimator` in config.ini names, and logs what the
  others report and when each would have called apogee [SNS-EST-06..09].
- **The two carried:**
  - `constacc`, the default: the constant-acceleration Kalman filter of
    DD-085, behind the interface, tuned as DD-113 sets out.
  - `lumped`: one equation of motion from pad to ground,
    dv/dt = a_T - g - beta (rho/rho_pad) v|v|, learning a_T and beta from the
    readings (`docs/lumped_parameter_filter.md`). `src/estimator_lumped.c` is
    the C port of `sim/study/lumped.py`, and
    `sim/study/lumped_port_check.py` flies both on the same readings. It
    has flown only in simulation, and logs beside `constacc` until it has
    real flights.
- **Replacing one:** in a build, link another table. In the field, set
  `estimator` and restart.
- **Apogee is the model's verdict:** an estimator says whether its model
  explains the readings (SNS-EST-08). Apogee is the obeyed estimator seen
  climbing, then seen falling, explained throughout, with the readings
  tracking its height; a fall whose climb was not seen must last 2 s
  (FLT-APO-07, DD-113). A port error near Mach 1 is readings
  the model does not explain, so nothing is decided on it and none of it is
  the peak (FLT-APO-08). There is no Mach flag.
- **One missing reading is not a gap:** the second of evidence an estimator
  needs starts again only after 250 ms without a used reading. A sensor that
  repeats itself for eight readings near apogee would otherwise cost 2 s, on
  2 of 1000 flights to 10 km.
- **Measured:** host flights obeying each, hop to 45 km, cold and hot pads,
  clean ports and three port errors (`test/test_mach.c`): never early with
  either. The tuning in force and its measurements are DD-113's.
- **The log:** `EST` text rows, rationed as script output is. PEAK and
  PEAK_AT_LEAST carry the peak, and the web page reads them. Events 5, 6 and
  8 (LOCK, UNLOCK, LOCK_FALLBACK) are not written; the page reads them in a
  log that carries them.
- **The cost:** every reading runs both filters, and each sample carries
  every estimator's state.
- **A prediction stops at the ground:** the model has no ground. A
  prediction with no reading behind it stops at the pad's level; carried on,
  a sensor repeating itself for ten minutes would put it 19 km below the
  pad.
- **Flown on the bench, 2026-10-03:** a 3 km bench flight on MK1C-SD: launch
  at 32 m, apogee called at the profile's apogee by both estimators, peak
  3000.06 m against 3000, main at 297.8 m against 300, landed at 0.1 m.
- **Open:** `lumped` anchors its air at the first reading's pressure and the
  sensor's temperature then; a long wait on a pad that warms is not followed
  (task EST-4). On the pad its speed reads about 0.5 m/s downward: the thrust
  term's fade pulls against the readings (task EST-1).

### DD-091: What Building The 2026-10 Review Settled
- **Apogee bounds (FLT-APO-01):** measured on the firmware itself, on host
  flights from 60 m to 30 km at sensor noise from 1.2 to 9 Pa: apogee is
  declared never early, and within 0.5 s to 10 km, 1.5 s at 20 km and 2.5 s at
  30 km. 9 Pa is margin: it is MK1B on USB alone (DD-094).
- **OK to fly must be heard (BUZ-CODE-08):** silence means a fault, so a
  personality with a silent OK to fly is refused.
- **The announcement does not wait for the script (SYS-LUA-02).**
- **A commanded restart is not a failed start (LUA-SAFE-01):** the reboot
  route clears the script's start-up mark before it resets
  (`lua_app_restart_commanded()`).
- **A script that fails its check is not saved (LUA-MGT-01):** the page checks
  first and uploads only a script that passes.
- **MK1C never abandons a pulse (PYR-ARM-03):** the gate closes at the
  precharge deadline on whatever the bus has; the timeout is that pulse's own
  record. U9's latch is released by the enable falling after every fire
  (TPS2595 datasheet page 23), which is what PYR-FAULT-01 asks.
- **The bench profile's descent starts from rest (SIM-04):** it gathers speed
  under gravity toward each rate, so a failed canopy is a fall and no speed
  appears from nowhere.
- **No hold on launch or apogee (task T3, 2026-10-03):** with none, every pad,
  flight and Mach test passes, 30 minutes of 30 Pa gusts at 9 Pa of sensor
  noise included: the filter's own lag and its setting aside of bad readings
  for 50 ms are the guard. `launch_detected()` is a pure function.
- **MK1C's presence pulse is ended by a hardware alarm (task P1):** 8 ms, not
  the next period. A fire drops a test in progress, and no storage write
  starts beside the pulse.
- **Open:** the three speed defaults are 0 until the modelling study gives
  values; MK1A and MK1B do not meet PYR-ARM-01's 50 ms (task R-2); MK1C-SD
  has no script (task R-3); HAL validation is not built (task R-9,
  `docs/hal_validation_apps.md`).

### DD-090: Ten Code Requirements, And The HAL As The Test Seam
- **Decision:** the code itself has requirements (CODE-01..10): structure
  and naming before comments; comments for traceability and for decisions
  the structure does not show, never to teach a subject; named functions
  before magic values; one responsibility per file; SOLID and DRY; black-box
  tests traceable to requirements; all code reachable through the published
  interfaces, with no dead code, no test-only functions and no test
  constructs that reach hidden functions; pure functions preferred.
  Libraries from outside the project are exempt and are not modified.
- **The seam:** the HAL interface is the mockable layer through which all
  flight code is tested (HAL-05), and it makes a port a new HAL. The HAL is
  validated separately, on hardware, with test equipment and HAL validation
  applications (HAL-06, BLD-06).
- **Why:** given by the user on 2026-10-03, after the requirements review,
  to govern the review of the tests and the code that follows it.

### DD-089: A Script Cannot Block The Board
- **Decision:** Lua is a functional requirement on every board (section 10
  of `REQUIREMENTS.md`). No script may block, prevent or delay any behaviour
  of the board: starting up, the network, the announcement, logging,
  telemetry, any flight function (SYS-LUA-02). Whatever script is stored the
  board starts and serves its web interface (LUA-SAFE-01). A script has full
  control of the pads the pin assignment gives it, a pyro channel's
  included, and that use is the operator's. The enabled script always runs,
  ground test and bench flight included (LUA-RUN-01).
- **Why:** the user's rulings in the 2026-10 review. A script that could
  hold up a start would turn a bad upload into a board that cannot be
  repaired from its own web interface.

### DD-088: Telemetry Is $PYRO Once A Second, And Carries Its Events
- **Decision:** the downlink is the $PYRO sentence, once a second in every
  state it is sent in (TEL-03). A flight event is queued and carried by the
  next message, so it can be up to a second late and none is lost (TEL-11).
  There is no configurable format or rate. The port accepts no commands
  (TEL-12). A user who needs another format assigns the serial pins to a
  script.
- **Why:** the user's rulings in the 2026-10 review. The log may run faster
  than the downlink; the downlink is for watching a flight, not for
  reconstructing it.

### DD-087: Ground Test By The Switch Alone, And Configuration At Start-Up
- **Decision:** the test switch procedure of DD-071 is the only ground test;
  the serial port takes no ground test commands. Its fires are delivered on
  command with no health gate (GND-TEST-13). The switch may connect the
  buzzer's pad to another pad and may not ground the buzzer; pad to pad, the
  input pad watches the buzzer's own pattern and short pulses find the
  switch while the buzzer is silent (GND-TEST-12,
  `docs/ground_test_on_buzzer_pad.md`).
- **Configuration:** configuration and pin changes take effect at start-up
  (CFG-10). The web interface writes the file; the running system, ground
  test mode included, goes on with what it started with.
- **Why:** the user's rulings in the 2026-10 review. Serial commands need a
  computer, which the user need excludes; and a save that only writes a
  file cannot be refused for the mode the board is in.

### DD-086: Any Restart Resumes A Flight In Progress
- **Decision:** after any restart, whatever its cause, the system resumes
  the flight if the pad's record exists (DD-026), the board is above the
  recorded ground and moving, and no USB host is attached (FLT-BROWN-02).
  The level and the speed come from the estimators run on 600 ms of readings
  (`pp_estimate_after()`); a start that has no estimate within 4 s does not
  resume. A resumed flight measures against the recorded ground
  (`pp_resume_flight()`), assumes no channel has fired, reads the channels'
  health and fires as soon as fresh data meets a trigger; a DELAY counts in
  full from the resume; the emergency rule applies from the resume
  (FLT-BROWN-06). All resume state is cleared when the flight lands and when
  a bench flight ends (FLT-BROWN-04). `/api/status` says whether a start
  resumed and, if not, why (FLT-BROWN-05).
- **Why:** best effort. A watchdog reset in the air that started cold would
  take a ground reference at altitude and fire nothing. A fire into a spent
  igniter is harmless, so nothing about the channels need survive.
- **Open:** restoring T+0, apogee and the peak by replaying the flight log,
  if it is fast enough (`docs/resume_from_log.md`); a resumed flight's log
  (task L3); the resume's timing (task RS-1).

### DD-085: One Estimator On Raw Pressure, And Every Operation In Pressure
- **Decision:** the detectors read one filtered state -- pressure, its rate
  and its acceleration -- formed from the raw readings by a Kalman filter
  whose correction is limited inside its update (SNS-EST-01, SNS-EST-02).
  No median and no low-pass stand in front of it. It discards nothing, is
  not re-seeded and is not loosened at a fire; it trends to data that is
  stable. Every flight comparison is in pressure, and a height or speed the
  operator set is converted once (SNS-EST-05). No altitude is clamped, at
  8000 m or at zero (SNS-ALT-01). DD-092 puts this filter, `constacc`,
  behind the estimator interface beside `lumped`.
- **Apogee at altitude:** filtered pressure must show the slowing climb,
  apogee and descent at every height in the sensor's range. No fallback
  waits for the pressure to return to an earlier level: one would deploy a
  good 30 km flight 150 m above the ground (HA-1).
- **Data is believed:** the board cannot know its sensor is wrong. A reading
  beyond the rated range is used (SNS-PRES-06). A stuck sensor is a chip
  failure: it is logged and nothing recovers it in flight (SNS-REC-01).
- **Why:** the user's rulings in the 2026-10 review. A filter on raw
  readings with an unlimited correction lets one reading 3 to 6 kPa low
  declare a launch; setting aside readings far from the prediction for
  50 ms stops a separation's shock of any size at no cost in delay
  (DD-113). The design and what remains to be
  measured are in `docs/descent_speed_estimator.md` and
  `docs/pressure_domain_flight_math.md`.

### DD-084: A Board Declares Its Constraints
- **Decision:** the requirements are general, and a board may redefine a
  default or narrow a range: `refire_interval` and `fire_gap` for its
  protection part, its sensor's range and noise, its height for proper
  operation, its log capacity and log details (PYR-BOARD-01..04, DAT-09,
  SNS-MAX-01, BRD-01). An out-of-range value is brought to the nearest
  permitted one and reported, never rejected. A constraint may change when
  a channel is energised; it may never withhold a decided fire.
- **Why:** MK1B's 1.5 A PTC needs a different gap from MK1C's eFuse, and a
  configuration file can arrive from another board.

### DD-083: Four Announcements By Priority, And Health That Gates Nothing
- **Decision:** the pad says one of OK to fly, pyro 1 fault, pyro 2 fault,
  general fault; silence is also a fault. One is announced at a time:
  general, then pyro 1, then pyro 2 (BUZ-CODE-02). Each board checks every
  enabled channel for the faults it can reliably detect; one that can detect
  none treats the channel as ready (PYR-HEALTH-01). A short cannot be told
  reliably from a match, so the general verdict is ready or fault and the
  measurement is named on the status report. Only enabled channels count,
  and the pin assignment is the source of truth (PYR-HEALTH-02).
- **Why:** the user's rulings in the 2026-10 review. A general fault goes
  back to the workbench; a pyro fault may be fixed at the pad, one at a
  time. Diagnosis is not for the beep.

### DD-082: Re-Fire, Emergency All-Fire, And The Gap, Without Roles
- **Decision:** after a channel's first fire it fires again every
  `refire_interval` while the descent speed exceeds that channel's re-fire
  speed (PYR-REFIRE-01). At any time after apogee, at
  `emergency_fire_speed`, every enabled channel fires and goes on firing
  until the speed drops or the flight lands (FLT-EMRG-01). A zero speed
  disables its rule. The channels are never energised together, and
  `fire_gap` of quiet separates a pulse on one from a pulse on the other; a
  first fire goes before a re-fire and pyro 1 before pyro 2
  (PYR-DEPLOY-02). The requirements name no drogue or main. The rules act
  on the filtered state (FLT-EMRG-05).
- **Why:** the user's rulings in the 2026-10 review. Any deployment reduces
  the damage, and a pulse into a spent igniter is harmless.
- **Open:** the defaults of the three speeds, zero until the modelling study
  gives them.

### DD-081: Never Early, Data Believed, Best Effort
- **Decision:** a fire is decided only on measured evidence that its event
  has happened: never early, never on an estimate, never on a timer
  (SYS-DEPLOY-04). Samples that arrive are believed; only their absence
  suspends a decision. Once decided, nothing withholds the attempt: health,
  continuity, faults and voltage gate nothing, in flight or in ground test
  (SYS-DEPLOY-05, PYR-HEALTH-01). No fire is refused, and every pulse is
  recorded, in the flight log and on `/api/status`, with what the board
  observed of it (PYR-FIRE-01): `hal_pyro_is_firing()` read straight after
  `hal_pyro_fire()` is the board's report that the channel is energised.
- **Why:** the user's rulings in the 2026-10 review. No correction is
  possible after launch. A safe flight carries two pyro systems, so this
  unit prefers no deployment to an early or uninformed one, and never holds
  a decided one back.

### DD-080: Requirements State Behaviour; Mechanism Is In The Design Record
- **Decision:** `REQUIREMENTS.md` states behaviour that can be verified from
  outside and met by any implementation. It dictates no processor, kernel,
  bus, interrupt or algorithm; the hardware layer may use any of them. A
  mechanism is design, recorded in these decisions and in each board's
  theory of operation. The requirements are general, with board values
  declared beneath them (DD-084). FreeRTOS is the only task model supported,
  as a design fact (DD-073).
- **Why:** the user's ruling in the 2026-10 review.

### DD-079: Descent Rates Are Judged In The Pad's Air
- **Decision:** the descent bands (a main at 10 m/s or less, a drogue at
  35 m/s or less, DD-023) and the fire rules' speeds read the descent speed
  as the same canopy would give it in the pad's air [FLT-AIR-01]: the true
  speed, the rate of ln(pressure) times the air's scale height RT/g at that
  pressure (`atmos_scale_height_m()`), times sqrt(rho / rho_pad)
  (`atmos_pad_air_ratio()`), with the 1976 US Standard Atmosphere's
  temperature at each pressure. The ratio is 1 at the pad, 0.95 1 km above a
  sea-level pad, and 0.22 at 30 km.
- **Why:** a parachute's terminal rate goes as 1/sqrt(rho): a drogue that
  settles at 25 m/s over the pad falls at about 200 m/s at 30 km. Read raw,
  a working canopy high up is past any speed set for the pad, and the
  re-fire and emergency rules would fire on it. A speed no canopy explains
  at that height still trips them.
- **Why the atmosphere's slope:** the standard altitude formula,
  h = 44330 (1 - (p/p0)^(1/5.2561)), is the troposphere's. Above 11 km the
  air stops cooling and that formula reads short: 25.3 km at a true 30 km,
  and a speed 0.54 of the true one. The scale height at each pressure gives
  the true speed.
- **Pinned by** `test_FLT_AIR_01_a_good_canopy_in_thin_air_is_not_refired`.

### DD-078: A Flight On The Bench
- **Decision:** on a board built with `PYRO_HAS_BENCH_FLIGHT` (MK1C,
  MK1C-SD and MK1B-8MB), `POST /api/sim/flight` flies a profile (`src/flight_sim.h`):
  on the pad, a constant-acceleration boost sized for the apogee asked for,
  a ballistic coast, a drogue whose rate scales as sqrt(rho0 / rho), a main
  from its altitude, landing. Its pressure, from the 1976 US Standard
  Atmosphere to 32 km, replaces each reading after the pressure trace has
  recorded the sensor's, so the flight software, the flight log and the
  high-rate log fly it on the real board while the IMU logs the board as it
  sits. `GET /api/sim` follows it; `support/bench_flight.py` drives one.
- **Guarded:** it starts only from PAD_IDLE with test mode on (SIM-01,
  USB-08), and runs on the flight task through `flight_call()`. From its
  start until the board reboots, every fire is mocked and logged as one; a
  mocked fire reads energised for 500 ms, as MK1B's pulse does, so the
  machine records it as fired, and the channel reads open after it, as a lit
  charge does. The channels read good until fired, so the whole plan flies
  with nothing connected. A flight stopped part-way hands the machine the
  pad's pressure mid-descent, and a main set by altitude would fire into it,
  which is why a stop does not give the channels back.
- **Why not a replay:** the logger has to be judged over a flight the board
  has not flown, to the altitudes the user asked about ("the full high-rate
  logger works over high altitude flights"), and the chamber reaches none of
  them. A profile is short to describe and repeats exactly.

### DD-077: The High-Rate Log On The SD Card
- **Decision:** on MK1C-SD, an LSM6DS3 on the SD card's bus (±16 g,
  ±2000 dps, both at 1.66 kHz into its FIFO in continuous mode) and a
  high-rate log on the card (`src/sd/hr_log.h`): every IMU set, every
  pressure and temperature conversion as the pressure trace records it, and a
  snapshot of the flight ten times a second. Two tasks at P on core1: a
  reader every 10 ms drains the FIFO and the conversions into a 32 kB
  lock-free ring, and a writer empties it onto the card in 4 kB stages. The
  flight task is not on its path.
- **When:** while the flight log runs, launch to landing, and on the bench
  from `POST /api/hr/start`. Between flights the ring keeps its newest half,
  so a log opens with the second before launch.
- **Power cuts:** the next file is created and preallocated contiguously on
  the pad (`f_expand`), so launch waits on no FAT search; it is
  `logs/next.bin` until it closes and is renamed `logs/hrNNNN.bin`, and one
  a power cut left is renamed at the next boot. Each record carries a
  CRC-16 of its payload, and the log ends at the first that does not match.
- **Why a card:** 1.66 kHz of six 16-bit values is 20 kB/s before framing;
  a 30 km flight is ten minutes, 12 MB. The internal flash has neither the
  room nor the write rate, and its erases stop the other core (DD-074).
- **No interrupt pin:** none reaches the MCU, so the reader polls. The FIFO
  holds 682 sets, 410 ms at 1.66 kHz, so a reader late by less than that
  loses nothing, and the FIFO says when it did (FIFO_OVER_RUN). Reads take
  whole sets only, aligned by FIFO_PATTERN (AN4650, page 88).
- **A file the card cannot take any more** -- the card mounted again under
  it, or three writes in a row failing, whose error FatFs keeps for good --
  is given up: the log goes on in a new file from the next whole record, and
  the old one is renamed at the next prepare, which never creates over a log
  it could not rename. The expanded size is synced at prepare, so a log is
  recoverable whole from its first byte. The record a switch tears is the
  most lost (HR-06). Contiguous space is taken as the longest run the free
  space holds, halving from 128 MB.
- **On the bench, 2026-09-29:** 70 s at 1.66 kHz with a remount at 40 s:
  24 kB/s to the card, no record dropped, no FIFO overrun, the slowest 4 kB
  write 9 ms, the ring at most 17 kB of 32; the new file's first set 0.9 ms
  after the old one's last. A 30 km bench flight, launch to landing: 16.2 MB,
  1,088,033 sets over 668 s with none lost, 33,421 conversions with none
  missed, 6,684 snapshots.
- **Timing:** a batch is stamped at its FIFO status read, after the bus is
  had, so a stage holding the bus does not make batches look early.
  `support/hr_log.py` spaces sets at the sensor's measured rate (1627.8 Hz
  on this LSM6DS3, not 1660) and fits each unbroken run of batches to a line.
- **Decoded by** `support/hr_log.py`; host-tested by `test_hr_log.c`.

### DD-076: Every File Through vfs.h, The SD Card First
- **Decision:** every file the firmware opens goes through `src/vfs.h`,
  which routes a path to FatFs on the SD card while one is mounted, and to
  the board's flash store (DD-100) otherwise. The board's identity
  (`/serial.txt`) and the pad record stay on the flash store on every board.
  FatFs is R0.16 with its two patches (`lib/fatfs`), with exFAT, long names,
  `f_expand` and its reentrant lock, over this firmware's SD driver
  (DD-075).
- **Configuration:** `config.ini`, `pins.ini`, `beep.ini` and
  `lua_user.lua` are the card's, and each is copied into the flash store
  whenever the two differ (`vfs_mirror()`, at mount and after every write),
  so a board whose card is missing or unreadable boots with the
  configuration it last had. A blank card is seeded from the flash store at
  mount. A file the card lacks is read from the flash store, so a blank card
  still serves the web pages.
- **Why:** at the user's direction: "Use the SD for all files, but copy the
  SD card configuration files to the littlefs storage when they are
  different."
- **Every call may block** on the store's lock; the flight task reads the one
  file it needs through `hal_fs_read_cached()` (DD-074) and writes through
  rings the storage task empties.

### DD-075: An SD Card And An IMU On MK1C's J3
- **Decision:** a board variant, `boards/mk1c_sd`, gives J3 (GPIO18-21) to
  SPI0 -- SCK 18, MOSI 19, MISO 20, the card's select 21 -- and J1.6
  (GPIO22) to the LSM6DS3's select. Lua is off on it: J3 is Lua's on MK1C.
  The SD card runs in SPI mode with CRC on (CMD59) at 12.5 MHz, the LSM6DS3
  at 10 MHz, and each takes the bus for one transaction at a time, so a
  card's busy wait gives the bus back between polls (`src/sd/spi_bus.h`,
  `src/sd/sd_card.h`).
- **Waits:** every one bounded by the SD specification's limits -- reads
  100 ms, write busy 250 ms, 500 ms on SDXC (Physical Layer Simplified
  Specification 6.00, PDF page 97) -- and the card's initialisation by 1 s.
- **A command waits out the last write:** a card programs its last block
  after it is deselected, so the ready wait before every command allows the
  write-busy limit, 500 ms, not the read limit. With the read limit a
  4 kB-chunk bench counts timeouts and retries; with the write-busy limit,
  none in 16 MB.
- **MK1C's 3.3 V cannot carry a card** (task C-U6). On J3.1 the card resets
  28-29 ms into every initialisation -- a power-on reset, since only one
  returns a card in SPI mode to SD mode (SD simplified 6.00, section 7.2.1,
  PDF page 228) -- and moves the MS5607 on the same rail by 10 Pa. The same
  clocking with the card deselected moves nothing, and a battery on VIN
  changes nothing: the card's draw meets U6's foldback limit (XC6206, PDF
  pages 1 and 5). On its own MIC2920A-3.3 from VIN the card initialises at
  once (SDHC, 15.6 GB) and writes 734 kB/s in 4 kB chunks at 12.5 MHz,
  1020 kB/s at 20.8 MHz, with no CRC error; its programming pauses reach
  155 ms (measured 2026-09-29). A board carrying a card needs a larger U6 or
  a regulator for the card.

### DD-074: A Flash Operation Parks The Other Core From A Task Raised To T
- **Decision:** on the ground, any task but the flight task may write flash,
  one operation at a time, through `flash_op()` (`src/flash_op.h`): the
  caller raises itself to T, wakes the other core's lockout helper -- a task
  at T that exists from boot, pinned to that core -- which disables that
  core's interrupts and spins in RAM (`flash_op_park`), runs the operation
  with its own interrupts off, releases the helper and drops back to its
  priority. Every wait is bounded: a helper that does not park within 20 ms
  refuses the operation; a parked helper frees itself after 1.5 s. The
  flight task and interrupt handlers are refused. A board whose pyro
  sequence is paced by the flight task says no through `board_flash_ok()`
  while it runs (DD-056), and `flash_op()` waits for it, up to 500 ms. The
  flash store programs and erases through `flash_op_slot_program()` and
  `flash_op_slot_erase()`, one page program or one sector erase each
  (`src/fs_flash.c`). From launch to landing the log writer's operations run
  in its slots instead, with no lockout (DD-104).
- **Why the caller is raised, not one writer at T:** under mode 0 a task at T
  stops every task at P on both cores for as long as it runs. A single
  writer at T would run every file request -- an upload, an OTA image, a
  config save -- at T and stop the flight task for all of it; an SD card's
  busy time, hundreds of milliseconds, likewise. Raising a caller to T for
  one operation keeps the stop to the operation. A caller left at P with a
  helper ready at T would be preempted and never run again until the helper
  gave up.
- **Why not the SDK's `flash_safe_execute()`:** it creates a task on the
  other core for every operation, on one static stack and control block when
  dynamic allocation is off, and raises its priority after creating it. The
  helpers here exist once.
- **The pad record:** the flight task reads it during a start's resume
  decision. `hal_fs_read_cached()` serves it from RAM behind a seqlock,
  filled at boot and refreshed by every write, so the read never waits.

### DD-073: FreeRTOS SMP, Mode 0
- **Decision:** at the user's direction ("Do a full refactor to the
  freertos plan"), the firmware runs on FreeRTOS-Kernel V11.3.1 SMP across
  both cores, with `configRUN_MULTIPLE_PRIORITIES` 0 as the user directed on
  2026-09-28. The task layout (`src/rtos/rtos_tasks.h`):
  - two priorities, P and T = `configMAX_PRIORITIES - 1`, checked at build
    time (`rtos_tasks.c`);
  - the flight task alone at P on core0, woken every period (DD-104) by an
    alarm on the hardware timer, never by the tick; its only blocking call
    is that wait, and `prove_core0.py` refuses every other blocking kernel
    call on its path by name;
  - the net task (TinyUSB, smallest_tcp, HTTP, with the USB interrupt), the
    Lua task, the storage task and the timer daemon at P on core1, in
    core1's frame (DD-104); the tick interrupts core1 only;
  - a lockout helper at T on each core (DD-074).
- **Why mode 0:** while anything runs at T, nothing at P runs on either
  core. The flight task is the only task at P on core0, so nothing it shares
  a priority with can hold core0 when it wakes. It never waits on anything
  another task can hold: what it hands over goes through a ring or a
  notification, neither of which waits.
- **What the flight task hands over:** the flight log's records go into a
  single-producer ring the storage task writes out; the pad record is
  written by the storage task. Changes to state the flight task owns -- the
  running config, test mode, a buzzer audition -- reach it through
  `flight_call()`, a mailbox it empties at the head of its period. Every
  HTTP route and work unit runs in the net task. Lua's stop is
  `vTaskSuspend()`; a runaway script is preempted, and its tick bounded by
  Lua's instruction hook.
- **The watchdog:** fed by the flight task, and on the ground only while the
  storage task checks in, so a stuck storage task resets the board there. In
  flight a stall is counted and the flight goes on. `/api/status` reports
  how the last start ended: `prev_watchdog`, true for a watchdog timeout
  rather than a requested reboot, and `prev_stage`/`prev_stage_ms`, the
  stage or crumb the flight task was in, read before anything restamps
  them.
- **A new image commits** to `pico_fota_bootloader` from the storage task
  after five seconds of every task running, not at boot, so an image that
  dies once its tasks start rolls back by itself.

### DD-071: A Ground Test Procedure, By Switch
- **Decision:** at the user's direction, a ground test switch assigned to a
  pin. Powered up with it closed, the board enters ground test mode and
  announces it: three long beeps and a pause, repeating. Opening the switch
  starts the procedure [GND-TEST-05..11]:
  - a countdown at a count a second, five fast beeps down to none, and at
    zero pyro 1 fires;
  - a 3 s steady tone and a second countdown, and at its zero pyro 2 fires;
  - three long beeps, once, then silence.

  A channel not enabled is skipped: with one enabled, the tone and the second
  countdown go; with none, the countdown leads to the all-clear. The user's
  words: "When the pin is asserted on powerup, the unit enters ground test
  mode"; "step 5 becomes, 3 second tone, followed by the 5 count down. The
  count-down pace is 1 second per count."
- **The switch, two ways:** "a switch to ground" or "a switch that shorts two
  other pins", because "the mk1a ground pad got crowded". pins.ini says
  `ground_test=none|ground|pair`, `ground_test_pin=` and, across two pads,
  `ground_test_drive_pin=` [GND-TEST-12].
  - Across two pads the driven pad alternates high and low each step, and the
    read pad is pulled the opposite way each time. The switch reads closed
    only when the read pad follows both ways (`ground_test_switch.c`), so a
    read pad touching ground or the supply is never a closed switch.
  - The switch's pads are the flight software's: reserved against Lua and
    claimed at boot. A pair may include the buzzer's pad (DD-087); a switch
    to ground may not.
- **The rules:**
  - *Enabled* means a configured mode other than none, and pads not released
    to Lua. A fire is delivered on command: no health reading withholds it
    (GND-TEST-13, DD-081).
  - *At power-up* means held closed for the last 0.5 s of the 2.5 s settle.
    The mode is taken after the sensor and pyro health checks, and a board
    resuming a flight keeps flying whatever the switch says.
  - A release counts only after the switch has been held closed 1 s in the
    mode, so the operator has heard the mode first; a switch opened during
    start-up must be closed and opened again. A change counts once it has
    held 100 ms.
  - Closing the switch again during a countdown or the tone stops the
    procedure before the next fire and announces the mode again.
  - The fire comes at the zero count, five seconds into the countdown. Five
    fast beeps (60 ms on, 60 off) take 0.54 s, clear of the next count.
  - The mode is terminal until the next power-up: no launch detection, no
    flight log, no $PYRO sentence, no pad announcement, and the USB attach
    chirp does not take the buzzer. `!GT` lines report each step.
  - The schedule is the sequence's, not the buzzer's: a board with no buzzer
    (MK1A as shipped) fires on the same clock, silently.
- **Contracts:** `hal.h`'s `hal_ground_test_asserted()`, with a weak default
  in `flight_states.c` so every build links, and
  `hal_ground_test_configure()` for `main()`. GROUND_TEST is state 11, after
  FAULT, so no recorded state number moves.
- **Open:** the procedure on hardware, both wirings, dummy loads on both
  channels, and a scope or meter on the firing pads (task GT-1).

### DD-069: No Sensor Bus Transfer Waits Without Bound
- **Decision:** every I2C transfer to a pressure sensor gives up. Outside
  the collector -- detection and MK1B's bring-up -- the MS5607's transfers
  and MK1B's BMP280 reset give up within 2 ms, and the BMP280's commands and
  reads within twice their own time on the bus and a millisecond (5.5 ms
  for its 24-byte calibration at MK1B's 100 kHz). The collector's transfers
  are queued whole and judged at the next alarm, and one that does not
  finish is aborted (DD-093). `support/wait_check.py` refuses the SDK's
  `i2c_write_blocking` and `i2c_read_blocking`.
- **Why:** at the user's direction, "There must never be a lockup in
  flight." The SDK's blocking transfers pass no timeout
  (`hardware_i2c/i2c.c:246` in SDK 2.2.0), so a part holding SCL low would
  hold core0 until the watchdog reset it, and the bring-up, detection
  included, runs again after any restart, in flight too.
- **Tests:** a fake part that holds the bus, against a fake SDK whose
  blocking transfers count as an unbounded wait
  (`test_bringup_a_held_bus_is_bounded`,
  `test_bringup_mk1b_held_bmp280_is_bounded`). The fake bus times a
  transfer at its rate, so a bound too short for MK1B's 100 kHz fails.
- **The proof, too:** `support/prove_core0.py` counts the SDK's blocking
  I2C transfers as unbounded waits, and names as flight roots the task ticks
  `hal_tasks_tick()` reaches through a function pointer (`pres_tick`,
  `pres_bringup_tick`, `buzzer_tick`), which a call graph cannot follow. A
  root missing from the image fails it.

### DD-063: Every Pressure Conversion, Traced; 100 Hz Checked On Each Sensor
- **Decision:** at the user's direction -- "We need to verify that 100hz
  pressure data is valid. The sensor may operate at 100hz, but there could be
  duplicate data." -- the firmware keeps its last 256 conversions
  (`src/pressure_trace.h`): the time each was measured, when it was read,
  its raw and converted values and how it was classed, served at
  `/api/pressure/trace` [SNS-PRES-13]. `support/pressure_trace.py HOST` polls
  it and judges the sensor: rate and interval spread, stale reads, missed
  slots, lag and noise. A stale read is judged against what chance gives:
  noise repeats a code now and then, more often where the codes step by
  more than one -- 2 on the MS5607 at OSR 4096, 4 on the BMP280 at x4. CI
  runs the judge on made-up traces.
- **Why:** a duplicate reading cannot be told from a steady pressure in the
  samples; only each conversion, with when it was measured and when it was
  read, shows a stale read or a missed slot.

### DD-062: Binary On Disk, CSV On Download
- **Decision:** at the user's direction (T8) -- "on-disk format should be
  binary. We can translate to csv on-the-fly when downloading", and "Give
  them a maximum flight time estimator":
  - **`flight_log.bin`** holds binary records (`src/flight_log.h`): the magic,
    a header record, then 22-byte samples and text rows. `/api/flight.csv`
    renders them as CSV, with a `# Log rate:` line (DD-101). The response is
    framed by Content-Length: the server counts the rendered length first,
    4 kB of CSV a work unit, then streams the same rendering. A board
    holding a `flight_log.csv` serves it as it is; the erase removes both.
  - **`/api/log/space`** reports the room the next flight's log has -- what
    is free plus the current log, which the next launch replaces, on
    whichever store the log goes to, less 16 kB for the filesystem's own
    metadata -- and the seconds that holds (DD-101). The Config tab shows the
    longest flight that holds [WEB-UI-06, WEB-API-12].
- **Why binary:** a sample is 22 bytes against about 38 as CSV, and the CSV
  is only for a person or a tool reading it, so it is made when it is read.
- **A short write keeps only what was not written:** retried whole, the
  written part would appear twice, and one duplicated record misaligns every
  record after it.
- **The CSV's text is fixed,** held to it by `test_flight_log.c`: the
  temperature is kept in tenths of a degree, so a value exactly between two
  tenths can round differently from `%.1f`.

### DD-060: A Theory Of Operation Per Board; Structure In Place Of Comments
- **Decision:** at the user's direction -- "write a comprehensive theory of
  operation for each board type ... Reduce the comments by referencing the
  theory of operation ... The code functionality should be clear by the code
  structure and not rely upon comments" -- each board directory carries a
  `THEORY_OF_OPERATION.md`, and its code points into it with
  `See THEORY_OF_OPERATION.md "Heading"`. `support/trace_check.py` resolves
  such a pointer beside the file that makes it, and fails CI on a heading
  that is not there; it also checks that the functions a board's document
  names exist, and scans `.pio`, `.cmake` and `CMakeLists.txt` too.
- **What is the same on every board is written once:**
  - `src/hal_common/board_defaults.c`: the LED, the telemetry UART, the
    buzzer pad and `board_pyro_raw()`, as weak plain-GPIO defaults. The
    buzzer stays overridable, as `src/board_if.h` promises for a board with a
    piezo driver or a PWM slice behind it.
  - `src/pressure_single_sensor.c`: the bring-up for one sensor on one bus,
    which sensor chosen by the bus speed `board_pins.h` declares. MK1B, with
    two sensors on one SCL, keeps its own.
  - `src/bmp280_driver.c`, on the board's `BOARD_I2C_INST`.
  - `src/board_support.h`: the deadline test, the median ADC read and the
    safe-output helper.
- **`board_early_init()` is in each `pyro_board.c`.** Its one job is to put
  the pyro outputs down, and those are the pyro backend's pins;
  `hal_platform_init()` silences the buzzer before calling it. Every board
  drives its enables and common low there. `board_info.c` holds picotool's
  pin names, MK1B's calling GPIO15 the shared low side, as the netlist does.
- **MK1C's pyro backend is split** by what each part is: the measurements
  (`pyro_measure.c`), the latched faults (`pyro_faults.c`), the firing
  sequence (`pyro_sequence.c`), and the `src/pyro.h` glue (`pyro_board.c`).
- **Names say what the code does:** the continuity phases are named for what
  they measure (`CHECK_PRESENCE`, `CHECK_SHORTS`), the fire steps for the
  step (`STEP_PRECHARGE`, `STEP_HOLD`), and the thresholds for what they
  separate (`PATH_TO_GROUND_MAX_COUNTS`, `NO_PATH_MIN_COUNTS`).
- **The simulator boards** build too: `boards/sim_mk1b` with a
  `pin_store_owns()` of its own, `boards/sim_mk1c` with the arm pump.

### DD-059: MK1B Reads Continuity As MK1A Does
- **Decision:** MK1B's sense node is MK1A's (the netlist,
  `sim/plant/plant_mk1b.c`): 100k to 3V3, and PYRO_COMMON_EN the shared
  low-side gate. So MK1B reads continuity by MK1A's rule. Two readings a
  second: with the common off, after the idle second has recharged the node,
  a channel still low is shorted to ground; with it on, below 500 counts is
  an igniter (a 1k bad joint reads 41, a 10k leak 372), above 3000 open, and
  the raw count is reported. That is the presence-and-shorts check DD-055
  asks for.
- **Why:** with the common on, a fitted igniter pulls the node to 0 counts,
  so a rule that called a low reading with the common on a short would read
  every healthy igniter as shorted.
- **After a fire** the common stays on into the fresh reading, so there is
  no short reading then; the last one stands.
- `board_pyro_tests` models the node as the netlist has it: an igniter
  reads good, an empty connector open, a short to ground shorted, a 1k joint
  good with its count, and no read comes before its settle or recharge.
- **U5 decides whether a board can see any of it:** MK1B as built fits U5
  as AP2192AMPG-13, and the A variant discharges its outputs through about
  100 ohm while disabled (DS32193 p.4, RDIS; note 6). That holds each sense
  node near 0 V whenever its channel is off, which is always during a check;
  on the bench such a board reads both channels shorted (20 and 16 counts),
  and no threshold can see past it. `BOARD_PYRO_U5_DISCHARGES_OUTPUTS` 1
  says so, and the board reports each channel ready, unjudged
  (PYR-HEALTH-01). MK1B-8MB fits the base AP2192 and judges each channel
  (DD-097).

### DD-058: The API Is Live In Flight; Only The Log Touches The Filesystem
- **Decision:** at the user's direction -- "Leave the USB and API live
  during flight. This will simplify testing." then "flight mode will lock
  out the filesystem except for logging" and "file writes must return a
  suitable error code" -- the web API and USB stay live in flight, and the
  flight log holds the filesystem from launch until its tail is written
  [WEB-API-08]:
  - the HAL's file calls return `HAL_FS_LOCKED` (-3), `hal_fs_open()` NULL;
  - a web request that needs a file -- an upload, a file GET, config, pins,
    beeps, serial, a delete, the log's erase -- is answered 423 Locked at
    its head, before its body is read [WEB-API-10];
  - a transfer that took the filesystem before launch is dropped on its next
    pass, and the log waits for it before it opens (`hal_fs_enter()` counts
    every file use but the log's).
- **Why the lock:** from launch to landing the only flash operations are the
  log writer's, in its slots (DD-104), and the erased pages are the log's to
  append to without an erase (DD-100). Another file would take pages and
  operations the log needs.
- **Why 423:** the file is locked, not the request malformed or the board in
  the wrong state.
- **Live in flight:** status and every other GET, the beep audition and the
  Lua check. A reboot or BOOTSEL asked for in flight waits for landing, and
  an update cannot write its image until then (DD-104).
- **Resting on:** the API is reached only over USB, and a flying rocket has
  no PC on its cable. A board that carries the API over a radio needs an
  in-flight lock on the rest of it.
- **The spent pad record** waits for the log to let go of the filesystem;
  refused, it would never be cleared.

### DD-056: MK1C Fires: DESIGN.md 7.1 As Loop Steps
- **Decision:** at the user's direction ("Finish F1"), MK1C's `pyro_fire()`
  runs DESIGN.md 7.1 and IGNITER_OPERATION.md F0-F10, one step per period,
  with nothing waiting (DD-053; `boards/mk1c/pyro_sequence.c`). It starts
  whatever the board has measured: an open channel, a latched fault, a low
  pack and a missing presence test are reported and withhold nothing
  (PYR-FIRE-01, PYR-HEALTH-01). It waits only for a sequence already
  running: the two channels share the bus (PYR-DEPLOY-02).
  - **F0-F1, arm:** the bias drops; the pump starts and is fed.
  - **F2, precharge:** each period feeds the pump; U9 ramps the bus at
    0.89 V/ms.
  - **F3 with F6, fire:** FIRE_x goes on and the pump stops in the same
    step, when SNS_BUS reaches 90 % of SNS_VBAT, or at the precharge
    deadline on whatever the bus has: 1.5 times the pump's 0.4 ms and the
    ramp, about 14 ms on 2S. A pulse gated at the deadline latches a
    precharge timeout for that pulse; the pulse is never abandoned
    (PYR-ARM-03).
  - **F7-F8, the hold:** the gate is released once U9's enable has had
    10 ms to collapse and the bus is flat (25 counts between periods), or at
    30 ms.
  - **F9, the drain:** the presence test waits for a cold bus. The second
    channel may fire meanwhile, on the bus the first left charged; the bus's
    1.1 uF bleeds on a 2 ms constant.
  - **F10, verify:** the first presence test after the drain reports the
    fired channel open (fired) or still present (a misfire, live). Nothing
    latches: a misfire never inhibits the other channel (invariant 12,
    PYR-ARM-06).
- **The pulse is U9's current limit.** No bulk capacitor is fitted (the
  user, 2026-09-26), so with the gate on U9 drives about 4 A into a 1 ohm
  bridgewire for the 9.6 ms its enable takes to collapse after the pump
  stops, far more than M5's 15 mJ. A misfire holds U9 in its limit for the
  same 9.6 ms and no longer. The enable falling after every fire releases
  U9's own latch (TPS2595 datasheet page 23), so each pulse starts with the
  part re-enabled (PYR-FAULT-01).
- **The pump is fed once a period.** The FIFO's four words and the one
  running carry it a period and a quarter (`boards/mk1c/arm_pump.h` derives
  the burst from the period), and that is all it runs past the last feed: a
  flight task that stops leaves the bus disarmed within that and U9's
  9.6 ms. The PIO program and its state machine are claimed at boot, not at
  a fire.
- **One watchdog:** the flight task's. The pump does not need one to disarm:
  it stalls on its own when the feed stops.
- **No flash while a fire is in its sequence.** An erase on the ground stops
  the flight task for tens of milliseconds, longer than the pump coasts, and
  a stalled precharge reaches its deadline uncharged. So `board_flash_ok()`
  answers false from the command until the gate is released, and while a
  presence pulse is out, and `flash_op()` waits for it (DD-074). MK1A and
  MK1B keep the weak default: their pulse is a timed level that a stall only
  lengthens.
- **One presence test is one sample** (invariant 8): the bus-short latch
  counts tests, not the periods that read a test, and a bus too low to judge
  presence by, under `TRACK_BUS_MIN_COUNTS`, is the short.
- **Not built:** ILM and FLT are not routed to the MCU, so the capacitance
  check of 7.2 and the FLT abort are absent; the precharge deadline covers a
  loaded bus. No decay profile is classified (DD-055: no live checks); F10's
  presence test is the verdict.
- `board_pyro_mk1c_tests` runs it against the plant of the board as built,
  on 2S and 1S: a fire, the measured trigger, the pump's bounds, a stopped
  feed, a precharge gated at its deadline, the verification, a misfire, both
  channels in turn, a high side stuck on, and the flash hold.
- **Open:** a fire with the bus shorted, on the bench (task R-7).

### DD-055: MK1C's Firmware Checks Presence And Shorts, Nothing Else
- **Decision:** at the user's direction -- "The ONLY FW checks we need are
  those that verify the pyro is present and we do not have short circuits.
  We do not need live HW checks." -- MK1C's pyro backend runs the presence
  test and the two short latches, and no other probe:
  - presence: a channel following the biased bus (DD-054);
  - a bus that will not rise under its bias: shorted to ground, by the bus,
    a harness lead, or a shorted low-side FET behind a fitted match;
  - the bus at the pack with nothing armed: a shorted high side.

  What it finds is reported and gates nothing (DD-081).
- **No other probe:** no channel-bias probe, no bus decay probe, no waveform
  capture. A shorted low-side FET without a match is not reported: it
  cannot fire anything, and with a match fitted the presence test sees the
  bus pulled down.
- **The arm pump:** the firing bus is energised only while ARM_TOGGLE pumps
  U9's enable (`boards/mk1c/arm_pump.c`), so firing needs it (DD-056).
- `board_pyro_mk1c_tests` runs the real backend against the plant: a fitted
  match reads present and an absent one open, a shorted bus, a shorted low
  side behind a match and a shorted high side each latch, and the only
  stimulus is the presence pulse, once in 500 ms, with nothing holding the
  flight task.

### DD-054: MK1C's Firing Bus Is Designed Around The Board As Measured
- **Decision:** at the user's direction, the firmware and its model follow
  the bench MK1C, not DESIGN.md 4's algebra. Scoped at CN1 and read by the
  board's ADC on 2026-09-26: R120 330 ohm (metered), R103 and C115 as
  designed, but U9's OUT, off, conducts back into the part above about
  0.72 V, a junction with about 250 ohm behind it, 2.4 mA at 1.5 V. The
  user: "an internal reverse bias is to be expected". So under bias the bus
  sits at about 688 counts (1.66 V), not 1058, drawing about 4 mA from
  GPIO23 at its 4 mA default drive; the channels under their own bias read
  about 1262, not 1214, because their bias diodes drop less at 0.2 mA.
- **Presence is a ratio** (`boards/mk1c/pyro_sense.h`): a channel is
  present when it reads at least half the bus in the same presence test,
  and the test says nothing when the bus did not rise (under 200 counts).
  DESIGN.md S3 says presence "does not use absolute levels"; the ratio keeps
  2x of margin whatever the bus does, where an absolute level would move
  with U9 and its temperature. `pyro_get()` uses it.
- **The model is the board** (`sim/plant/plant_mk1c.c`): U9's reverse path
  and the GPIO-and-Schottky bias sources, fitted to the bench, and the
  bus's 1.1 uF. It reproduces the ADC's 685-690 on the bus, 1258-1266 on
  the channels, and the scope's fall: 415 us to 0.9 V, 2.32 ms from 0.6 to
  0.2 V. `plant_tests` holds it there.
- **What it costs:** an open R_BLEED does not show in the bus level -- U9's
  path carries the bus either way, 685 against 730 counts. Below the knee
  the bus decays through the pull-down alone, so the decay there would show
  it (about 2.3 ms against 16 ms); nothing checks that (DD-055). The
  tracking current is about 0.11 mA.

### DD-053: No Sleeps: The Exec Loop Is The Only Clock
- **Decision:** at the user's direction, no code sleeps or busy-waits. The
  period sets the time; anything that has to wait parks on a deadline that
  a later step checks. `support/wait_check.py` fails CI on any sleep,
  busy-wait, or blocking DMA or PIO wait in `src/` or `boards/`; its ratchet
  of sites not yet converted is empty.
- **Why:** a sleep inside a step is time the flight task does not have. A
  `sleep_ms(10)` settle once a second in a continuity check overruns the
  period once a second.
- **MK1B's continuity** works like MK1A's: the settle is a deadline, the
  reading comes on a later `pyro_update()`, once a second, and a fresh one
  lands a settle after a pulse. The first `pyro_update()` starts the cycle,
  not `pyro_init()`, because the release claim comes after init and a board
  with both channels released never calls `pyro_update()`: its common, then
  Lua's pad, is never left raised. `board_pyro_tests` runs the board file on
  the host against `test/fake_sdk`, whose sleeps fail the test.
- **The sensor's bring-up** is steps too: bus recovery at one SCL edge a
  step (I2C has no lowest clock rate), with a STOP on every SDA the board
  has; the pull-ups' settle; the MS5607's 2.8 ms PROM reload and the
  BMP280's 2 ms start-up, each a deadline. It takes about a quarter of a
  second, inside BOOT_SETTLE. `hal_pressure_init()` starts it on the
  pressure task's slot; BOOT_SENSOR waits for it (`hal_pressure_sensor()` is
  -1 until then), and one still going `SENSOR_BRINGUP_MS` after boot is a
  missing sensor. `sensor_bringup_tests` runs each board's own
  `pressure_board.c` against fake sensors that refuse a transfer during
  their reset.
- **Outside the check:** waits on a bus or a peripheral's handshake, which
  wait for hardware rather than for time, each bounded (DD-069). In the SDK,
  `uart_init()` can busy-wait only when re-initialising an enabled UART,
  which ours never is, and `stdio_get_until()` is reachable only from
  newlib's `_read()`, which nothing calls.

### DD-052: Each Board Runs Its Sensor Bus As Fast As Its Device And Its PCB Allow
- **Decision:** at the user's direction, the I2C speeds are part of each
  board support package: `BOARD_MS5607_I2C_HZ` and `BOARD_BMP280_I2C_HZ` in
  `boards/<name>/board_pins.h`. Each is the device's fastest, or slower where
  that board's PCB cannot carry it; a reliable connection comes first. The
  device limits stay with the drivers (`MS5607_I2C_MAX_HZ`,
  `BMP280_I2C_MAX_HZ`), and `pressure_board.c` fails the build if a board
  asks for more.
- **The devices** (docs/datasheets/): the MS5607's I2C clock is 400 kHz at
  most (page 5). The BMP280 lists standard, fast and high-speed modes (pages
  27 and 31), not fast-mode plus; high-speed runs to 3.4 MHz, but the RP2040
  has no high-speed mode (section 4.3.2). Both are 400 kHz, fast mode.
- **The boards,** read from their design files: fast mode allows a 300 ns
  rise (UM10204 page 44), which 4k7 holds to about 75 pF of bus (page 50),
  several times what one sensor and a short trace present.
  - MK1A: the BMP280 on GPIO20/21 with R1 and R2, 4k7 (its schematic PDF):
    400 kHz.
  - MK1B: the MS5607 on GPIO10 with GPIO7's SCL, R11 and R10, 4k7 (its KiCad
    board): 400 kHz. The BMP280's SDA, GPIO6, has no pull-up but the
    RP2040's own 50-80k, too slow an edge for fast mode: 100 kHz. The
    BMP280 probe runs first, at 100 kHz, and the rate goes up before the
    MS5607's probe.
  - MK1C: the MS5607 on GPIO6/7 with R3 and R5, 4k7 (its KiCad board):
    400 kHz.
  - The reference template: 100 kHz until a new board's pull-ups are read.
- **Why faster:** a faster bus shortens every cycle's transfers. An MS5607
  cycle is 18.8 ms at 400 kHz; a BMP280's 13.7 ms at 400 kHz and 14.5 ms at
  100 kHz (DD-093).

### DD-050: A Failed Sensor Never Deploys Anything
- **Decision:** a whole second of one reading, to the pascal, is a stuck
  sensor (SNS-PRES-10), and a gap of more than 250 ms between samples
  suspends decisions until a whole second of new samples exists
  (SNS-PRES-11). Either makes the samples suspect, the stuck sensor for as
  long as it lasts and a second after. A suspect sample decides nothing: it
  arms nothing (DD-017), moves no peak, feeds no apogee detector, is no
  known state to the fire rules, and lands nothing (DD-015). No sample for
  0.5 s in flight is a lost sensor. Each sets a DIAG bit, which
  `/api/status` lists by name, and logs SENSOR_STUCK or SENSOR_LOST, with a
  telemetry line. The flight carries on once the sensor answers again. On
  the pad either is a general fault (SNS-PRES-17, DD-094).
- **Why:** a failed sensor must never cause a deployment (DD-081: only the
  absence of samples suspends a decision). A stuck value reads as a rocket
  that has stopped.
- **250 ms** is longer than an erase's stop on the pad (50 to 70 ms, DD-107)
  and a sample together, so a stall is never a gap. A real sensor with the
  MS5607's noise never reads as stuck in an hour on the pad.

### DD-047: The Flight Log Carries Each Sample's Reading
- **Decision:** each flight log sample carries, before `event`, `raw_pa`,
  the reading the sample is the filtered state of, and `temp_c`, the
  sensor's temperature (DAT-02). Text rows carry those fields empty.
  `sim/replay.c` feeds a log's readings back through the estimators and
  detectors and sets the decisions against the log's own event rows, as
  `pyro_sim --replay <log>` (DAT-08).
- **Why:** a change to the estimators or the detectors can be checked
  against a real flight only from its readings; a filtered pressure cannot
  be filtered again. Every sample is a row (DD-101), so a log replays whole.
- **Before `event`**, not after: a text row's last field is free text, and
  the web UI finds columns by name.
- **The HALs fill the columns themselves** from `pp_last_read_raw_pa()` and
  their own temperature, so `hal_log_sample()` keeps its signature.

### DD-046: Every Sample Carries The Time Of Its Reading
- **Decision:** each reading is stamped from the hardware timer, to the
  microsecond, at the moment it describes (SNS-PRES-08): the collector
  stamps each conversion at its middle (DD-093). The pressure layer takes
  the stamp (`pp_feed_us()`) and carries the microseconds to its samples
  beside the milliseconds. Every detector hold and dwell that measures the
  sensor, and every logged sample row, uses the sample's time (FLT-RATE-05,
  DAT-02).
- **Why:** a reading stamped when a task read it carries that task's
  lateness, and every dt with it; a hold on a task's clock lets lateness
  choose which sample decides. A read held off keeps its stamp
  (`test_SNS_PRES_08_a_held_read_keeps_its_stamp`).
- **For the bench:** `/api/status` shows `sample_interval_us` and
  `stamp_lag_max_us`.
- **A hold's start is stored as its sample time plus one** (`held_for()` in
  `src/hold.h`), so 0 can still mean "not started" at any sample time.

### DD-045: The Ground Reference Re-Seeds After A Step
- **Decision:** when every sample has been more than GND-CAL-03's 50 Pa from
  the reference for 5 s, and the board is still (under 1 m/s), the reference
  restarts from the current pressure (GND-CAL-06). `!GND reseed` goes out on
  telemetry, `ground_reseeds` counts it on `/api/status`, and the pad record
  is written afresh with the new ground.
- **Why:** a board powered at the prep table and carried to a higher or
  lower pad sees every later sample rejected by the gate, and its reference
  would freeze at the old ground for good.
- **5 s:** for as long as the wait lasts after the rocket is set down, the
  reference, and every altitude, is wrong. No gust lasts 5 s, and drift
  stays inside the gate.
- **Still, because** a board being carried is still moving from one ground to
  the next; a re-seed mid-walk would only need another.

### DD-043: The Launch's Ground Pressure Comes From Before The Rise
- **Decision:** at launch the reference freezes to the mean of the 250 ms
  blocks of GND-CAL-01's 5 s mean that ended before T+0, the first sample of
  the rise (GND-CAL-04). Each block records when it began. With less than a
  second of such blocks the reference is flagged degraded (GND-CAL-07), and
  with none it is kept as it is.
- **Why:** frozen at detection, 1-2 s into the climb, the reference would
  hold the first metres of that climb, which pass the 50 Pa gate, and every
  AGL value would read low: 0.17 m at 30 g to 0.42 m at 2 g.
- **T+0 is already known.** FLT-LAUNCH-03 records the first sample above
  50 cm, so no fixed look-back is needed, and a fast launch does not lose
  seconds of pad.
- **Degraded, not refused:** a rocket launched the moment the board reached
  PAD_IDLE still flies; its AGL values are only as good as the calibration.

### DD-039: HTTP Is A Byte Stream, Between Two Rings
- **Decision:** each connection owns an rx ring and a tx ring (`net_ring.c`,
  2 kB each) and an HTTP engine (`http_conn.c`). The engine parses the
  request from rx a line at a time, takes the body at the pace its consumer
  allows, and writes the response into tx [WEB-HTTP-01..03]. The transport
  (`src/http_transport.c`, DD-102) moves bytes between the stack and the
  rings and runs no handler; the work runs in the net task.
- **Why:** TCP divides a request however it likes, and the answer must be
  the same whichever way: a body in a later segment, a header block split
  across segments, a request whose first segment is a single byte, an
  upload sent in small writes. A body taken only as fast as it is consumed
  lets TCP's window, not a refused segment, hold back a sender.
  `support/http_stream_check.py` sends requests in each of these shapes to a
  board.
- **What follows from it:**
  - Every response carries Content-Length (WEB-HTTP-02), files included.
  - The OTA reply is sent before the reboot: the reboot runs through
    `pending_reset` once the reply is on its way.
  - OTA answers `Expect: 100-continue`, so curl does not wait a second
    before sending.
  - An upload that dies mid-body is never closed, so the store keeps the
    previous file whole (DD-100).
- **Why rings:** smallest_tcp (DD-102) gives each connection
  application-owned RX and TX buffers behind a vtable. It advertises the RX
  buffer's free space as the window, and the application drains RX and
  fills TX. `net_ring.h` follows those operations, and the transport writes
  the stack's buffer operations over each exchange's own rings.
- **The cost:** 4 exchanges of 2 kB rx, 2 kB tx and a 5 kB work buffer. The
  work buffer is shared, one use at a time, by the gathered body, the part
  of a response that does not fit tx, and a streamed file's buffer. Sixteen
  links share the four exchanges: a connected link is given an exchange by
  the server, and until then its window stays shut, which holds the sender.
