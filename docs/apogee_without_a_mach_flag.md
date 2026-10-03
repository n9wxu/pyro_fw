# Apogee without a Mach flag

Task HA-1, 2026-10-03: "Remove all lockout restrictions. Reevaluate the
behavior of the mach-lockout on the bench with the new filters. Forget the
previous concerns and take a fresh look at the physics and limitations."

## Bottom line

- With clean static ports, the estimator of DD-085 needs no Mach flag at any
  height. Apogee is declared 0.15 to 2.7 s after the true one from 60 m to
  45 km with the flag removed, the same as with it.
- With static ports that misreport near Mach 1, removing the flag and adding
  nothing fires 11 to 101 s before apogee on every supersonic flight tested.
- The flag as built is not safe either. On four of the fastest test flights it
  fires 69 to 97 s before apogee through its own last-resort rule.
- A rule with no flag and no speed thresholds does better than both in the
  host tests: apogee must be the top of a ballistic arc. It fired after apogee
  on every flight, every port error and every scale of port error tested.
- Nothing has been changed in the flight code. The experiment is
  `sim/study/apogee_arc_experiment.patch`. The choice is between the options
  at the end.

## The physics

**What the sensor measures.** The barometer reads the pressure in the
avionics bay, which follows the static ports. Below about Mach 0.8 that is
the air's pressure. Near Mach 1 a shock moves along the airframe and the
ports read wrong by a fraction of the dynamic pressure q = 0.7 p M².

**How big the error is.** At Mach 1, q is 0.7 p. A port error of 5 to 15 % of
q is 3.5 to 10 % of the pressure: 300 to 900 m of apparent height at sea
level. The test model is `mp_port_error_pa()` in `sim/mach_plant.c`: none
below Mach 0.85, a ramp to Mach 1, a step at Mach 1, a slope beyond.

**Why it looks like an apogee.** The error appears or collapses in the one to
three seconds the rocket takes to cross Mach 0.85 to 1. A port that reads high
shows a fall during the boost. A port that reads low shows a fall as the
rocket slows through Mach 1 in the coast. Both are real, sustained pressure
changes, not noise. The estimator follows sustained data (SNS-EST-02), so its
rate turns positive and the apogee rule is met.

**What cannot be known.** Pressure alone cannot tell a port error from
motion. Something else has to: what a rocket can physically do.

**What a rocket can do near apogee.** At the top of a flight the speed is
near zero, so drag is near zero and the only force is gravity. The
deceleration is 1 g. In the estimator's own units that is a curvature of
ln(p) of g / H, about 0.0012 to 0.0016 /s², at every height. A port error
turning a 300 m/s climb into an apparent fall in two seconds needs 15 g or
more, and it arrives as a break the estimator's innovations show.

**What a real apogee always has.** It is approached from a climb that slows
smoothly to zero. A port error produces a descent that starts with a break
and was never preceded by a slow climb on the same smooth stretch of data.

## What was measured

All on the host, on the firmware itself, with `test_TST_03_report` and the
sweeps in `test/test_mach.c`: nine rockets from a 60 m hop to 45 km, two pads,
four port models, and the "fakes descent" port error scaled from 0.1 to 4
times in both signs.

| Apogee rule | Clean ports | Port errors, 54 report flights | Port error sweep, 36 flights |
|---|---|---|---|
| The flag as built (FLT-MACH-02 to FLT-MACH-07) | +0.15 to +2.7 s | 4 early, by 69 to 97 s, at 30 and 45 km | none early |
| No flag, nothing added | +0.15 to +2.7 s | 40 early, by 11 to 101 s | early |
| No flag; apogee only when smooth and curving under about 3.5 g | +0.15 to +2.7 s | none early | 1 early, by 30 s |
| No flag; apogee only on an arc on which a climb was seen (the patch) | +0.15 to +2.7 s | none early; +0.15 to +2.9 s | none early |

The last row is the rule in the patch:

1. **On an arc**: no innovation over 4 standard deviations in the last second,
   no gap or stuck sensor, and |curvature| under 0.005 /s², about 3.5 g.
   Anything else ends the arc and forgets it.
2. **A climb seen**: on this arc the rate has shown the rocket climbing, by
   more than three times the rate's own uncertainty.
3. **Apogee**: on that arc the rate shows a descent, by the same margin. The
   patch held this 60 ms; task T3 has since removed that hold from the flight
   code.
4. **If no climb was seen** on the arc, a descent must hold for 2 s instead.
   This is what fires after a sensor gap or a glitch that spans the apogee
   itself. With 1, 2, 3 or 5 s here no port error in the tests fired early.

It has no flag, no release, no Mach thresholds, no raw-rate path and no
dependence on the level the flag was set at.

## Limitations found

- **The reported peak.** With no flag, a port that reads low leaves a false
  lowest pressure, and the reported maximum altitude is wrong: 3157 m for a
  true 1526 m in `test_FLT_MACH_07_...`. The peak should come from the top of
  the arc at apogee, not from the lowest pressure seen. FALLEN mode measures
  from the same peak.
- **A gap across apogee.** Two tests that lose the sensor across apogee
  (`test_SNS_PRES_11_a_gap_across_apogee_...`,
  `test_SNS_PRES_06_...`) run too short to see rule 4 fire. They need a longer
  window, not a different rule.
- **The model is the limit.** "None early" is against `mp_port_error_pa()`.
  No flight data of a real port error is in the tree. A port error that
  builds slowly and smoothly enough to stay under 3.5 g and under 4 standard
  deviations would pass rule 1. That takes an error growing over ten seconds
  or more, which a Mach transition is not.
- **Arming is separate.** The channels arm once the rocket has passed 30 m
  and slowed below 10 m/s (FLT-ASC-04). That rule reads the same apparent
  speed and was left alone.

## Does the estimator already model a ballistic flight with drag?

No. `pest_update()` in `src/pressure_estimator.c` is kinematic: its state is
ln(p), its rate and its curvature, and its only assumption is that the
curvature changes slowly (a white-jerk process noise, `JERK_PSD`). It has no
gravity and no drag. It reports whatever acceleration the readings show,
which is why it follows a port error.

The arc rule above uses the physics outside the filter, as a test on the
filter's output. Putting the physics inside the filter is the other way to
do it, and is option B.

## Options

### A. The arc rule (the patch)

Remove the flag and every rule tied to it. Apogee is the top of an arc on
which a climb was seen. Take the peak from the top of the arc.

- For: smallest change; no thresholds in Mach; measured safe against every
  port error in the tests, including the four the flag gets wrong; nothing
  changes for a subsonic flight.
- Against: the filter itself still believes the port error, so the altitude
  and speed it reports during the transition are wrong, and the telemetry
  shows them.
- Withdraws FLT-MACH-02 to FLT-MACH-07, the `mach_lock` and `mach_flag_ms`
  status keys and the LOCK events. FLT-APO-01 gains the arc.

### B. A flight model in the filter

State: height, vertical speed and a drag coefficient the filter discovers.
Prediction: dv/dt = -g - k v |v| with the density from the standard
atmosphere, after burnout. A port error then disagrees with the model for
its whole length, shows as a run of large innovations, and is not followed.

- For: the estimate itself stays right through the transition, so the
  reported altitude, the speed and the fire rules all benefit; apogee can be
  predicted ahead, not only seen after.
- Against: it is an extended Kalman filter with a model that is wrong in the
  boost (thrust is unknown), wrong after a deployment (the drag changes by a
  factor of a hundred in a second) and wrong for a flight that arcs over. Each
  needs a switch back to the kinematic model, and each switch is a place to be
  wrong. "Data is believed" (the review's rule 2) and "a sustained run is
  followed" (SNS-EST-02) both have to be reworded, because this filter
  disbelieves sustained data by design.
- Cost: a new estimator, its tuning study, and every flight test re-measured.

### C. A, then B for the coast only

The arc rule decides apogee. A model-based predictor runs beside the
kinematic filter from burnout to apogee, only to flag when the two disagree,
and only for the record at first.

- For: A's safety now; B's evidence gathered on real flights before anything
  depends on it.
- Against: two estimators to keep.

### D. Keep the flag

- Against: it is the most complicated of the four, it is the only one with
  thresholds that depend on the air's temperature, and it fires early on the
  30 and 45 km flights in its own test report.

### E. One lumped-parameter filter, with no phases (2026-10-03 discussion)

The user's direction: no switched phase models. One filter tracks the
pressure smoothly from the pad to the ground and adjusts lumped parameters as
it goes, because a barometer cannot separate mass, drag and thrust.

```
dv/dt = a_T − g − β · (ρ(h) / ρ_pad) · v · |v|
```

- **State:** height (as ln p), vertical speed, `a_T`, `β`.
- **`a_T`**: thrust per unit mass. Never negative. It decays toward zero by
  itself, so a burn that ends needs no burnout detector.
- **`β`**: drag area per unit mass, as the pad's air would give it. Never
  negative. It changes slowly, and is allowed to jump when the readings
  insist, which is what a canopy opening looks like.
- **Nothing is switched.** The same equation holds on the pad, in the burn,
  in the coast, under a canopy and on the ground. Only the two parameters
  move.

What is separable, slightly:

- Early in the burn the speed is low, drag is small, and `a_T` shows almost
  alone.
- In the coast `a_T` is zero, and `β` shows alone.
- Drag area does not change before deployment and mass changes only by the
  propellant, so the coast's `β` applies to the burn, less a few percent.
  With it, the burn's `a_T` can be recovered.

What rejects a port error, with no flag:

- Drag can only slow a rocket toward zero speed. It cannot reverse it.
- Thrust cannot be negative.
- So the only thing that turns a climb into a descent is gravity, at 1 g. A
  reversal faster than that fits no value of `a_T` and `β`, shows as a run of
  large innovations, and is not followed. This is the arc rule's physics,
  inside the filter instead of beside it.

What is not yet known, and a Python study on the test flights would show:

- whether the two parameters stay stable through a 30 g burn and a canopy
  opening without a switch;
- whether a port error during a supersonic burn is rejected or absorbed as a
  short rise in `β`;
- what "data is believed" (SNS-EST-02) becomes, since this filter refuses
  sustained data that no parameters can explain;
- the cost in the 384 kB slot of MK1B and in the 20 ms loop.

An accelerometer (a later MK1D, or one wired to an MK1C) measures
`a_T − drag` directly and makes the parameters separable.

## To reproduce

The patch is against commit d345755, before the apogee hold was removed.

```bash
git checkout d345755
git apply sim/study/apogee_arc_experiment.patch
export CI_BUILD=1
cmake -S . -B build && cmake --build build --target mach_tests
git checkout -- src && rm src/apogee_detector.[ch]
```

The patch still links `mach_lock.c` with its flag switched off, so the tests
that assert the flag's own behaviour fail under it. Those tests go with the
flag.
