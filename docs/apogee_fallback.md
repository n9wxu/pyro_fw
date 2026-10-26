# Apogee from two filters: a simple one and the model

Prompted by a pressure-chamber flight on an MK1B-8MB that declared neither
apogee nor landing. The user's decisions, 2026-10-25:

1. Launch detection in the log must start at 0 altitude.
2. A simple filter, with no Mach lockout, that always detects apogee on simple
   flights.
3. The Kalman filter always runs as an estimator but can be taken out of the
   decision loop.
4. Even with the Kalman filter in the decision loop, it can detect that it is
   not converging and let the simple filter save the day.
5. A unit inside the body tube of a simple rocket sees a pressure spike at
   ejection.

And during the study:

- "A simple mach lockout is just watching for the measured speed to drop
  below a threshold for a few seconds."
- "The ejection pressure spike will decay instantly when the rocket
  separates. Most of the time, the pyro will live in a nose-cone or
  electronics bay and it will not get access to the ejection pressure."
- "Drop the maximum altitude to 30,000 ft. Higher will need special low noise
  sensors or GPS."

The studies are `sim/study/simple_apogee.py` (`simple`, `lockout`,
`handover`, `chamber <flight.csv>`) and `sim/study/apogee_fallback.py`
(`flights`, `chamber <flight.csv>`).

## Bottom line

- **The simple filter works on simple flights.** A running median of 3 raw
  readings (60 ms), apogee when it stands 2 m above its lowest value for
  0.5 s, declares apogee on every subsonic flight at 9, 20 and 35 Pa of noise,
  with and without an ejection spike, early on none. It is 1.0 to 1.2 s late
  (median). On the chamber flight it declares 0.6 s after the lowest reading,
  on a real supersonic flight 1.25 s after.
- **The hold is set by noise.** A 0.3 s hold rejects an ejection spike of any
  size; at 35 Pa of noise (MK1B with its buzzer and flash writes running)
  anything under 0.5 s is early on noise alone. Falling the 2 m from rest at
  the top takes 0.64 s, a floor for any setting.
- **Under 30,000 ft, clean ports never fool it.** On every sim flight to
  Mach 1.9 with clean ports the simple filter alone is early on none, with or
  without the speed lockout.
- **Port errors do.** With the sim's port-error models it is early on 318 of
  504 flights alone, and on 46 with the lockout at 50 m/s for 3 s, by at most
  2.0 s.
- **Decision 4 has a safe form:** hand over to the simple filter while the
  model filter does not explain the readings and the lockout has released
  (50 m/s for 3 s). Early on none of the sim's flights. Tests of "not
  converging" by duration alone are not safe: unexplained for 10 s hands over
  early on 16 port-error flights.
- **On the one real supersonic flight** (a J motor, a 38 mm airframe, three
  static ports) every filter here is right. Its port error, a 0.7 s stall and
  one reversed reading, is far milder than the sim's models, which are the
  only evidence behind every early count in this study.

## The ceiling and the flights

The board's height for proper operation (SNS-MAX-01) becomes 30,000 ft,
9,144 m. Above it the sensor's noise swamps the measured speed: at 30 km one
pascal is 7.7 m, and 9 Pa of noise is about 130 m/s of noise in a speed taken
over 0.5 s.

| Rocket | Apogee, cold sea-level pad | hot pad at 2,000 m | Max Mach |
|---|---|---|---|
| hop | 77 m | 81 m | 0.11 |
| subsonic | 754 m | 892 m | 0.50 |
| mid-Mach | 1,114 m | 1,380 m | 0.76 |
| 30 g | 1,341 m | 1,682 m | 0.93 |
| draggy | 1,514 m | 2,242 m | 1.50 to 1.94 |
| 30 kft (low-drag, trimmed) | 8,063 m | 8,985 m | 1.41 to 1.47 |

The flight set is DD-092's matrix on these six rockets: both pads, four port
models on the three fast ones, 1.2 and 9 Pa, eight seeds; and the port-error
scale sweep, 0.1 to 4 times, both signs, on the three fast ones. 696 flights,
684 with readings a sensor can give. "30 kft" is the low-drag rocket with
3.6 kg of propellant over 4.5 s, added to put a flight at the ceiling.

## 1. The log starts at T+0

T+0 is already the first sample of the rise more than 50 cm above the pad
(FLT-LAUNCH-03). The log starts at detection instead (`flight_action_launch`,
`src/flight_pad.c`): 100 ft and 5 m/s on the filtered state (FLT-LAUNCH-07).
The samples between are not written.

In the chamber log the first row is T+151 ms at 42 m (filtered), and its raw
reading is already about 160 m. On a slow rocket the gap is seconds.

What it needs: the samples from the start of the rise to detection, held
until detection and then written ahead of the LAUNCH row. The pressure ring
(`pp.ring`, `src/pressure_processing.c`) is a short producer-consumer queue,
not a history, so this is a buffer of its own. Its length is the longest rise
to detection the board must cover, times 50 samples a second.

## 2. The simple filter

```
m      = median of the last 3 raw readings
lowest = the lowest m since launch
apogee : m > lowest * (1 + 2 m / 8000 m), held 0.5 s, after m has been 30 m above the pad
```

A new lowest value restarts the hold. It has no state model, no Mach number
and no estimator.

Flights: the three subsonic rockets, cold and hot pads, 8 seeds, each quiet
and with a motor-ejection spike of 3, 30 or 100 kPa decaying in 20 ms, at 3,
1 or 0 s before or 1 or 3 s after the apogee of the same flight with no
ejection, the canopy out from that moment.

| Median, rise, hold | Early at 9 Pa | 20 Pa | 35 Pa | Late at 35 Pa: median | max |
|---|---|---|---|---|---|
| none, 1 m, 0.3 s | 0 | 150 | 583 | | |
| none, 2 m, 0.5 s | 0 | 16 | 89 | | |
| 60 ms, 1 m, 0.5 s | 0 | 0 | 29 | | |
| **60 ms, 2 m, 0.5 s** | **0** | **0** | **0** | **0.96 s** | **1.52 s** |
| 180 ms, 1 m, 0.5 s | 0 | 0 | 0 | 0.82 s | 1.47 s |
| 180 ms, 2 m, 0.5 s | 0 | 0 | 0 | 1.13 s | 1.76 s |
| 180 ms, 2 m, 0.3 s | 0 | 0 | 25 | | |

At 9 Pa, 60 ms, 2 m, 0.5 s is 1.19 s late (median), 1.32 s at worst. The
model filter on the same flights is 0.16 s late, and never early on a spike,
but a spike can make it late by up to 12.7 s. FLT-APO-01 allows the model
filter 0.5 s.

- **Spikes:** a hold of 0.3 s or more rejects every 20 ms spike at every size
  and time. A spike lasting longer is the board's own charge in a closed bay,
  which it knows of (SNS-EST-09): the model filter skips 1 s of readings after
  each pulse and the pyro rules bound the state to free fall for 2 s
  (PYR-MODE-06). A motor's ejection is not announced; it ends at separation.
- **The rise could follow the board's measured noise** (`pp_noise_pa()`)
  instead of a fixed 2 m, so a quiet board declares sooner. Not studied.

On the two real logs:

| Median, rise, hold | Chamber, after the lowest reading | DATA47, after the lowest reading |
|---|---|---|
| 60 ms (DATA47: one reading), 2 m, 0.5 s | 0.58 s | 1.25 s |
| 180 ms, 1 m, 0.5 s | 0.66 s | 0.83 s |
| 1 s, 2 m, 0.5 s | 1.42 s | 1.66 s |

## The speed lockout

```
speed  = the change in m over 0.5 s, as height
locked : until |speed| has been under V for H, then released for the flight
```

The simple filter declares only once released. Under the ceiling every
flight's lockout releases.

| V, H | Subsonic (96): early | Mach ≥ 0.8, clean (96): early | Mach ≥ 0.8, port errors (504): early | worst |
|---|---|---|---|---|
| no lockout | 0 | 0 | 318 | −39.1 s |
| 50 m/s, 1 s | 0 | 0 | 52 | −29.4 s |
| 50 m/s, 2 s | 0 | 0 | 47 | −28.4 s |
| **50 m/s, 3 s** | **0** | **0** | **46** | **−2.0 s** |
| 100 m/s, 2 s | 0 | 0 | 52 | −29.6 s |
| 150 m/s, 2 s | 0 | 0 | 68 | −30.8 s |

- **The 46 early at 50 m/s for 3 s** are early by 0.6 to 2.0 s: 40 on the
  draggy rocket with a port reading low, 6 on the 30 kft rocket with the
  error at 4 times. The draggy rocket slows through Mach 1 just before its
  apogee, so the error collapses into an apparent fall as the speed drops.
- **A port that reads high makes a fast climb look slow,** which is how the
  shorter holds release while the rocket is still supersonic.
- **The chamber flight:** 100 m/s for 1 s declares 1.0 s after the lowest
  reading; 50 m/s for 3 s, 21.6 s after (its fall is faster than 50 m/s
  throughout).
- **DATA47:** 1.25 s after, with either.

## 3. The model filter in or out of the decision loop

All estimators already run on every reading whichever is obeyed (SNS-EST-07),
and `estimator` in config.ini picks the one obeyed (SNS-EST-06). Decision 3 is
a setting naming what decides: the model filter, with the simple filter as its
fallback (section 4), or the simple filter alone. The model filter keeps
running and logging either way.

With the simple filter alone, the lockout table applies: no early declaration
on subsonic or clean-port flights to the ceiling; with the sim's port errors,
46 of 504 early by up to 2.0 s with the lockout. For a hand-run chamber the
simple filter alone declares 0.6 s after the top.

## 4. Handing over when the model filter does not converge

The model filter decides; when it is not converging, the simple filter's
verdict is taken. Each line is the simple filter (60 ms, 2 m, 0.5 s) taken
under one condition, over the 696 flights:

| Handover condition | Subsonic (96): early | Mach ≥ 0.8 (600): early | worst |
|---|---|---|---|
| Model unexplained for 2 s | 0 | 161 | −37.6 s |
| Model unexplained for 10 s | 0 | 16 | −27.0 s |
| Model never explained after the first 2 s | 0 | 106 | −13.6 s |
| Unexplained, climb never measured above 150 m/s | 0 | 94 | −14.5 s |
| Unexplained, speed lockout 100 m/s for 1 s released | 0 | 66 | −30.6 s |
| Unexplained, speed lockout 50 m/s for 2 s released | 0 | 1 | −28.4 s |
| **Unexplained, speed lockout 50 m/s for 3 s released** | **0** | **0** | |

And the model filter's own fall as the fallback, no simple filter
(`apogee_fallback.py flights`, 684 flights):

| Model filter's fall, breaks < 0.5 s forgiven | Early | Late median |
|---|---|---|
| held 6 s | 6 | 6.14 s |
| held 8 s | 5, all the 30 kft rocket at 4 times the port error | 8.14 s |
| held 10 s | 0 | 10.14 s |

- **Unexplained is what a port error looks like.** The longest unexplained
  run before apogee is 7.9 s on subsonic flights and 18 s on fast ones.
- **The lockout handover:** the model filter has the decision while it
  explains the readings, so the early declarations of the lockout alone (the
  draggy rocket near its apogee) do not happen; by then the model filter
  explains again.
- **On the logs:** the chamber flight hands over 1.0 s after the top with
  100 m/s for 1 s, 21.6 s after with 50 m/s for 3 s; the model's own fall held
  10 s declares 10.2 s after. On DATA47 the model filter declares 0.1 s after
  the top and keeps the decision.
- **The speed gate needs a quieter speed.** With the 60 ms median the
  measured speed is noisy enough that a 150 m/s gate hands over early on 94
  flights. A speed taken over a longer span (a fit over 2 s is about 8 times
  quieter than a difference over 0.5 s) would serve the lockout and the gate;
  the apogee rule keeps its short median.

## A real supersonic flight

`DATA47.TXT`, from another logger: a J motor, a 38 mm body tube with three
static ports. A reading every 83 ms (12 a second), in hPa to 0.01; one
reading in six repeats the one before. It starts at that logger's own launch
detection, about 20 m up. The pad pressure is taken as the landing reading,
102,361 Pa.

| Time | Height | Speed over one reading |
|---|---|---|
| 0 to 0.75 s | 20 to 262 m | 200 to 550 m/s |
| 0.8 to 1.5 s | 288 to 382 m | 60 to 290 m/s; at 1.33 s one reading rises 57 Pa (−59 m/s) |
| 1.67 to 2.5 s | 424 to 907 m | 510 to 715 m/s |
| 3 to 21 s | 1,111 to 3,259 m | a smooth coast, 615 down to 0 m/s |
| 21.05 to 21.30 s | 3,259 m | the lowest reading |
| 24 s on | | −20 to −28 m/s under a canopy |

The stall and the catch-up are the port reading high through the transonic
span. As an apparent fall it lasts one reading. The model filter did not
explain the boost (unexplained from 0.17 to 7.9 s) and its speed reached
1,227 m/s at 2.17 s. It is one flight, one airframe, at 12 readings a second;
the firmware reads 50 and the model filter is tuned for that rate.

## The chamber flight

`flight.csv`: MK1B-8MB, ground 86,986 Pa, 1,066 s, state 4 (ASCENT)
throughout; LAUNCH at 0.15 s, ARMED at 0.59 s. Climb to 3,010 m at 3.3 s
(2,700 m/s at launch), fall to the ground by 24 s, with three one-step breaks
at 9.0, 13.0 and 18.5 s. The fall goes from −28 to −340 m/s in 4.5 s, about
7 g downward; no falling body does more than 1 g. Neither estimator explained
it after 0.19 s (`explains=0` in the log and in `test/estimator_replay.c`).
Today's firmware, which declares apogee only on an explained estimate
(FLT-APO-07), declared none, and so no landing.

Also in this log: ARMED at 0.59 s, when the raw readings say the board was
climbing at about 1,900 m/s. Arming reads the model filter's speed whether or
not it explains the readings (FLT-ASC-04).

## Decisions to take

1. **How much weight the sim's port models carry.** Every early count above
   comes from them; the one real supersonic flight has a much milder error.
2. **Simple filter settings:** 60 ms, 2 m, 0.5 s (safe to 35 Pa), whether the
   rise follows the measured noise, and its lateness requirement beside
   FLT-APO-01's 0.5 s.
3. **The lockout:** 50 m/s for 3 s, and whether its speed comes from a longer
   span than the apogee rule's median.
4. **The handover for decision 4:** unexplained with the lockout released,
   the model's own fall held 10 s, or both. With 50 m/s for 3 s the chamber
   flight is handed over 21.6 s after the top; the faster 100 m/s for 1 s
   hands it over 1.0 s after but is early on 66 port-error flights.
5. **The log buffer for T+0:** how many seconds of rise it must hold.
6. **Arming:** whether it reads the simple filter's speed, or waits on the
   model explaining the readings.
7. **Requirements:** SNS-MAX-01's 30,000 ft; FLT-APO-01 and FLT-APO-07 for
   two filters; FLT-APO-08's peak under the simple filter; SNS-EST-06 for the
   decision setting; the log's start (DAT-02, FLT-LAUNCH-03).
