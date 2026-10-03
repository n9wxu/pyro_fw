# The Mach flag

How the flight computer keeps a supersonic rocket from firing on corrupted
pressure, and why each number is what it is. The decision records are DD-049
and DD-085; the requirements are FLT-MACH-02 to FLT-MACH-07; the tests are
`mach_tests` (`test/test_mach.c`) on the plant in `sim/mach_plant.c`. The
prompt this design answers is `docs/mach-lockout-prompt.md`. The code is
`src/mach_lock.c`, with the thresholds in `src/mach_lockout.h`.

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

1. **Flag "too fast" while the data is still clean.** Below Mach 0.85 the
   climb rate is trustworthy. If it passes Mach 0.62-0.76, the rocket might go
   supersonic, so the flag goes up before any corruption can start.
2. **Once flagged, distrust the data until it proves itself.** The flag is
   released only when the data has shown, continuously for one second, what
   only a subsonic rocket coasting upward shows:
   - a smooth pressure history: no reading more than four standard deviations
     from the filter's prediction for the last second;
   - still climbing;
   - slower than the release threshold;
   - slowing at least as hard as gravity alone requires.

   Each condition alone can be faked. A Mach-dependent error can make the
   climb look slow for a moment, or make the rocket look like it is slowing.
   Faking all four at once, smoothly, for a whole second, during a boost or a
   supersonic coast, is far less likely than faking any one of them. It is a
   probabilistic argument, not a proof; the risks below say what it leaves.
3. **Never let the flag prevent a deployment, and never let it bring one
   early.** If the flag still stands, apogee is found from the gravity
   signature of a descent: smooth data, a slow descent, gathering speed as
   gravity gives, for 2 s. As a last resort the flag also falls when smooth
   data shows the pressure rising above the pressure the flag was set at.

Rule 2 does not miss apogee. A coasting rocket slows by at least 1 g, and near
apogee drag is negligible, so the coast from the release speed (at most Mach
0.57) to apogee takes seconds, and the data is clean again from Mach 0.85,
above the release speed.

## Physics in raw pressure

The hydrostatic equation, dp/dz = -ρg with ρ = p/(RT), makes the fractional
pressure rate proportional to the vertical speed:

  -ṗ/p = g·v/(R·T)

g = 9.80665 m/s², R = 287.05 J/(kg·K), γ = 1.4. So a comparison -ṗ > k·p needs
no logarithm, no altitude and no temperature. One unit of Mach is
-ṗ/p = g·√(γ/(RT)): 0.0384 s⁻¹ at 318 K and 0.0465 s⁻¹ at 216 K. The envelope
is a 10-45 °C pad, 0-2000 m elevation, to 30 km, so the local air runs from
216 K at the tropopause to 318 K on a hot pad.

Decelerating upward, p̈/p ≈ g·a/(R·T): 1 g is 0.00105-0.00155 in p̈/p. The
lapse rate adds at most about 0.1 g at release speeds.

Sign convention: climbing, ṗ is negative. A coasting rocket's -ṗ shrinks
toward zero, so its p̈ is positive.

## Thresholds

The filtered state is u = ln(p), its rate and its acceleration (DD-085). The
rate is ṗ/p directly, and the acceleration plus the rate squared is p̈/p, so
each threshold is a plain comparison of floats: the same Mach, or the same
fraction of g, at any site elevation. The Mach and g ranges are over
216-318 K.

| Purpose | Comparison (`src/mach_lockout.h`) | Meaning |
|---|---|---|
| Set flag | `-rate > 0.029` | Mach 0.62 to 0.76 |
| Release: slow ascent | `rate < 0 && -rate < 0.022` | Mach 0.47 to 0.57 |
| Under gravity | `curve + rate * rate >= 0.0009` | 0.58 g to 0.85 g |
| Minimum-altitude arm | `p < 0.9965 * p_pad` | about 30 m |

`test_FLT_MACH_02_thresholds` holds each to a hair either side.

- **Set flag** (physical). The hottest air has the smallest rate per Mach, so
  in any air the flag goes up before Mach 0.85. Colder air flags earlier,
  which is the safe direction. A rocket whose peak falls between the flag and
  Mach 0.85 is flagged for nothing, and released after burnout
  (`test_FLT_MACH_03_a_mid_mach_flight_releases_after_burnout`).
- **Release: slow** (physical). Also taken in the hottest air, so the true
  Mach at release never exceeds 0.57. It also requires climbing: the release
  must witness an upward coast, and apparent descent while flagged is exactly
  what a corrupted boost produces.
- **Under gravity** (physical). The parameter-free check. A thrusting rocket
  accelerates upward; a coasting one slows by at least 1 g plus drag. The
  floor sits below 1 g even in the hottest air, for the estimate's noise. No
  upper limit: drag is unknown before flight.
- **Release: sustained** (design constant): smooth, slow, climbing and under
  gravity for 1 s.
- **Minimum-altitude arm** (design constant): no channel arms below about
  30 m (FLT-MACH-06).
- **Apogee, with no flag** (statistical): the filtered pressure rising by
  more than three times its rate's own uncertainty, and above its lowest
  value by more than twice the estimate's, with no hold time (FLT-APO-01). The lowest
  value is tracked only while no flag stands.
- **Apogee under the flag** (physical, FLT-MACH-04): smooth data, a descent
  slower than the release rate, and under gravity, for 2 s. A port error that
  makes a boost read as a descent does not also read as free fall for two
  seconds. Or, as the last resort: smooth data, the pressure rising, and
  above the pressure at which the flag was set. A pressure error bigger than
  the whole climb since the flag is not credible. Either way this deployment
  is late, never early
  (`test_FLT_MACH_04_apogee_is_found_while_the_flag_stands`).

## The estimator

One Kalman filter on the raw readings (`src/pressure_estimator.c`, DD-085;
its design is in `docs/descent_speed_estimator.md`). The sensor's noise is
measured from the readings. A reading more than six standard deviations from
the prediction is skipped, unless it is the third in a row.

"Smooth" is the filter's own word on the data: no reading in the last second
fell more than four standard deviations from its prediction
(`src/pressure_processing.c`). A step in the pressure -- a shock crossing the
ports, an ejection charge -- breaks it for a second. That is the intended
behaviour.

The filter lags a hard boost. So until the flag has been released once, the
rate across the newest two reading intervals can set it as well.

## The machine

The flag is a latch inside ASCENT, not a state, so the state table, TEL-07's
mapping and the web UI keep their states. LOCK, UNLOCK and LOCK_FALLBACK are
events in the flight log and `!MACH LOCK`, `!MACH UNLOCK`, `!MACH FALLBACK` on
telemetry. `/api/status` shows `mach_lock`, `mach_flag_ms` (flight time of the
flag; 0: never) and `peak_lower_bound`.

- **From the first rise.** The flag is evaluated on the pad from the first
  reading of a rise (`mach_lock_on_pad()`), not from the launch declaration:
  a hard boost is past Mach 0.85 before the detector has its 100 ft and
  100 ms. A flag set on the pad is forgotten after 10 s once the rise has
  fallen back with no launch.
- **Before any release, either rate.** Setting the flag is the safe
  direction, so the filtered rate or the two-interval rate of the raw
  readings sets it. After a release only the filtered rate can set it again,
  so one bad reading cannot flag the rocket just before apogee.
- **A sensor giving no data sets and releases nothing.** A stuck sensor or a
  gap makes the samples suspect, and a suspect sample is not acted on.
- **While flagged** apogee is not taken from the pressure minimum, and the
  lowest pressure is not updated. The release restarts it at the current
  pressure.
- **A flight resumed climbing starts flagged,** at the pressure it rejoined
  at: its speed history is gone (`src/flight_boot.c`).
- **The reported peak** (`max_alt_cm`: telemetry, status, the landing's
  beep-out) is the height at the lowest pressure outside the flag, so nothing
  from the flagged span is in it. It is marked a lower bound if the flag was
  released within 2 s of apogee, or if apogee was found while it stood
  (FLT-MACH-07).
- **An apogee found under the flag arms the channels** if they were not yet
  armed, and the fire rules then run as on any descent.

## Deviations from the prompt

| Prompt | Here | Why |
|---|---|---|
| Precomputed integer fit coefficients, no floating point | One Kalman filter in floating point; the thresholds compare floats | One estimator for every decision (DD-085) |
| A quadratic fit over a window | The filtered state, with "smooth" from its innovations | The filter measures the noise and follows it at altitude |
| Flag on clean fits | The filtered rate, and a two-interval raw rate before the first release | Hard boosts |
| LOCKED is a state | A latch in ASCENT | The state table, telemetry and UI |
| Launch on p̈ | 100 ft and 5 m/s on the filtered state | p̈ misses launches under about 2.1-2.4 g net |
| Each channel fires once | The re-fire and emergency rules (PYR-REFIRE-01, FLT-EMRG-01) | DD-082 |
| Main on p > p_main | The configured modes, each converted to a pressure once | AGL is the same comparison; the others are the operator's choice |
| The fallback waits for the flag's level | The gravity signature of a descent first; the flag's level as the last resort | A flag set low in the boost would deploy low |

## Assumptions and remaining risks

- **The release could be faked.** An error would have to keep the data smooth
  for a second while making the climb look slower than Mach 0.47-0.57 and
  decelerating at 0.58-0.85 g or more, during a boost or a supersonic coast.
  `test_FLT_MACH_03_no_release_inside_a_port_error` scales the plant's
  fakes-descent port, which turns a boost into an apparent descent, from 0.1
  to 4 times, both signs, on the draggy, low-drag and 30 g profiles: the flag
  is never released before burnout or while the port's error is live, and no
  fire comes before apogee. The plant's port is one shape: an error that
  grows with Mach and steps at Mach 1. An airframe whose error in the
  supersonic coast happened to fall smoothly, cancelling most of the true
  rate, is the case this argument cannot rule out.
- **Apogee under the flag is late by design.** The gravity signature takes
  2 s of descent. The flag-level rule, where it is the one that acts, can be
  very late: the flag is set during the boost, often a few hundred metres up.
  It exists only so the rocket does not come in ballistic.
- **Beyond about Mach 2 with a large port error** the gravity signature can
  be met early on the test plant. That is outside the envelope this design
  was measured in.
- **A port faking a descent also fools the launch detector.** It reads the
  climb below the pad, so the launch is declared only once the error fades,
  and T+0 with it. The flag is unaffected: it is evaluated from the first
  rise.
- **A bad reading can set the flag,** before the first release, from the
  two-interval rate. It is the safe direction: the flag is released after a
  second of smooth coast. Within about 2 s of apogee it cannot be, and apogee
  is then found under the flag, late. A conversion a flash operation ran
  beside never reaches the rate (DD-068). On MK1B a beep code can set it on
  the bench; MK1C shows none.
- **A sensor noisier in flight than on the pad** is followed: the filter
  measures the noise as it flies. More noise delays the release and the
  apogee; it never brings either early.
- **A failed sensor decides nothing** (SNS-PRES-10, SNS-PRES-11): a stuck run
  or a gap makes the samples suspect until a second of new samples exists.
  The flag can neither be set nor released by it, and no pressure trigger
  acts on it, however long it lasts.
- **The air's temperature is not measured.** Every threshold is taken at the
  envelope's worst end, so colder air only flags earlier and releases later.

## Static ports

- At least 4-5 body diameters behind the nose-cone shoulder, away from rail
  buttons and other protrusions.
- Three or four, evenly spaced around the circumference, so crossflow
  averages out.
- A sealed avionics bay, so the ports are the only path for air, with the
  ports sized for the bay's volume.
- Good placement makes the supersonic error smaller. It never removes the
  need for the flag.
