# Flight math: altitudes inside, heights in the pad's air

Every reading becomes an altitude, the estimators filter altitudes, and every
flight decision compares a height above the pad or a speed in the pad's air
(DD-114, DD-085, SNS-EST-01, SNS-EST-05).

## The two coordinates

- **An altitude** is the 1976 U.S. Standard Atmosphere's height of a pressure
  (`atmos_altitude_m()`, `src/atmosphere.c`). It depends on nothing but the
  pressure, so it never moves when the pad's pressure or temperature does,
  and a restart in flight takes it up again from the next reading.
- **A height** is true metres above the pad, in the pad's air: the standard
  atmosphere offset at every height by ΔT, the pad's temperature less the
  standard's at the pad (`air_over_pad()`). From dz = (R T / g) d(ln p):

  h = (z − z_pad) + (R ΔT / g) ln(p_pad / p)

  and a height's speed is the altitude's speed times 1 + ΔT / T, T the
  standard's temperature at that altitude (`air_height_m()`, `air_scale()`).
  With ΔT = 0 a height is the difference of two altitudes.

`src/pressure_processing.c` converts once a sample: the reading's altitude for
the estimators, and the obeyed estimator's altitude, speed and acceleration
to the height, speed and acceleration every flight step reads
(`pp_sample_t`). Nothing is clamped (SNS-ALT-01).

## The pad's air

- **Its temperature** is the sensor's on the pad, followed until the ground
  is frozen at launch (GND-CAL-04); in flight the sensor's temperature moves
  no height. `standard_atmosphere=true` in config.ini makes ΔT zero.
- **Its pressure** is the ground reference (`src/ground_reference.c`), which
  averages the filtered pressure on the pad.
- **A restart in flight** takes both from the pad record (FLT-BROWN-01).
- **The bench flight** turns its profile's heights into pressures in the
  board's pad air (`fsim_pressure()`), so its heights read as flown.
- **The sensor's temperature** is still read with every pressure: the MS5607
  compensates each pressure with it (SNS-PRES-15).

## Every comparison

| Setting | Compared in flight | Where |
|---|---|---|
| Launch: 15 m | filtered height > 15 m | `launch_detected()` |
| T+0: 50 cm | the raw reading's height > 0.5 m | `src/pressure_processing.c` |
| Arming: 30 m | filtered height > 30 m (FLT-ASC-08) | `src/flight_ascent.c` |
| AGL trigger | height ≤ h | `fire_control_step()` |
| FALLEN distance | height ≤ peak − d | `fire_control_step()` |
| SPEED trigger | descent speed ≥ v | `fire_control_step()` |
| Re-fire and emergency speeds, the descent bands | descent speed · sqrt(rho / rho_pad) against v (FLT-AIR-01) | `fire_control_descent_speed_ms()` |
| Landing: 2 m/s within 30 m | \|speed\| < 2 m/s and height < 30 m | `landing_detected()` |
| Apogee | the estimator's altitude and speed (FLT-APO-07) | `apogee_detected()` |
| The simple filter | the raw readings' heights (FLT-APO-09) | `src/simple_apogee.c` |
| DELAY | time | |

For 2 s after the board's own pulse the height and speed the triggers read
are held to free fall from the state before it, so the shock of the
separation cannot bring a trigger forward (PYR-MODE-06).

Apogee is decided in the estimator's own altitudes: the top is a shape, which
no temperature moves.

## The estimator

- **`constacc`** carries altitude, speed and acceleration, with white jerk of
  7 m²/s⁵ (`src/altitude_filter.c`). It tracks the sensor's noise in pascals
  and weighs each reading by its metres per pascal (`atmos_m_per_pa()`),
  never as surer than 0.5 m (DD-113). It is the one the build carries
  (DD-115).

## What the pad's temperature does not remove

- **The bay's own temperature.** On a pad the sensor settles near the air's,
  offset by sun on the airframe and the board's own heat. That offset is in
  every height.
- **The column's shape.** The pad's air is the standard's lapse with one
  offset at every height; a real day's inversions and fronts are not in it.
- **The air in flight.** The sensor lags the air by minutes, and the offset
  is frozen at launch.

## Limits, and best effort beyond them

The firmware does not stop working at a limit. Three things are separate:

1. **The sensor's rated range.** The MS5607 is 300 to 1100 mbar at full
   accuracy and 10 to 1200 mbar in its linear range
   (`docs/datasheets/MS5607-02BA03_2017-06.pdf`, page 4); the BMP280 is 300 to
   1100 hPa (`docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`, page 7). That
   is about 31 km and 9 km. Every conversion is used, whatever its value: a
   reading beyond the rated range is less accurate and still data
   (SNS-PRES-06). Only a pressure of zero or less, which has no altitude, is
   not fed to the estimators.
2. **The height for proper operation,** the greatest apogee at which
   FLT-APO-01 holds, declared by each board (SNS-MAX-01, BRD-01):
   `pressure_sensor_range()` gives 30 km for the MS5607 and 9 km for the
   BMP280, and `/api/limits` serves it. Above it the board flies on, on the
   data it has.
3. **No clamp.** No altitude and no setting is clamped (SNS-ALT-01). A
   conversion may report a high altitude wrongly, and that does not reach the
   flight.

A height setting above either limit is not clamped or refused: it is a
comparison that becomes true when the sensor can see it. The Config tab warns
of an AGL or FALLEN setting above the board's height for proper operation.

## What the log carries

The filtered pressure, the reported height, the raw reading and the sensor's
temperature (DAT-02), so `pyro_sim --replay` flies the estimators and
detectors again (DAT-08), and the web page's summary reads heights from it.

## Measured, and open

- **AGL triggers:** within 3 % of the set height from pads at sea level,
  1000 m and 2000 m in the standard atmosphere
  (`test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad`), and
  within 1 % and 1 m from a pad 5 K colder than the standard at sea level and
  one 32 K warmer at 2000 m
  (`test_SNS_EST_05_an_agl_trigger_acts_at_its_height_in_air_off_the_standard`).
- **Not measured:** FALLEN's error from peaks up to the sensor's range, and
  how far a sun-soaked or self-heated bay moves the pad's temperature.
- **The longest flight the envelope allows:** each board declares its flight
  log capacity as a duration (DAT-09).
