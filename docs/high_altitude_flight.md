# High-Altitude Flight: What Changes With Height

What changes as a flight goes high, and what the firmware does about each.
The bench flight (DD-078, `POST /api/sim/flight`) flies a profile through the
whole flight software on a real board. The host flies the firmware with the
rockets of `test/flight_run.c` (`fly()`), from a 60 m hop to 45 km, in
`test/test_flight_profiles.c`, `test/test_fire_rules.c` and
`test/test_mach.c`.

## 1. Descent speeds are judged in the pad's air (DD-079)

A canopy's terminal rate goes as 1/sqrt(rho): a drogue that settles at
25 m/s over the pad falls at about 200 m/s at 30 km. The descent phases and
the fire rules judge a descent speed as the pad's air would give it: the
true speed scaled by sqrt(rho / rho_pad), the density of the pad's air at
each height (FLT-AIR-01, DD-114, `air_pad_ratio()`). The scale is 1 at the pad, 0.95 1 km above a
sea-level pad, 0.27 at 20 km and 0.12 at 30 km. A good drogue at 20 km is not
taken for a failed one, and a failing one is still caught there
(`test_FLT_AIR_01_a_good_drogue_at_20_km_is_not_a_failed_one`,
`test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen`).

## 2. Apogee at every height (DD-092)

Apogee is the obeyed estimator seen climbing, then seen falling, with its
model explaining the readings throughout (FLT-APO-07), at every height.

### Evidence

On the host, at sensor noise of 1.2, 3 and 9 Pa, four seeds each: apogee is
never early, and is declared within 0.5 s of the true one to 10 km, 1.5 s at
20 km and 2.5 s at 30 km (FLT-APO-01,
`test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height`).
Obeying each estimator, with clean ports and three port errors, from a hop to
45 km: `lumped` is never early and 0.09 to 0.94 s after apogee; `constacc` is
never early and 0.09 to 1.8 s after it, later than 2 s on two of the 30 km
and 45 km cases (DD-092).

Above the sensor's range the flight goes on with the data it has
(SNS-MAX-01): a 45 km flight at 3 Pa declares apogee after the true one,
fires both channels and lands
(`test_SNS_MAX_01_best_effort_above_the_sensors_range`).

### Why

A pascal per second is 1/(rho g) metres per second, and rho g falls with the
air:

| Height | rho g, Pa/m |
|---|---|
| Pad, sea level | 12.0 |
| 9 km | 4.6 |
| 15 km | 1.9 |
| 20 km | 0.86 |
| 30 km | 0.18 |

So the same sensor noise in pascals is about 70 times the noise in speed at
30 km that it is on a sea-level pad. The estimators average longer as the air
thins, with no schedule keyed to height: each tracks the sensor's noise in
pascals, and a reading's weight in altitude falls with the square of its
metres per pascal (`src/altitude_filter.c`, `src/estimator_lumped.c`). The
apogee rule's margin is twice the speed's own uncertainty, so a noisier
speed makes apogee later, not earlier.

### A fall whose climb was not seen

It lasts 2 s before it is apogee (FLT-APO-07).

## 3. The altitude formula above 11 km (accuracy)

Heights come from the 1976 US Standard Atmosphere's layers
(`src/atmosphere.c`): the troposphere's lapse to 11 km, isothermal to 20 km,
warming above. Above 11 km the air stops cooling, so a height there needs
the layers above the troposphere, and so does a speed. No reported height is
clamped (SNS-ALT-01, `test_SNS_ALT_01_the_reported_peak_is_not_clamped`).

## 4. AGL triggers in the pad's air (accuracy)

An AGL setting is compared with the height in the pad's air: the standard
atmosphere offset at every height by the pad's temperature (SNS-EST-05,
DD-114, `air_height_m()`). From pads at sea level, 1000 m and 2000 m in the
standard atmosphere, mains set for 150, 300 and 600 m fire within 3 % of their
height (`test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad`);
from a pad 5 K colder than the standard at sea level and one 32 K warmer at
2000 m, within 1 % and 1 m
(`test_SNS_EST_05_an_agl_trigger_acts_at_its_height_in_air_off_the_standard`).
With `standard_atmosphere` set, a day 15 K from standard is about 5 % out, as
on every barometric altimeter.
