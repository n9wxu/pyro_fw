# The Mach flag

How the flight computer keeps a supersonic rocket from firing on corrupted
pressure. There is no Mach flag: each estimator says whether its model
explains the readings, and its apogee is not decided on readings it does
not explain, and none of them is its peak (DD-092; SNS-EST-08, FLT-APO-07,
FLT-APO-08), and the readings must track its height (DD-113). The simple
filter stands in for it only after 3 s of measured speed under 50 m/s
(FLT-APO-10, DD-109). The estimators are in `docs/flight_states.md`, "The
estimators", and `docs/lumped_parameter_filter.md`. The host tests are
`mach_tests` (`test/test_mach.c`) on the plant in `sim/mach_plant.c`.

## The problem

As a rocket nears Mach 1, shock waves form on the airframe. Once it is
supersonic, the static ports sit in flow that has passed through a shock, and
the pressure they sense is not the air's. The error:

- begins near Mach 0.85;
- changes abruptly at each Mach 1 crossing;
- varies with Mach for the whole supersonic flight, not only at the crossings;
- has a size and sign set by the airframe and the port placement, so it cannot
  be modelled or subtracted.

Near Mach 1 the dynamic pressure is comparable to the static pressure, so a
small change in a port's pressure coefficient is an error of several percent.
During a boost that error can grow fast enough to make a climbing rocket look
slow, stopped or falling. A barometric altimeter reads a pressure rise as a
loss of height, and an apogee detector that believes it fires at supersonic
speed.

Fixed timers fail both ways. A draggy rocket on a large motor is supersonic
briefly and slows fast, so a long timer is still running at apogee. A
low-drag rocket coasts supersonic for ten seconds or more, so a short timer
runs out while the data is still corrupt. The operator enters nothing about
the rocket, so neither can be tuned per flight.

## Three rules

1. **Decide only while the model explains the readings.** An estimator
   explains the readings when they agree with its prediction and it has had
   a second of them with no gap of more than 250 ms (SNS-EST-08). No
   rocket's motion produces a port error: its readings break from the
   prediction, the model stops explaining them, and apogee waits.
2. **Apogee is the top of a climb that was seen.** The obeyed estimator must
   be seen climbing and then seen falling, each by more than three standard
   deviations of its rate, explaining the readings throughout (FLT-APO-07,
   `apogee_detected()`). Losing the explanation forgets the climb. A fall
   whose climb was not seen lasts 2 s before it is apogee.
3. **The peak is only what was explained.** The reported peak is the lowest
   filtered pressure the estimator explained, marked a lower bound when the
   climb to apogee was not seen (FLT-APO-08).

None of the rules has a Mach number, a release or a fallback in it. A port
error that makes a boost look like a fall breaks the arc it would need, and
the climb to a real apogee is seen again once the readings settle.

## Physics in raw pressure

The hydrostatic equation, dp/dz = -ρg with ρ = p/(RT), makes the fractional
pressure rate proportional to the vertical speed:

  -ṗ/p = g·v/(R·T)

g = 9.80665 m/s², R = 287.05 J/(kg·K), γ = 1.4. The estimators report the
rate of ln p, which is ṗ/p, and its curvature; a speed is the rate times
RT/g at that pressure (`atmos_scale_height_m()`). One unit of Mach is
-ṗ/p = g·√(γ/(RT)): 0.0384 s⁻¹ at 318 K and 0.0465 s⁻¹ at 216 K. The envelope
is a 10-45 °C pad, 0-2000 m elevation, to 30 km, so the local air runs from
216 K at the tropopause to 318 K on a hot pad.

Decelerating upward, p̈/p ≈ g·a/(R·T): 1 g is 0.00105-0.00155 in p̈/p.

Sign convention: climbing, ṗ is negative. A coasting rocket's -ṗ shrinks
toward zero, so its p̈ is positive.

## Thresholds

| Purpose | Comparison | Where |
|---|---|---|
| `constacc` explains | for the last second, no reading more than 4 σ from its prediction, no gap of more than 250 ms, and the curvature of ln p under 0.005 s⁻², about 3.5 g | `src/estimator_constacc.c` |
| `lumped` explains | the running mean, over about a second, of the squared normalised innovation under 4, and a second of used readings with no gap of more than 250 ms | `src/estimator_lumped.c` |
| A reading not used | more than 6 σ from the prediction, unless it is the third in a row | both estimators |
| Climbing, falling | the rate beyond three times its own standard deviation | `src/apogee_detector.c` |
| A fall whose climb was not seen | 2 s | `src/apogee_detector.c` |
| Arming | below 0.9965 p_pad, about 30 m, having passed 10 m/s and then slowed below it | `src/flight_ascent.c` (FLT-ASC-04, FLT-ASC-08) |

- **The curvature bound** (physical). Near apogee the speed is near zero,
  drag with it, and only gravity acts: 1 g. A port error that turns a climb
  into an apparent fall within a second or two needs far more
  (`docs/apogee_without_a_mach_flag.md`).
- **The 3-sigma margin** (statistical). It widens with the rate's own
  uncertainty, so more noise makes apogee later, not earlier.
- **Arming** (design constant). Apogee is taken once the channels are armed,
  and no channel arms below about 30 m (FLT-ASC-08).

## The estimator

Both estimators run on the raw readings, every one (SNS-EST-07); the flight
obeys the one `estimator` names, `constacc` by default.

- `constacc` is a constant-acceleration Kalman filter on ln p
  (`src/pressure_estimator.c`, `docs/descent_speed_estimator.md`). It
  follows whatever acceleration the readings show, so a port error moves
  its state, and its "explains" test is what keeps a decision off it.
- `lumped` carries one equation of motion with a lumped thrust and drag
  (`docs/lumped_parameter_filter.md`). A reversal faster than gravity fits
  no thrust and drag it can hold, so a port error's readings show as large
  innovations.

Each tracks the sensor's noise from its innovations, never below 1.2 Pa. A
step in the pressure -- a shock crossing the ports -- ends the explanation
until the readings agree with the model again. That is the intended
behaviour. A separation's shock, up to 50 ms, is set aside and ends nothing
(SNS-EST-02): the sensor sits behind the bay's pressure bulkhead.

## The machine

No flag and no state for one. What the flight carries:

- **Every estimator is flown.** From launch to landing the log has an `EST`
  row a second for each, with its height, speed and whether it explains, and
  a row when its own apogee rule is first met (SNS-EST-07). Only the obeyed
  estimator's apogee is the flight's. `/api/status` shows the obeyed one and
  whether it explains (`estimator`, `estimator_explains`).
- **The reported peak** (`max_alt_cm`: telemetry, status, the landing's
  beep-out) is the height of the lowest filtered pressure the obeyed
  estimator explained, on samples the sensor's own faults do not make
  suspect (FLT-APO-08). It is marked a lower bound when the climb to apogee
  was not seen: the log carries PEAK or PEAK_AT_LEAST, and `/api/status`
  shows `peak_lower_bound`.
- **A flight resumed on the way down** has passed apogee, and its peak is a
  lower bound (`src/flight_boot.c`).
- **A sensor giving no data decides nothing** (SNS-PRES-10, SNS-PRES-11): a
  gap or a stuck run makes the samples suspect until a second of new samples
  exists.

## Design choices

| Here | Why |
|---|---|
| Kalman filters in floating point | DD-085 |
| An estimator's filtered state, with "explains" from its innovations | The filter measures the noise and follows it at altitude |
| Apogee waits while the model does not explain the readings | DD-092 |
| Apogee is the top of a climb that was seen | FLT-APO-07 |
| A fall whose climb was not seen lasts 2 s before it is apogee | A level set low in the boost would deploy low |
| Launch at 15 m on the filtered state (FLT-LAUNCH-07) | A rule on p̈ misses launches under about 2.1-2.4 g net |
| The re-fire and emergency rules (PYR-REFIRE-01, FLT-EMRG-01) | DD-082 |
| The configured modes, each compared in pressure (SNS-EST-05) | AGL is that comparison; the others are the operator's choice |

## Assumptions and remaining risks

- **The port model is the only evidence.** "Never early" in the host tests
  is against `mp_port_error_pa()`: none below Mach 0.85, a ramp to Mach 1, a
  step there, a slope beyond. No flight data of a real port error is in the
  tree. An error that builds smoothly enough to stay inside an estimator's
  explanation would pass; that takes an error growing over ten seconds or
  more, which a Mach transition is not.
- **A port error larger than the air's own pressure,** beyond the sensor's
  range, can bring apogee early: by up to 2.4 s on the Python study's 45 km
  flights (`docs/lumped_parameter_filter.md`).
- **A fall whose climb was not seen is late by design:** 2 s. A sensor gap
  of more than 250 ms across apogee leaves the climb unseen and waits for a
  second of new samples as well.
- **A port faking a descent also fools the launch detector.** It reads the
  climb below the pad, so the launch is declared only once the error fades,
  and T+0 with it.
- **A sensor noisier in flight than on the pad** is followed: the estimators
  measure the noise as they fly, and more noise widens the margin apogee
  must clear.
- **A reading a flash operation ran beside** is used like any other, noise
  and all (SNS-PRES-14, DD-106).
- **The air's temperature is not measured in flight.** `lumped` anchors its
  atmosphere on the sensor's temperature at the first reading; `constacc`
  uses none.

## Static ports

- At least 4-5 body diameters behind the nose-cone shoulder, away from rail
  buttons and other protrusions.
- Three or four, evenly spaced around the circumference, so crossflow
  averages out.
- A sealed avionics bay, so the ports are the only path for air, with the
  ports sized for the bay's volume.
- Good placement makes the supersonic error smaller. It never removes the
  need for rule 1.
