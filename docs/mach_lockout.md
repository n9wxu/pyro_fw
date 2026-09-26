# The Mach lockout

How the flight computer keeps a supersonic rocket from firing its drogue on
corrupted pressure, and why each number is what it is. The decision record is
DD-049; the requirements are FLT-MACH-02..07; the tests are `mach_tests`
(`test/test_mach.c`) on the plant in `sim/mach_plant.c`. The prompt this design
answers is `docs/mach-lockout-prompt.md`.

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
loss of height, and an apogee detector that believes it fires the drogue at
supersonic speed.

Fixed timers fail both ways. A draggy rocket on a large motor is supersonic
briefly and slows fast, so a long timer is still running at apogee. A
low-drag rocket coasts supersonic for ten seconds or more, so a short timer
runs out while the data is still corrupt. The operator enters nothing about
the rocket, so neither can be tuned per flight.

## Three rules

1. **Flag "too fast" while the data is still clean.** Below Mach 0.85 the
   climb rate is trustworthy. If it passes Mach 0.62-0.76, the rocket might go
   supersonic, so the flag goes up before any corruption can start.
2. **Once flagged, distrust the data until it proves itself.** The lock lets
   go only when the data has shown, continuously for one second, what only a
   subsonic rocket coasting upward shows:
   - a smooth pressure history: every fit clean;
   - still climbing;
   - slower than the release threshold;
   - slowing at least as hard as gravity alone requires.

   Each condition alone can be faked. A Mach-dependent error can make the
   climb look slow for a moment, or make the rocket look like it is slowing.
   Faking all four at once, smoothly, for a whole second, during a boost or a
   supersonic coast, needs an error that fits a quadratic almost perfectly and
   outweighs the true acceleration. That is far less likely than faking any
   one of them. It is a probabilistic argument, not a proof; the risks below
   say what it leaves.
3. **Never let the lock prevent a deployment.** If the lock still stands when
   clean data shows the rocket falling back below where the flag went up, the
   drogue fires anyway.

Rule 2 never misses apogee. A coasting rocket slows by at least 1 g, and near
apogee drag is negligible. The coast from the release speed (at most Mach
0.57, about 200 m/s) to apogee takes (v_t/g)·atan(v/v_t), where v_t is the
rocket's terminal velocity: about 3 s even for v_t = 20 m/s, and a rocket that
draggy cannot reach Mach 1. The release takes at most about 2 s once the data
is clean (one fit window, then the one-second release). The data is clean
again from Mach 0.85, above the release speed, so the release's latency does
not eat into the coast. On M0's profiles the lock lets go 8.5-16 s before
apogee, and on the low-drag flight to 10 km from the hot pad at least 14.3 s
before it, over 1000 seeds.

## Physics in raw pressure

The hydrostatic equation, dp/dz = -ρg with ρ = p/(RT), makes the fractional
pressure rate proportional to the vertical speed:

  -ṗ/p = g·v/(R·T)

g = 9.80665 m/s², R = 287.05 J/(kg·K), γ = 1.4. So a comparison -ṗ > k·p needs
no logarithm, no altitude and no temperature. One unit of Mach is
-ṗ/p = g·√(γ/(RT)): 0.0384 s⁻¹ at 318 K and 0.0465 s⁻¹ at 216 K. The envelope
is a 10-45 °C pad, 0-2000 m elevation, to about 9 km AGL, so the local air
runs from 216 K at the tropopause to 318 K on a hot pad.

Decelerating upward, p̈/p ≈ g·a/(R·T): 1 g is 0.00105-0.00155 in p̈/p. The
lapse rate adds at most about 0.1 g at release speeds.

Sign convention: climbing, ṗ is negative. A coasting rocket's -ṗ shrinks
toward zero, so its p̈ is positive.

## Thresholds

Every constant is one of three kinds: a physical bound that holds for any
rocket, a statistical confidence in multiples of the pad's σ, or a design
constant of the controller. The ranges are this program's output
(`test_M1_design_note`), over 216-318 K, or over the pad's 283-318 K for the
arm height:

| Purpose | Integer form | Meaning |
|---|---|---|
| Set flag | `1000*(-pdot) > 29*p` | Mach 0.62 to 0.76 |
| Release: slow | `pdot < 0 && 1000*(-pdot) < 22*p` | Mach 0.47 to 0.57 |
| Release: decelerating | `10000*pddot >= 9*p` | 0.58 g to 0.85 g |
| Minimum-altitude arm | `10000*p < 9965*p0` | 29.1 m to 32.7 m |
| Apogee | `p >= 1.0001*p_min` | 0.63 m to 0.93 m |

- **Set flag** (physical). The hottest air has the smallest rate per Mach, so
  in any air the flag goes up before Mach 0.85. Colder air flags earlier,
  which is the safe direction. A rocket whose peak falls between the flag and
  Mach 0.85 is flagged for nothing, and released within 3 s of burnout: the
  burnout's step leaves the window, the rocket slows below the release speed,
  then the release's second (`test_M1_mid_mach_releases`: 2.2-2.6 s).
- **Release: slow** (physical). Also taken in the hottest air, so the true
  Mach at release never exceeds 0.57. It also requires climbing: the release
  must witness an upward coast, and apparent descent while locked is exactly
  what a corrupted boost produces.
- **Release: decelerating** (physical). The parameter-free check. A thrusting
  rocket accelerates upward; a coasting one slows by at least 1 g plus drag.
  The floor sits below 1 g even in the hottest air, for the estimate's noise.
  No upper limit: drag is unknown before flight.
- **Release: sustained** (design constant): every fit clean and meeting both
  conditions for 1 s.
- **Minimum-altitude arm** (design constant): no channel arms below about
  30 m. Today's launch detector already needs 100 ft; this holds if C2 moves
  the launch to p̈.
- **Apogee** (physical): 1.0001 above the lowest pressure a clean fit showed,
  outside the lock (T5-A, DD-048). The prompt's 1.0005 is 3-5 m, and would put
  the drogue 0.8-1.0 s after apogee; this is about 0.4 s.
- **Fallback**: locked, fitted ṗ > 0 on clean fits, and p > p_flag, both for
  1 s (physical). p_flag is the fitted pressure when the flag went up, or the
  sample's own reading if that fit was not clean (DD-050). A
  pressure error bigger than the whole climb since then is not credible, so a
  clean, sustained descent past that level is real. This deployment is late,
  never early.

Rates arrive from the fit as floats and are rounded and clamped before the
integer comparisons (`src/mach_lockout.h`): at most 1 MPa/s and 200 kPa/s², so
every product stays inside an int32 from 1 to 120 kPa, and no real flight comes
near either clamp (`test_M1_integer_forms`).

## The estimator

A least-squares quadratic through the last second of samples, fitted against
each sample's own time and evaluated at the newest (`src/pressure_fit.c`,
SNS-PRES-09, DD-048). The endpoint has no lag on a constant acceleration,
which matters for flagging a hard boost in time. A fit is clean when its
residual RMS is within 2σ and no sample lies more than 4σ from it (statistical:
σ is measured on the pad, in pascals, so the test is the same at any height
and site). A step -- a shock crossing the ports, an ejection charge -- spoils
every fit that holds it for one window. That is the intended behaviour.

The noise of each estimate is σ·√(Σcᵢ²) over its coefficients, here for
σ = 1.2 Pa at 50 Hz, as speed and acceleration at sea level and at 9 km:

| Estimate | Noise | Sea level | 9 km |
|---|---|---|---|
| pdot | 2.24 Pa/s | 0.19 m/s | 0.49 m/s |
| pddot | 4.34 Pa/s^2 | 0.037 g | 0.097 g |

Differentiating raw pressure is unusable: a two-point ṗ at 20 ms carries
1.2·√2/0.02 = 85 Pa/s of noise, and a two-point p̈ 1.2·√6/0.02² = 7300 Pa/s²,
62 g at sea level. At 9 km, 1 g of deceleration is 45 Pa/s² of p̈; the fit's
4.34 leaves the release's 0.58 g floor 6σ above zero, where a 0.5 s window at
50 Hz would leave it 1.1σ (T5-W).

## The machine

The lock is a latch inside ASCENT, not a state, so the state table, TEL-07's
mapping and the web UI keep their states. LOCK, UNLOCK and LOCK_FALLBACK are
events in the flight log and `!MACH LOCK`, `!MACH UNLOCK`, `!MACH FALLBACK` on
telemetry. `/api/status` shows `mach_lock`, `mach_flag_ms` (flight time of the
flag; 0: never) and `peak_lower_bound`.

- **From T+0.** The flag is evaluated on every sample from the first above
  50 cm, on the pad, not from the launch declaration: at 66 g the rocket is
  past Mach 1 by the time the detector has its 100 ft and 100 ms. A flag
  raised on the pad outlives a rise that falls back -- a port faking a descent
  can read the rocket below the pad -- and is forgotten after 10 s with no
  launch.
- **Before any release, any fit.** Setting the flag is the safe direction, so
  it takes the fit whether clean or not, and also the rate over the newest two
  intervals: through a 66 g boost's first second the one-second fit still
  holds the pad and reads the climb 120 m/s slow. After a release only a clean
  fit can flag again, because the coast has been seen and a bad reading must
  not lock out the apogee just ahead.
- **While locked** there is no apogee and p_min is not updated. The release
  restarts p_min at the current pressure.
- **Brownout recovery into ASCENT starts locked,** flagged where it rejoined:
  the flight's speed history is gone.
- **The reported peak** (`max_altitude`: telemetry, status, the landing's
  beep-out) is the height at p_min, so nothing from the locked interval is in
  it. It is marked a lower bound if the lock let go within 2 s of apogee.
- **Deployment after apogee** follows the configured modes on the fit's
  pressure (PYR-MODE-05, PYR-MODE-06), which wait out the bay's pressure after
  each charge.

## Deviations from the prompt

| Prompt | Here | Why |
|---|---|---|
| Precomputed integer fit coefficients, no floating point | Solved per sample against each sample's time, in floating point; the thresholds compare in integers | Flash stalls make the spacing uneven (SNS-PRES-08) |
| Raw samples | The median of three, with σ measured on the same stream | Single glitches never reach the fit |
| 0.5 s at 100 Hz | 1 s at 50 Hz | The release margin at altitude (T5-W) |
| Flag on clean fits | Any fit, and a 40 ms rate, from T+0, before the first release | Hard boosts |
| LOCKED is a state | A latch in ASCENT | The state table, telemetry and UI |
| Launch on p̈ | 100 ft and 5 m/s, held 100 ms (C2 is open) | p̈ misses launches under about 2.1-2.4 g net |
| Each channel fires once | PYR-REFIRE-01's one retry of a drogue whose charge did not light stays | DD-028 |
| Main on p > p_main | The configured modes, on the fit's pressure | AGL is the same comparison; the others are the operator's choice |
| Apogee 1.0005, 10 clean fits | 1.0001, 60 ms of clean fits | The drogue 0.4 s after apogee rather than 0.8-1.0 s |

## Assumptions and remaining risks

- **The release could be faked.** An error would have to keep every fit clean
  for a second while making the climb look slower than Mach 0.47-0.57 and
  decelerating at 0.58-0.85 g or more, during a boost or a supersonic coast.
  `test_M1_port_error_margin` scales M0's fakes-descent port, which turns a
  boost into an apparent descent, from 0.1 to 4 times, both signs, on the
  draggy, low-drag and 30 g profiles. The lock held through every one. It let
  go at Mach 0.34-0.45 after burnout, never in the error, and every drogue
  came after apogee. M0's port is one shape: an error that grows with Mach and
  steps at Mach 1. An airframe whose error in the supersonic coast happened to
  fall as a smooth quadratic, cancelling most of the true rate, is the case
  this argument cannot rule out.
- **The fallback is late by design,** and can be very late: p_flag is set
  during the boost, often a few hundred metres up (169 m on the mid-Mach
  profile, `test_M1_fallback`), so a lock that never releases deploys low. It exists only so the
  rocket does not come in ballistic.
- **A port faking a descent also fools the launch detector.** It reads the
  climb below the pad, so the launch is declared only once the error fades,
  and T+0 with it. The flag is unaffected: it is evaluated from the first
  rise.
- **A bad reading can set the flag,** before the first release, from the
  40 ms rate. It is the safe direction: the lock releases after a second of
  clean coast. Within about 2 s of apogee it cannot, and the fallback deploys
  a second or two late.
- **σ is the pad's,** frozen from a second before T+0. A sensor noisier in
  flight than on the pad makes more fits unclean. That delays the release and
  the apogee; it never releases early.
- **A failed sensor holds everything** (DD-050): a stuck run or a gap in the
  window makes every fit suspect until a whole window of new samples exists.
  The lock can neither be set nor released by it, and no pressure trigger
  acts on it, however long it lasts. A sensor that sticks before apogee and
  never recovers deploys nothing.
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
  need for the lockout.
