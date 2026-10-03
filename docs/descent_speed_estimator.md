# The speed the fire rules compare: options for the estimator

Options for the filtered descent speed that the re-fire thresholds and the
emergency all-fire speed compare, and for a model-based filter that carries
the flight through the Mach lock. Written 2026-10-02 during the requirements
review. Nothing here is started. The estimator is flight-critical and the
choice is the user's.

## What exists

One estimator (DD-048, `src/pressure_fit.c`): a least-squares quadratic
through the last second of the median's output, evaluated at the newest
sample. It gives p, ṗ, p̈ and a clean verdict from its residuals. Every
detector's speed is ṗ through the altitude formula's slope (FLT-ASC-02).
Triggers hold for a duration (DD-042). Descent rates are judged in the pad's
air (DD-079, `pp_air_scale()`).

The Mach lock (DD-049, `docs/mach_lockout.md`) does not filter through the
corrupted span. It refuses the data while the flag stands and asks the data
to prove itself before release. It has no model of the flight.

Open against it: HA-1. Above about 15 km the release cannot see 0.58 g
through the fit's p̈ noise (`docs/high_altitude_flight.md`, options A to F).

## What the new fire rules ask of the speed

Three configured speeds now decide fires: `pyro1_refire_speed`,
`pyro2_refire_speed`, `emergency_fire_speed`. Each is a comparison against a
descent speed, repeated every second, for the rest of the flight.

The fit is already a filter, so "filtered" is met in name. Its noise is the
question. For a quadratic over N even samples spanning T, the endpoint rate
has variance 192 σ²/(N T²): 1.96 σ per second at N = 50, T = 1 s. A pascal
per second is 1/(ρg) metres per second, which grows as the air thins:

| Height | ρg, Pa/m | Speed noise at 1.2 Pa | at 3 Pa |
|---|---|---|---|
| Pad, sea level | 12.0 | 0.2 m/s | 0.5 m/s |
| 9 km | 4.6 | 0.5 | 1.3 |
| 15 km | 1.9 | 1.2 | 3.1 |
| 20 km | 0.87 | 2.7 | 6.8 |
| 30 km | 0.18 | 13 | 33 |

These are one σ of true speed on a single fit. DD-079's scale brings 30 km
down by 0.22, to about 7 m/s in pad air at 3 Pa. Below 9 km the fit is
steady enough for any sensible threshold. Above 15 km a threshold compared
against single fits will be crossed by noise, and an emergency all-fire on
noise is a fire nobody chose. So the requirement on the speed should be
stated as a noise bound, whatever estimator meets it.

## What a model can and cannot know

A Kalman filter is a model's prediction corrected by measurements, weighted
by how much each is trusted. Lowering the trust in pressure while the Mach
flag stands, and restoring it on release, is exactly a measurement variance
R scheduled by the flag. The mechanics fit the idea well. The model is the
hard part, for the reason DD-022 and DD-049 already give: the operator
enters nothing about the rocket.

- **Under thrust** the acceleration is the motor's. No model without the
  motor predicts burnout.
- **In supersonic coast** the deceleration is gravity plus drag. Drag is
  unknown, largest exactly there, and changes sharply through Mach 1. It
  cannot be learned beforehand, because the fastest point of the flight
  comes first.
- **In subsonic coast** gravity dominates and drag fades toward apogee. A
  gravity-plus-fitted-drag model is good here, where the pressure is good
  too.
- **In descent** the model is a terminal speed set by an unknown canopy.
  A deployment is a step in it. Pressure is clean and subsonic.

So with pressure alone, the span where the model would be preferred is the
span where no model is available. A filter coasting on its last acceleration
is right for about a second and wrong by burnout. Its covariance says so
honestly, which is useful, but its state is not a flight to prefer.

An accelerometer changes this. Acceleration measured on the board does not
pass through the static ports. Integrated, it is a speed and a height
through thrust and through Mach, and pressure corrects its drift outside
the lock. That is a model worth preferring. One board has the part
(`src/sd/lsm6ds3.c`).

## Options

### A. Keep the fit, lengthen its memory where the air is thin

The speed for the fire rules comes from a longer window as the pressure
falls, the same remedy as HA-1's option A. Rate noise goes as T^-1.5, so 4 s
has an eighth of 1 s's. Under a canopy the rate is steady and a 4 s lag
costs nothing; right after a fire it delays the verdict by the window.
One estimator, known mathematics, known cost (the fit already takes 2.9 ms
against a 1.6 ms limit, T5-C, and a 200-sample fit is four times that
unless it becomes incremental).

### B. A three-state filter in the pressure domain

State p, ṗ, p̈ with a white-jerk process and R = σ², run on the median's
output. In the pressure domain the measurement noise is the same at every
height, so one tuning serves the whole flight. Constant work per sample,
no window to store. Its covariance is a running statement of how well the
speed is known, which is the natural gate for Rule A: act only when the
speed's σ is under a stated fraction of the threshold.

- R is scheduled: σ² normally, effectively infinite while the Mach flag
  stands or a fit is suspect (SNS-PRES-10, -11), so corrupt pressure cannot
  drag the state.
- Process noise is scheduled by phase: wide in ascent, narrow in a steady
  descent, opened for a few seconds after each first fire so a deployment
  is followed rather than smoothed over.
- Through the lock it coasts and its covariance grows. It does not replace
  the lock's release test, and it does not fix what happens inside the lock.

What it buys over A: constant cost, a believability number, and noise that
adapts by phase. What it costs: a second estimator beside the fit, or the
work of moving every detector onto it, and DD-048's "one estimator" goes.

### C. B with a descent model

In descent the state carries a drag term k with a = g − k v², so the filter
estimates the terminal speed the present canopy will reach, seconds before
the rocket reaches it. This answers "did the fire slow the descent" without
waiting, and removes FLT-EMRG-03's concern that a good drogue is still
accelerating. An extended filter, harder to prove, and the fire rules as now
written compare the present speed, not a predicted one.

### D. Accelerometer and pressure fused, on boards that have both

Vertical acceleration drives the prediction; pressure corrects it outside
the Mach lock. This is the filter that "prefers the model" through Mach, and
it would close HA-1 as well. To settle before it could be trusted:

- the LSM6DS3 saturates at 16 g, and a hard motor exceeds that;
- the axis is the rocket's, so the gyroscope must hold the tilt, and under
  a canopy the board swings and only pressure is usable;
- today the accelerometer feeds the high-rate log, off the flight task
  (HR-05, RTOS-01). Bringing it onto the flight path is a new contract;
- boards without the part still need A or B.

### E. Leave the estimator, state the rule

No new filter. The fire rules require the speed above the threshold on every
clean fit for a hold, say 1 s, and the documentation says the thresholds are
not dependable above about 15 km. Cheapest. It leaves the noise table as it
is and HA-1 where it is.

## Rule A

Whatever is chosen, a prediction is not knowledge. A fire is decided on a
state the measurements support: for B, C and D that means the covariance is
under its bound, not merely that the filter has a number. A filter coasting
through the lock on a kinematic model decides nothing. Whether D's
accelerometer-carried state counts as knowledge for apogee is a separate
decision, and until it is made the lock's present rules stand.

## Scope, set 2026-10-02

Three assumptions bound the filter's job:

1. **The lock sets early, under boost, after a clean launch.** Launch is
   declared and T+0 and the ground reference are taken from good data before
   the flag goes up (FLT-LAUNCH-07, GND-CAL-04, FLT-MACH-02).
2. **The lock releases before apogee**, leaving time for the pressure data
   to be stabilised.
3. **A model is useless until the release.** From the release it stabilises
   the trajectory: it tracks the deceleration toward apogee, and afterwards
   the descent.

So the filter runs from release onward. Options B and C are in scope, in
that span only. D is out of scope here. Nothing filters through the lock.

Assumption 1 holds for practical motors: Mach 0.62 inside the first 30 m
needs about 100 g (the flag is 240 to 270 m/s in pad air). The study confirms the margin. Assumption 2 is the one
that fails with height. Inside the lock's 9 km envelope the release comes
8.5 to 16 s before apogee (`docs/mach_lockout.md`). Above about 15 km at
3 Pa of noise the release cannot see its deceleration test, the lock holds,
and apogee comes from the fallback (HA-1). Where assumption 2 stops holding
is the maximum safe altitude.

## Direction set 2026-10-02: a Kalman filter that tracks the noise

The user's direction for the design: a Kalman filter tracks the extra noise
at altitude and runs through it. Two properties of the filter do this, and
neither needs a schedule keyed to height.

- **The gain falls as the air thins, by itself.** With the state in height
  and speed and the measurement in pressure, a metre of height is ρg
  pascals of measurement: 12 Pa at the pad, 0.18 Pa at 30 km. The same
  sensor noise in pascals is therefore worth less per sample high up, and
  the filter leans on its model for longer. That is the lengthened
  averaging HA-1's option A asks for, arrived at continuously.
- **The measurement variance is estimated, not assumed.** The spread of the
  innovations, reading minus prediction, is the noise the filter is
  actually seeing. Tracking it lets the filter widen its trust interval
  when the sensor is noisier than it was on the pad, near its limits or in
  rough air, and narrow it again. The pad's σ is the starting value and a
  floor.

The filter is not loosened at a fire (ruled 2026-10-02). Smoothing a
deployment's deceleration does not matter: the fire rules ask whether the
rocket is too fast, and that condition takes seconds to develop. A lagging
speed after a good deployment costs a few re-fires into a spent igniter,
which is harmless. What the filter must not do is smooth out an overspeed.
Its lag in reporting one is the figure to measure; it can only make a
re-fire or an emergency fire late.

A smooth bias, as from a supersonic port, is not noise; the Mach lock stays
the guard for that span.

**The release.** "Useless until the lock releases" is kept as "decides
nothing until the lock releases". The release test fails at altitude today
because it reads a one-second fit. If the filter runs on the data while the
flag stands, its state is worthless in the supersonic span and converges
once the rocket is subsonic, at a rate the thin air sets. The release's four
conditions can then be read from the filter's state and its innovations:
consistent, climbing, slow, and slowing by gravity. The study measures the
time from subsonic to release against height and noise.

## The filter after release

- **Model.** A subsonic coast: a = -(g + k v^2), with k carried as a state
  and learned from the coast itself. Drag fades toward apogee, so an error
  in k matters least where the decision is made. In descent, the same form
  with the sign of drag reversed and k reopened at each first fire.
- **Start.** At release the state is seeded from the fits that earned the
  release, with a wide covariance. The time from release to a covariance
  under its bound is the stabilisation time. It must be shorter than the
  time from release to apogee, at every height the board claims.
- **Raw readings, innovation limited.** No filter stands in front. A host
  evaluation (`docs/kalman_launch_evaluation.md`) found a single reading 3
  to 6 kPa low declares a launch through a raw filter at pad-quiet tunings,
  the defect DD-040 fixed, and that holding each innovation to a few of its
  standard deviations inside the update stops one or two bad readings of
  any size at no cost in delay. A run of readings is data and moves it.
- **The data wins, without a re-seed.** The board cannot know that its
  sensor is wrong. A Kalman filter trends toward data that is stable, so
  after a discontinuity it follows the readings by its own gain. Nothing
  re-seeds it and nothing is discarded. There is no state in which samples are arriving and
  the flight refuses to decide.
- **Offset is not noise.** The noise tracking must not read a sustained
  offset as extra noise, or it would lower the gain exactly when the filter
  should be moving. It measures the scatter of the innovations about their
  running mean, not their size.
- **Fires.** Nothing special happens at a fire. The filter trends to the
  new descent as the readings settle on it.

## The modelling study

Purpose, in order:

1. **Which values are configuration.** Decide what the operator must set
   and what is a constant of the design. Candidates for configuration: the
   two re-fire speeds, the emergency all-fire speed, `refire_interval`,
   `fire_gap`. Candidates for constants: the gate width, the covariance
   bounds, the process noise schedule.
2. **Defaults.** A value for each configured field that is safe across the
   study's rockets, or the finding that none exists and the default is zero.
3. **The maximum safe flight altitude**, per sensor noise: the greatest
   apogee at which the release precedes apogee by more than the
   stabilisation time, apogee is declared by the detector and not the
   fallback, and no fire rule acts on noise.

The study also covers `docs/pressure_domain_flight_math.md`: flight math in
pressure, with altitude for the operator only.

The plant is `sim/mach_plant.c` extended above 9 km with the 1976 standard
atmosphere, as the bench flight already uses (DD-078).

| Swept | Range |
|---|---|
| Apogee | 300 m to 35 km |
| Sensor noise σ | 1.2, 3 and 5 Pa |
| Coast drag, as terminal speed | 20 to 300 m/s |
| Peak Mach | 0.3 to 3 |
| Pad | sea level to 2000 m, 10 to 45 °C |
| First canopy's rate in pad air | 10 to 40 m/s, and failed |
| Second canopy's rate | 3 to 8 m/s, and failed |
| Seeds per point | 1000 |

The central measurement, ruled 2026-10-02: filtered pressure must show the
slowing climb, apogee and descent at every height in the sensor's range. A
fixed one-second window does not above about 15 km; a fallback that waits
for the flag's pressure is rejected. The study finds the filtering that
declares apogee within a stated lateness at each height and noise.

Measured at each point:

- launch declared before the flag sets, and by how long;
- release to apogee, and release to a stabilised state;
- apogee: detector or fallback, and its lateness in seconds and metres;
- speed σ of the filter and of the fit, through coast and descent;
- readings the gate discarded, and track losses;
- each fire rule: fires on a failed canopy, and fires on a good one.

Outputs: a table of defaults, a maximum altitude per σ, and the list of
constants with the margin each was chosen at. The filter is built only as
far as the study needs, on the host, and decides nothing on a board until
the study has been read.
