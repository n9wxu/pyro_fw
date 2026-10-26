# Flight math in pressure, altitude for the operator only

Ruled in the requirements review on 2026-10-02, and built (DD-085,
SNS-EST-05): every flight decision compares pressures, and metres exist only
where a person reads or types them.

## What is already in pressure

- Each estimator reports the filtered pressure, the rate and curvature of
  ln p, their uncertainties, and whether its model explains the readings
  (`src/estimator.h`, SNS-EST-01, SNS-EST-08).
- The ground reference averages pressure (`src/ground_reference.c`).
- Apogee is the rate of ln p seen negative, then positive, each by more than
  three times its own standard deviation (FLT-APO-07).
- The peak is the lowest filtered pressure the estimator explained
  (FLT-APO-08).

## What is still in metres

Only what a person reads or types. The settings are in the configured units
(CFG-03). Each sample carries a height, a speed and an acceleration for the
log, telemetry, `/api/status` and the web page (`altitude_cm`, `speed_cms`,
`accel_cms2` in `pp_sample_t`), from the 1976 atmosphere's layers
(`atmos_height_above_m()`, `atmos_scale_height_m()`). None is clamped
(SNS-ALT-01). The speed is the rate times RT/g, and arming compares it with
10 m/s (FLT-ASC-04).

## The proposal

Each setting is compared in pressure terms, against the pad's pressure, with
the standard atmosphere's temperature at each pressure (`src/atmosphere.c`).
p(pad + h) below is `atmos_pressure_above_pa()`: the standard atmosphere's
pressure h above the pad's pressure altitude.

| Setting | Compared in flight | Where |
|---|---|---|
| Launch: 15 m and 15 m/s | p < p(pad + 15 m), and -rate · RT/g > 15 m/s | `launch_detected()` |
| T+0: 50 cm | the raw reading < p(pad + 0.5 m) | `src/pressure_processing.c` |
| Arming: about 30 m | p < 0.9965 p_pad (FLT-ASC-08) | `src/flight_ascent.c` |
| AGL trigger | p ≥ p(pad + h) | `fire_control_step()` |
| FALLEN distance | p ≥ the standard atmosphere's pressure d below the peak | `fire_control_step()` |
| SPEED trigger | rate · RT/g ≥ v | `fire_control_step()` |
| Re-fire and emergency speeds, the descent bands | rate · RT/g · sqrt(rho / rho_pad) against v (FLT-AIR-01) | `fire_control_descent_speed_ms()` |
| Landing: 2 m/s within 30 m | \|rate\| · RT/g < 2 m/s and p > p(pad + 30 m) | `landing_detected()` |
| DELAY | time | |

For 2 s after the board's own pulse the pressure and rate the triggers read
are bounded to free fall from the state before it, so a charge in the bay
cannot bring a trigger forward (PYR-MODE-06).

The speed rows rest on the hydrostatic relation: -ṗ/p = g v/(R T). A speed
is a pressure ratio per second once T is given.

What this gives:

- **The atmosphere's layers at every comparison,** anchored on the pad's own
  pressure: a high pad's AGL triggers act within 3 % of their height
  (SNS-EST-05), and a height above 11 km is the standard's.
- **No clamp can reach a detector.** Nothing is clamped at all (SNS-ALT-01).
- **Noise that does not change with height.** The sensor's σ is the same in
  pascals everywhere, and the estimators track it from their innovations.
- **One coordinate for the estimators,** whose measurement is the pressure.

## What it does not remove

A barometer measures pressure. Turning an operator's metres into pascals
needs the air's temperature between the pad and the trigger. The conversion
takes it from the standard atmosphere at the pad's pressure (SNS-EST-05). On a
day 15 K from standard a height is about 5 % out, as on every barometric
altimeter.

The sensor measures a temperature with every pressure, and the flight log
carries it (`temp_c`, DAT-02). What it measures is the sensor in its bay. On a
pad it settles near the air's, offset by sun on the airframe and the board's
own heat; in flight it lags the air by minutes. `lumped` anchors its own
atmosphere on it at the first reading (DD-092).

A speed threshold takes T(p) from the standard atmosphere at the comparison
(`atmos_scale_height_m()`).

## Limits, and best effort beyond them

The firmware does not stop working at a limit. Three things are separate:

1. **The sensor's rated range.** The MS5607 is 300 to 1100 mbar at full
   accuracy and 10 to 1200 mbar in its linear range
   (`docs/datasheets/MS5607-02BA03_2017-06.pdf`, page 4); the BMP280 is 300 to
   1100 hPa (`docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`, page 7). That
   is about 31 km and 9 km. Every conversion is used, whatever its value: a
   reading beyond the rated range is less accurate and still data
   (SNS-PRES-06). Only a pressure of zero or less, which has no logarithm, is
   not fed to the estimators.
2. **The height for proper operation,** the greatest apogee at which
   FLT-APO-01 holds, declared by each board (SNS-MAX-01, BRD-01):
   `pressure_sensor_range()` gives 30 km for the MS5607 and 9 km for the
   BMP280, and `/api/limits` serves it. Above it the board flies on, on the
   data it has.
3. **No clamp.** No altitude and no setting is clamped (SNS-ALT-01). A
   conversion may report a high altitude wrongly, and that does not reach the
   flight.

An altitude setting above either limit is not clamped or refused. In pressure
it is a comparison that becomes true when the sensor can see it. The Config
tab warns of an AGL or FALLEN setting above the board's height for proper
operation.

## Costs

- The log, telemetry and status API carry metres, converted from the
  filtered pressure at each sample (`src/pressure_processing.c`).
- The log carries the filtered pressure, the reported height, the raw reading
  and the sensor's temperature (DAT-02), so `pyro_sim --replay` flies the
  estimators and detectors again (DAT-08), and the web page's summary reads
  altitude from it.

## What the study should answer

- **The trigger error against truth:** within 3 % of the set height from
  pads at sea level, 1000 m and 2000 m in the standard atmosphere
  (`test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad`), and
  about 5 % out on a day 15 K from standard. Not measured: FALLEN's error from
  peaks up to the sensor's range.
- **Worst-case ratios or T(p) for the speeds:** T(p) from the standard
  atmosphere (FLT-AIR-01).
- **The anchor for the conversion:** the standard atmosphere at the pad's
  pressure (SNS-EST-05). Not measured: how far a sun-soaked or self-heated bay
  moves the sensor's pad temperature, which `lumped` uses.
- **The estimators' coordinates:** `constacc` carries ln p, its rate and its
  curvature; `lumped` carries height and speed, with ln p as its measurement
  (`docs/lumped_parameter_filter.md`).
- **The longest flight the envelope allows:** each board declares its flight
  log capacity as a duration (DAT-09).
