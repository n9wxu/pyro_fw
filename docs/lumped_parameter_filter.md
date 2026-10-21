# One lumped-parameter filter for the whole flight

The study asked for under task HA-1 on 2026-10-03: "Do a complete study. I
prefer a single 'simple' model over layers of special cases." It answers
`docs/apogee_without_a_mach_flag.md`, option E. The filter is in the firmware
as `lumped`, the default estimator ("The port (DD-092)" below). The reference
filter, its harness and every number in the study are in
`sim/study/lumped*.py`; the raw output is `sim/study/lumped_results.txt`.

## Bottom line

- **It works, and it is simpler.** One equation tracks the rocket from the
  pad to the ground with two lumped parameters. Over 736 flights inside the
  sensor's range, with and without static-port errors, at 1.2 and 9 Pa of
  sensor noise, it declared apogee early on none, with a median lateness of
  0.18 s and a worst case of 5.1 s.
- **Near apogee its speed error is about half the kinematic filter's**
  (0.55 against 1.03 m/s rms): the constant-acceleration filter of DD-085,
  `constacc`, in Python.
- **One rule covers the transonic span:** decide only while the model
  explains the readings. No flag, phase or Mach number is in it.
- **The physics alone does not reject a port error.** With the "model
  explains the readings" check removed, 224 of 368 flights fire early. The
  model makes that check sharp; the check does the rejecting.
- **The two parameters are not reliably identified.** Height and speed are
  tracked well. The drag and thrust values the filter settles on are often
  wrong, most of all on hard-boost rockets. They are lumped.
- **It needs the pad temperature.** With the standard atmosphere in its place,
  4 of 736 flights are early by up to 0.10 s on the hot pad. The sensor
  measures a temperature with every pressure.
- **In the firmware** (DD-092): `src/estimator_lumped.c`, checked against
  `sim/study/lumped.py` and flown in the firmware's own suites.

## The model

```
dv/dt = a_T - g - beta * (rho(h) / rho_pad) * v * |v|
```

State: height above the pad, vertical speed, and two parameters.

| Parameter | What it lumps | How it is written | Why |
|---|---|---|---|
| `a_T` | every upward force but drag, per unit mass: thrust, and the pad or the ground holding the rocket up | `A0 * ln(1 + e^s)`, the filter carries `s` | never negative: nothing pulls a rocket down but gravity and drag |
| `beta` | drag area per unit mass, as the pad's air would give it | the filter carries `ln(beta)` | never negative, and it acts against the motion: it can stop a climb and cannot turn it into a fall |

The same equation holds on the pad (where `a_T` settles at 1 g), in the burn,
in the coast, under a canopy and on the ground. Nothing is switched. The
measurement is the raw pressure, as ln p, through the atmosphere anchored at
the pad's pressure and temperature. The code is `sim/study/lumped.py`, about
200 lines.

Two things make the two parameters "slightly separable", in the user's words:

- **Time scale.** Thrust is brief and drag persists. Left with no evidence,
  `a_T` fades in 3 s, and `beta` keeps what it has learned. This one line
  matters more than any other part of the filter (below).
- **Speed.** Drag goes with the square of the speed and vanishes at apogee;
  `a_T` does not depend on speed.

## What decides

Three rules read the filter. None has a phase or a Mach number in it.

1. **Launch:** 100 ft above the pad and climbing at 5 m/s (FLT-LAUNCH-07).
2. **Apogee:** the filter has been seen climbing, then is seen falling, by
   more than three times its own uncertainty in speed, with the model
   explaining the readings all the way. If the climb was not seen (a sensor
   gap across apogee, a resume), the fall must last 2 s (FLT-APO-07).
3. **"The model explains the readings":** the running mean of the squared,
   normalised innovation is under 4, and the filter has used a second of
   readings with no gap of more than 250 ms (SNS-EST-08). This is the test
   every decision waits on.

## What runs beside it

Rule 3 is the only consistency test (DD-092). Beside the filter the flight
keeps:

| In the flight code | What it does |
|---|---|
| The arming gate (FLT-ASC-04, FLT-ASC-08) | arms above about 30 m, after 10 m/s was passed and the speed fell below it; apogee is taken once armed |
| The bound to free fall for 2 s after a pulse (PYR-MODE-06) | holds what the triggers read; the filter itself takes no readings for 1 s after the board's own pulse and loosens its drag |
| The thrust report (FLT-ASC-03) | the sign of the filtered acceleration |

Outside the filter: a pressure of zero or less, which has no logarithm, is
not fed (SNS-PRES-06), and a run of eight identical readings is taken back
from the estimators, a second of one being a stuck sensor (SNS-PRES-10).
Inside it: one or two readings more than six standard deviations off are
skipped, and the third in a row is used (SNS-EST-02).

## Results

All on the nine test rockets of `test/flight_run.c`, from a cold sea-level
pad and a hot pad at 2000 m, with the four port models of `test/test_mach.c`,
at 1.2 and 9 Pa of sensor noise. "Late" is the time from the true apogee to
the declared one.

### Apogee

| Case | Flights | Early | Never | Late: median | 90 % | worst |
|---|---|---|---|---|---|---|
| In range (60 m to 30 km), pad temperature known | 736 | 0 | 0 | 0.18 s | 0.50 s | 5.11 s |
| Same, pad temperature read 10 K high | 736 | 0 | 0 | 0.21 s | 0.59 s | 4.75 s |
| Same, pad temperature read 10 K low | 736 | 0 | 0 | 0.16 s | 0.39 s | 0.91 s |
| Same, standard atmosphere instead of the pad temperature | 736 | 4, by up to 0.10 s | 0 | 0.17 s | 0.42 s | 0.99 s |
| 45 km, past the sensor's range | 128 | 16, by up to 2.4 s | 0 | 0.80 s | 1.43 s | 2.85 s |

### The port error scaled from 0.1 to 4 times, both signs

360 flights on the five fast rockets. Every flight whose readings stayed
physically possible, 270 of them, was not early; the latest was 4.75 s.

The other 90 scale the modelled error until it exceeds the air's own pressure
for 7 to 31 s, so the sensed pressure is negative and the readings are
discarded. 22 of those fired early, by 30 to 46 s: when the readings came
back they showed a smooth fall for seconds, which is what a real descent
shows. No real port can do this, and no filter that believes data can tell
it from a descent.

### Speed, against the kinematic filter (m/s rms, clean ports)

| | Coast | Last 3 s before apogee |
|---|---|---|
| Lumped filter, mean over the rockets | 3.21 | 0.55 |
| Kinematic filter (`constacc` in Python), mean over the rockets | 4.44 | 1.03 |
| 30 km at 9 Pa: lumped / kinematic | 2.10 / 4.63 | 1.33 / 4.23 |
| 66 g boost at 9 Pa: lumped / kinematic | 11.97 / 24.59 | 0.36 / 0.74 |
| 30 g boost at 9 Pa: lumped / kinematic | 15.24 / 8.76 | 0.39 / 0.71 |

The coast figure is dominated by one or two seconds after burnout, where the
filter is still moving thrust out and drag in. Rule 3 is false during it.

### Descent, which the fire rules read

| Case | Error in pad-air descent speed, rms | Worst |
|---|---|---|
| Canopy at 20 m/s, from 760 m, 20 km and 30 km | 0.3 to 0.7 m/s | 5.1 m/s |
| No canopy at all, to 385 m/s | 0.2 to 0.7 m/s | 3.5 m/s |
| Canopy lost at 500 m | 35 m/s reported 1.7 s after the loss | |
| Drogue then main, with a 3 kPa charge in the bay at each fire | 1.9 m/s | 19 m/s, in the second after the main opens |
| The same with the bay's readings used at once | 36 to 51 m/s | 1954 m/s |

SNS-EST-04 asks for a lost canopy to be seen within 2 s: 1.7 s.

### The pad

- 8 h of 30 Pa gusts: no false launch. The largest height was 11 m of the
  30 m a launch needs.
- One or two bad readings a minute, from 300 Pa to 60 kPa off, either sign:
  no false launch.
- **A weakness:** in gusts the speed estimate on the pad swings by up to
  24 m/s. On the pad the model is a hover (`a_T` near 1 g), and a gust looks
  like a change of thrust. The height test is what holds the launch back.

### The sensor failing around apogee

| Failure | Early | Apogee declared |
|---|---|---|
| No readings from 1 s before to 1 s after | 0 | +4.0 s |
| No readings from 8 s before to 3 s after | 0 | +6.0 to +6.8 s |
| Stuck for 2 s, from 3 s before | 0 | +2.1 to +2.7 s |
| Stuck for 3 s, from 1 s before | 0 | +5.0 s |
| Two readings 3 kPa off, half a second before | 0 | +0.07 to +0.35 s |
| Ten readings 3 kPa off, 2 s before | 0 | +6.7 to +11.9 s |

Nothing is decided without a second of readings, so a gap costs its own
length plus one to three seconds. The last row is the slowest recovery found
inside the envelope.

### What the parameters recover

| | True | Estimated |
|---|---|---|
| Canopy at 20 m/s | 20 | 20.4 to 20.6, in 13 of 14 flights; 52 in one |
| Main at 5 m/s | 5 | 5.1 to 5.3 |
| Airframe terminal speed, subsonic rockets | 89 | 51 to 85 |
| Airframe terminal speed, low-drag rockets | 397 | 144 to 286 |
| Thrust per unit mass, early burn, 1.2 Pa noise | 101 to 657 | 95 to 631 |
| Thrust per unit mass, early burn, 9 Pa noise | 101 to 657 | 36 to 421 |

The canopy is found well, because the filter is told when a pulse was sent
and loosens the drag at that moment. The airframe's drag is biased high. The
thrust lags at 9 Pa. On one 66 g flight the drag settled a factor of 200 off
and the tracking was still good: the two parameters trade against each other.

## What each part contributes

Each row removes or changes one thing, on the 368 in-range flights.

| Change | Early | Late: median | worst |
|---|---|---|---|
| As chosen | 0 | 0.18 s | 0.78 s |
| No "model explains the readings" check | 224, by up to 82 s | | |
| Thrust never fades | 0 | 0.25 s | 44 s |
| Speed allowed to drift freely | 0 | 0.42 s | 4.4 s |
| Thrust fades in 1 s instead of 3, standard atmosphere | 107, by up to 1.2 s | | |
| Thrust fades in 1 s, pad temperature known | 0 | 0.16 s | 0.64 s |

Every other tuning value, halved and doubled, gives no early fire
(`sim/study/lumped_tune.py`). Two are fragile without rule 2's "seen
climbing, then seen falling": the memory of the fit check, and how close to
zero the thrust may go.

## What the design rests on

1. **The physics does not reject a port error by itself.** A port error
   looks to this model like a sudden stop followed by a fall, which a rocket
   can do. What gives the error away is that the readings then fall faster
   than gravity allows, which shows as large innovations. Rule 3 reads that.
   So the single model needs one consistency test beside it. It does not
   need a lockout.
2. **Limits in the parameters' form, not clamps.** Held positive by clamps,
   with the covariance corrected by hand, the parameters fire early on 85 of
   216 flights. Written into how the parameters are expressed (`a_T` through
   ln(1 + e^s), `beta` as ln(beta)), the limits need no such code and most of
   the failures go.
3. **Thrust and drag cancel.** With both free, the filter can settle on a
   huge thrust balanced by a huge drag and stay there: 31 s late on one
   flight. The 3 s fade on thrust breaks the tie.
4. **A decision from a prediction.** With the readings gone for tens of
   seconds the model runs alone, and on its own it can declare an apogee.
   Rule 3 needs a second of used readings: a fire needs measured evidence
   (DD-081).
5. **The temperature.** On the hot pad the standard atmosphere makes gravity
   look 15 % too strong, and the filter then needs a small constant "thrust"
   to fit. The pad temperature removes the need.

## Limits

- **The study is in Python.** The firmware's own suites (`test/flight_run.c`)
  have more cases: restarts, late loops, stalls, ground test. They fly the C
  port, the default estimator.
- **The port model is the only evidence** about port errors.
- **Cost is estimated, not measured.** State 3 to 4, covariance 6 to 10
  terms, and about six more transcendental calls a reading. In Python it is
  1.7 times the kinematic filter. The C port's prediction and update are
  estimated at about 40,000 cycles a reading, against the 10 ms loop.
- **The pad speed in gusts** needs the height test to stay as it is.
- **"Data is believed"** (SNS-EST-02) reads with SNS-EST-08: a sustained run
  of readings is followed, and a decision waits until the model explains
  it.
- **Beyond the sensor's range** (45 km) it can be up to 2.4 s early with a
  large port error.

## The port (DD-092)

`src/estimator_lumped.c` is `sim/study/lumped.py` in C, single precision,
behind `src/estimator.h`. It is the default estimator. `constacc` flies beside
it, and both are fed every reading (SNS-EST-07).

- **Checked against the reference:** `sim/study/lumped_port_check.py` flies
  54 of the study's flights, replays the readings the reference was fed
  through the C code, and compares the two. On the pad they agree to a few
  parts in a million. Over 875,000 readings, where both explain them, 0.17 %
  have rates more than half a standard deviation apart, and they disagree
  on explaining 0.04 % of the time. At a burnout or a canopy opening neither
  explains the readings, the filter is thrown about, and a rounding
  difference grows for a few seconds, differently on each compiler; nothing
  is decided there. The check holds the totals, not each flight.
- **The pad's temperature** is the sensor's own at the first reading.
- **The peak** is the lowest filtered pressure the model explained
  (FLT-APO-08).
- **The second of evidence** starts again after 250 ms without a used
  reading, not at one missing reading. Restarted at one, it costs 2 s on 2 of
  1000 flights to 10 km, where a sensor repeats itself for eight readings
  near apogee. The sweep and the failure cases above hold with it.
- **The model has no ground.** With readings, they supply it. With none, a
  prediction that reaches the pad's level stops there
  (`stop_at_the_ground()`). Without the stop, ten minutes of a sensor
  repeating itself carry the prediction 19 km below the pad. The study's
  sweep and failure cases hold with it.
- **Flown in the host suites** (`test/test_mach.c`, obeying each estimator):
  never early from a hop to 45 km on the test plant's port errors; 0.09 to
  0.94 s after apogee.
- **Cost:** about 7 kB of flash.
- **Flown on the bench** (DD-078), 2026-10-03: a 3 km flight on MK1C-SD,
  apogee called at the profile's apogee by both estimators (DD-093).
- **Still owed:** the loop time with both filters running.

An accelerometer, on a later MK1D or wired to an MK1C, measures
`a_T - drag` directly. It would enter the same filter as a second
measurement and make the two parameters separable.

## To reproduce

```bash
cd sim/study
python3 lumped_study.py            # every section: about 10 minutes
python3 lumped_tune.py '{"chosen": {"pad_temp_err": 0.0}}' 8
python3 lumped_trace.py "20 km" cold fake 9.0 1 '{"pad_temp_err": 0.0}' 0 80 50
python3 lumped_port_check.py       # the C port against lumped.py
```
