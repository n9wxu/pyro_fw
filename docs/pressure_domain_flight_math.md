# Flight math in pressure, altitude for the operator only

Ruled in the requirements review on 2026-10-02: every
flight decision compares pressures, and metres exist only where a person
reads or types them. Nothing here is started. It is part of the modelling
study in `docs/descent_speed_estimator.md`.

## What is already in pressure

- The ground reference averages pressure, not altitude (GND-CAL-02).
- The Mach lock's thresholds are pressure ratios in integers
  (`src/mach_lockout.h`, DD-049).
- Apogee is a ratio above the lowest fitted pressure (FLT-APO-01).
- The fit is on pressure: p, ṗ, p̈ (DD-048).

## What is still in metres

`pp_pressure_to_height_cm()` runs at every sample:
h = 44330 (1 − (p/p0)^(1/5.2561)), then its slope and curvature turn ṗ and
p̈ into a speed and an acceleration. From those come launch (100 ft,
5 m/s), T+0 (50 cm), arming (10 m/s), AGL, FALLEN, SPEED, landing (2 m/s,
30 m), the descent bands, and the fire rules' speeds with DD-079's air
scale on top.

The formula's known errors reach decisions through that path
(`docs/high_altitude_flight.md`, sections 3 and 4): it assumes 288.15 K at
every pad, so a 1500 m pad fires a 300 m main at 290 m, and above 11 km it
reads short, 25.3 km at a true 30 km. The clamps that belong to reporting
have twice leaked into detection (DD-042).

## The proposal

Each setting is converted once, when its reference is known, into the
pressure quantity the loop compares. The loop converts nothing.

| Setting | Converted when | Compared in flight |
|---|---|---|
| Launch height, T+0 height, arm height, landing height | the pad reference freezes | p against p0 × a ratio |
| AGL trigger | the pad reference freezes | p ≥ p0 × r(h) |
| FALLEN distance | apogee | p ≥ p_min × r(d, p_min) |
| SPEED, launch speed, arm speed, landing speed | at the comparison, from p | ṗ against k(v, T(p)) × p |
| Re-fire and emergency speeds, in pad air | at the comparison, from p | ṗ against g v √(ρ(p) ρ_pad) |
| DELAY | never | time |

The speed rows rest on the hydrostatic relation the Mach lock already uses:
−ṗ/p = g v/(R T). A speed is a pressure ratio per second once T is given.

What this buys:

- **One conversion per setting, off the decision path.** It can afford the
  1976 atmosphere's layers and a pad temperature taken from the pad's own
  pressure, which removes the 3 % high-pad error and the short reading above
  11 km from the triggers.
- **No clamp can reach a detector.** SNS-ALT-02 and -03 become rules about
  what is displayed and nothing else.
- **Noise that does not change with height.** The sensor's σ is the same in
  pascals everywhere. Thresholds and the filter's measurement variance are
  stated once.
- **Integers in the loop**, as the Mach lock shows, and no `powf` per sample.
- **One coordinate for the post-release filter**, which wants pressure for
  its measurement anyway.

## What it does not remove

A barometer measures pressure. Turning an operator's metres into pascals
needs the air's temperature between the pad and the trigger. The sensor
measures a temperature with every pressure, and that is information the
board has: it shall be captured and logged with the pressure, at every log
rate. What it measures is the sensor in its bay. On a pad it settles near
the air's, offset by sun on the airframe and the board's own heat; in
flight it lags the air by minutes. So the atmosphere above the pad is still
assumed, once, at the conversion, where it can be stated and tested. The
pad's temperature need not be: the study compares the conversion anchored
on the sensor's pad temperature against the standard atmosphere's, using
logged flights and bench soaks. On a day 15 K from standard an unanchored
height is about 5 % out, as on every barometric altimeter. The requirement
should say which anchor is used and what error remains.

A speed threshold needs T(p) at the comparison. The Mach lock chose fixed
ratios for its worst air. The fire rules need the standard atmosphere's
T(p), which DD-079 already computes.

## Limits, and best effort beyond them

The firmware does not stop working at a limit. Three different things are
called a ceiling today, and they should be separated:

1. **The sensor's rated range.** MS5607 10 to 1200 mbar, BMP280 300 to
   1100 hPa, from the datasheets in `docs/datasheets/` (pages to be cited).
   That is about 31 km and 9 km. SNS-PRES-06 discards a reading outside it.
   Whether a reading beyond the rated range is knowledge is a ruling to
   make: under the never-fire-blind rule it is not, and the flight waits
   for the sensor to return to range, then acts at once on what it shows.
2. **The height for proper operation**, from the modelling study: the
   greatest apogee at which the Mach lock releases before apogee and the
   filter stabilises. Above it the board flies on, with apogee from the
   fallback, and the documentation says so.
3. **No clamp.** Nothing is clamped to 8000 m (ruled). That figure was a
   limit of the conversion formula. A simple conversion may report high
   altitudes wrongly, and that does not reach the flight.

An altitude setting above either limit is not clamped or refused. In
pressure it is simply a comparison that becomes true when the sensor can
see it. PYR-ALT-01's clamp becomes a warning.

## Costs

- Every detector in `src/flight_states.c` changes its comparisons. The
  logic does not change, and each rewritten test must prove the same
  decision at the same sample on recorded flights (`pyro_sim --replay`).
- The log, telemetry and status API still carry metres. They convert at
  output, off the flight task where they can.
- The simulator and the web UI's summary read altitude from the log, so
  the log keeps both pressure and the reported height.

## What the study should answer

- The trigger error against truth, in metres, for the present path and the
  pressure path: pads from sea level to 2000 m, 10 to 45 °C, standard and
  ±15 K atmospheres, triggers from 100 m to 3 km.
- The same for FALLEN from peaks up to the sensor's range.
- Whether fixed worst-case ratios or T(p) serve the speed thresholds.
- Whether the sensor's temperature on the pad is a better anchor for the
  conversion than the standard atmosphere's, and by how much a sun-soaked
  or self-heated bay spoils it.
- The state coordinates for the post-release filter: p, ln p, or height
  with pressure as the measurement.
- The longest flight the envelope allows, which sets the log capacity each
  board must declare.
