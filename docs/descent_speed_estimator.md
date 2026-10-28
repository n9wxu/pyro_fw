# The speed the fire rules compare: options for the estimator

The descent speed that the re-fire thresholds and the emergency fire speed
compare, and the estimator behind it, with the options for it. The
decisions are DD-085, one Kalman
filter on the raw readings, which is `constacc`, and DD-092, estimators
behind one interface with `constacc` the default (DD-113). This document is
`constacc`'s design (`src/altitude_filter.c`, `src/estimator_constacc.c`).

## What exists

Two estimators behind `src/estimator.h`, both fed every reading; the flight
obeys the one `estimator` in config.ini names (SNS-EST-06, SNS-EST-07):

- `constacc`, the default: a Kalman filter on the readings' altitude, its
  speed and its acceleration (this document).
- `lumped`: one equation of motion from the pad to the ground, learning a
  lumped thrust and drag (`docs/lumped_parameter_filter.md`).

The fire rules' descent speed is the obeyed estimator's speed in the pad's
air (DD-114), scaled by sqrt(rho / rho_pad) (FLT-AIR-01,
`fire_control_descent_speed_ms()`). For 2 s after a pulse the
state it is read from is bounded to free fall from the state before
(PYR-MODE-06). The rules compare it at every step after apogee
(PYR-REFIRE-01, FLT-EMRG-01), on the filtered state whenever the sensor gives
data, with no further judgement (FLT-EMRG-05).

## What the fire rules ask of the speed

Three configured speeds decide fires: `pyro1_refire_speed`,
`pyro2_refire_speed`, `emergency_fire_speed`. Each defaults to 0, which turns
its rule off (DD-094). The requirement on the speed is a bound, whatever
estimator meets it: noise alone crosses no threshold, and a steady descent
20 % below a rule's speed never trips it (SNS-EST-03); a speed that passes a
threshold is reported past it within 2 s (SNS-EST-04).
`test_SNS_EST_03_a_steady_descent_below_a_rule_never_reads_past_it` and
`test_SNS_EST_04_a_speed_that_passes_a_rule_is_reported_within_two_seconds`
hold `constacc` to both.

A pascal per second is 1/(ρg) metres per second, which grows as the air
thins:

| Height | ρg, Pa/m |
|---|---|
| Pad, sea level | 12.0 |
| 9 km | 4.6 |
| 15 km | 1.9 |
| 20 km | 0.86 |
| 30 km | 0.18 |

The same sensor noise is about 70 times the speed noise at 30 km that it is
on a sea-level pad, before the pad-air scale, 0.12 at 30 km, takes most of it
back.

## What a model can and cannot know

A Kalman filter is a model's prediction corrected by measurements, weighted
by how much each is trusted. The model is the hard part: the operator
enters nothing about the rocket, and the configuration has no key for its
mass, motor or drag.

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

So with pressure alone no model of the rocket is known before flight.
`constacc` assumes only that the acceleration changes slowly. `lumped` learns
a lumped thrust and drag from the readings as it flies. Each says when its
model does not explain the readings (SNS-EST-08).

An accelerometer changes this. Acceleration measured on the board does not
pass through the static ports. Integrated, it is a speed and a height
through thrust and through Mach, and pressure corrects its drift. One board
has the part (`src/sd/lsm6ds3.c`), and it feeds the high-rate log.

## Options

B is `constacc`. The others are not built.

### A. Keep the fit, lengthen its memory where the air is thin

Not built: there is no fit (DD-085). The speed for the fire rules would come
from a longer window as the pressure falls. Rate noise goes as T^-1.5, so 4 s
has an eighth of 1 s's. Under a canopy the rate is steady and a 4 s lag costs
nothing; right after a fire it delays the verdict by the window.

### B. A three-state filter on the readings' altitudes

Built: `constacc` (DD-085, DD-114). State altitude, its speed and its
acceleration, with a white-jerk process, on the raw readings' altitudes. The
sensor's noise is tracked in pascals, the same at every height, and weighed
at each reading's metres per pascal, so one tuning serves the whole flight.
Constant work per sample, no window to store. Its covariance is a running
statement of how well the speed is known: the apogee rule's margin is twice
the speed's own standard deviation (FLT-APO-07).

Nothing in it is scheduled:

- R is the tracked sensor noise (below), the same in every phase. A sample
  the sensor's own faults make suspect is not decided on (SNS-PRES-10,
  SNS-PRES-11), and a run of repeated readings is taken back from the
  estimators.
- One process noise serves the whole flight, and the filter is not loosened
  at a fire.
- Through a port error it follows the readings; its "explains" test is what
  keeps a decision off them (`docs/apogee_without_a_mach_flag.md`).

### C. B with a descent model

Not built as written. `lumped` carries a drag term through the whole flight,
a = a_T - g - beta (ρ/ρ_pad) v|v|, learns it from the readings, and loosens it
at each pulse (`docs/lumped_parameter_filter.md`). The fire rules compare the
present speed, not a predicted terminal one.

### D. Accelerometer and pressure fused, on boards that have both

Not built. Vertical acceleration drives the prediction; pressure corrects it.
This is the filter that carries a model through Mach. To settle before it
could be trusted:

- the LSM6DS3 saturates at 16 g, and a hard motor exceeds that;
- the axis is the rocket's, so the gyroscope must hold the tilt, and under
  a canopy the board swings and only pressure is usable;
- the accelerometer feeds the high-rate log, off the flight task (HR-02,
  HR-05). Bringing it onto the flight path is a new contract;
- boards without the part still need a pressure-only estimator.

### E. Leave the estimator, state the rule

Not chosen. No new filter: the fire rules would require the speed above the
threshold on every sample for a hold, say 1 s, and the documentation would
say the thresholds are not dependable above about 15 km. The fire rules hold
nothing (FLT-EMRG-05).

## Rule A

A prediction is not knowledge. A fire is decided on measured evidence
(SYS-DEPLOY-04, DD-081). An estimator says whether its model explains the
readings and has had a second of them with no gap (SNS-EST-08), and apogee
waits on it (FLT-APO-07). After apogee the re-fire and emergency rules act
on the filtered state whenever the sensor gives data (FLT-EMRG-05).

## Scope

The estimators run on every reading, from the pad to the ground. A port
error is readings the model does not explain, and apogee is not decided on
them (DD-092). How high apogee keeps the bounds
of FLT-APO-01 is the board's height for proper operation (SNS-MAX-01).

## A Kalman filter that tracks the noise

The user's direction: a Kalman filter tracks the extra noise at altitude and
runs through it. `constacc` does this with two properties, neither keyed to
height.

- **The gain falls as the air thins, by itself.** A reading's variance in
  altitude is σ² times the square of its metres per pascal, R T / (g p). The
  same sensor noise in pascals is worth more metres high up, and the filter
  leans on its model for longer. That is the lengthened averaging of option
  A, arrived at continuously.
- **The measurement variance is estimated, not assumed.** The scatter
  between successive innovations is the noise the filter is seeing. Tracked
  over about 2 s, it widens the trust interval when the sensor is noisier
  than on the pad, near its limits or in rough air, and narrows it again.
  1.2 Pa (`AF_NOISE_FLOOR_PA`) is the starting value and a floor. A
  reading is never weighed as surer than 0.5 m of height
  (`AF_READING_NOISE_M`): what the model leaves out -- gusts, the airframe,
  the ports -- is metres (DD-113).

The filter is not loosened at a fire. Smoothing a
deployment's deceleration does not matter: the fire rules ask whether the
rocket is too fast, and that condition takes seconds to develop. A lagging
speed after a good deployment costs a few re-fires into a spent igniter,
which is harmless. What the filter must not do is smooth out an overspeed:
SNS-EST-04 bounds its lag to 2 s, so it can only make a re-fire or an
emergency fire late.

A smooth bias, as from a supersonic port, is not noise; the "explains" test
guards that span. `constacc`'s model explains the readings when none has been
more than 4 σ from its prediction for a second, no gap has exceeded 250 ms,
and its acceleration is under 3.5 g (`src/estimator_constacc.c`).

## The filter as built

`constacc` as built, from its first reading:

- **Model.** Constant acceleration in altitude: the acceleration changes by
  a white jerk (`JERK_PSD`, 7 m²/s⁵). No gravity and no drag.
- **Start.** At the first reading, at rest, with the noise at its floor
  (`af_start()`).
- **Raw readings, innovation limited.** No filter stands in front. A reading
  more than 6 σ from the prediction is not used for the first 50 ms of its
  run: a separation's shock lasts that long, and a longer run is data
  (SNS-EST-02, DD-113). A reading set aside says nothing of the model. In the host
  evaluation (`sim/study/kf_launch.py`, `sim/study/kf_extra.py`), a single
  reading 3 to 6 kPa low declares a launch through a raw filter at pad-quiet
  tunings, and holding each innovation to a few of its standard deviations
  inside the update stops a short run of bad readings of any size at no
  cost in delay.
- **The data wins, without a re-seed.** The board cannot know that its
  sensor is wrong. A Kalman filter trends toward data that is stable, so
  after a discontinuity it follows the readings by its own gain. Nothing
  re-seeds it.
- **Offset is not noise.** The noise is tracked from the difference between
  successive innovations, which a model running behind the rocket does not
  change. A sustained offset does not read as extra noise, so it does not
  lower the gain when the filter should be moving.
- **Fires.** Nothing happens at a fire: `constacc` ignores the pulse it is
  told of (SNS-EST-09). The filter trends to the new descent as the readings
  settle on it.

## The modelling study

The review asked the study three things. Where each stands:

1. **Which values are configuration.** The two re-fire speeds, the emergency
   fire speed, `refire_interval` and `fire_gap` are configuration
   (`src/config_fields.h`); each board declares the range and default of the
   last two (PYR-BOARD-01). The estimators' gates, noise floor and process
   noise are constants of the design.
2. **Defaults.** The three speeds default to 0, which turns their rules off
   (DD-094).
3. **The maximum safe flight altitude** is each board's height for proper
   operation, the greatest apogee at which FLT-APO-01 holds (SNS-MAX-01):
   30 km on the MS5607 and 9 km on the BMP280 (`pressure_sensor_range()`).

The central requirement, ruled 2026-10-02: the filtered state must show the
slowing climb, apogee and descent at every height in the sensor's range
(DD-085). FLT-APO-01 states the bounds.

The host prototype of `constacc` is `sim/study/est_proto.py`; `lumped`'s
study is `docs/lumped_parameter_filter.md`. The plant is `sim/mach_plant.c`:
a rocket that can go supersonic, the standard atmosphere's layers above any
pad, and the pressure its static ports report. The firmware's own suites fly
both estimators on it (`test/test_mach.c`, `test/test_flight_profiles.c`).
