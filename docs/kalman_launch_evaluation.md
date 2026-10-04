# Raw pressure into one Kalman filter: pad tracking and liftoff

An evaluation asked for in the requirements review on 2026-10-02: can the
launch detector take its height and speed from a Kalman filter fed raw
readings, with no filter in front of it, while the same filter tracks the
ambient pressure on the pad. Host simulation only; nothing on a board.
The scripts are `sim/study/kf_launch.py` and `sim/study/kf_extra.py`.

## What was simulated

- One filter, state p, ṗ, p̈, a white-jerk process, fixed steady-state gain,
  50 readings a second.
- The launch rule is FLT-LAUNCH-07's, in pressure: 366 Pa below the
  reference (100 ft at a sea-level pad) and falling faster than 60 Pa/s
  (5 m/s), together for 100 ms.
- The ground reference is the filter's own pressure from 3 s earlier, frozen
  at detection. No separate average.
- Three ways of feeding it: **raw**; **limited**, raw with each innovation
  held to 6 of its own standard deviations; and **median**, the present
  median of three in front.
- Three tunings, slow to fast: position gain 0.16, 0.30 and 0.54.
- Synthetic data: white sensor noise of 1.2, 3 and 5 Pa, a weather ramp,
  gusts of 30 Pa rms, and launches at a constant 1.5 to 100 g net.

Limits: the noise is synthetic, the pad is at sea level, thrust is constant,
and the present firmware chain was not run for comparison.

## Results

**The pad.** Two hours with 30 Pa gusts gave no false launch at any tuning
or noise. The height condition is what holds: the gusts alone drive the
estimated speed far past 5 m/s. On a quiet pad at 3 Pa the speed's noise is
0.4, 1.1 and 3.4 m/s rms for the slow, middle and fast tunings.

**The reference.** The delayed copy was within 6 Pa of the true pad
pressure at every launch, about half a metre. It follows the weather with
no separate filter. A launch slower than about 0.7 g net takes more than
3 s to reach 100 ft and would drag it.

**Bad readings on the pad**, each 12 kPa low unless stated:

| Feed | Slow, middle tuning | Fast tuning |
|---|---|---|
| Raw | one reading of 3 to 6 kPa declares a launch | one reading needs 60 kPa; five in a row at 12 kPa |
| Median of three | one is stopped; two in a row of 3 kPa declare a launch | five in a row |
| Limited innovation | none up to 60 kPa for one or two; eight in a row | four in a row |

**Launches**, 3 Pa noise, 100 seeds each. Delay is after the true crossing
of 100 ft; 100 ms of it is the rule's own hold.

| Feed, tuning | 1.5 g | 10 g | 30 g | 100 g |
|---|---|---|---|---|
| Raw, middle | 89 ms | 92 | 85 | 111 |
| Raw, fast | 91 | 92 | 85 | 91 |
| Limited, slow | 89 | 179 | 439 | 608 |
| Limited, middle | 89 | 92 | 85 | 220 |
| Limited, fast | 91 | 92 | 85 | 91 |
| Median, middle | 110 | 112 | 106 | 131 |

Every launch was detected. With the true noise at twice the assumed value
the limited filter's delays did not change.

**T+0.** Solved back from the state at detection, as liftoff from rest: a
mean error of about 20 ms, but worst cases of 180 ms at the middle tuning
and 570 ms at the fast one on a slow launch. Not good enough as it stands.

## Findings

1. **One filter can do both jobs.** It tracks the pad and gives the launch
   detector its height and speed, with a reference taken from its own
   state. The low-pass filter and the rolling mean are not needed for this.
2. **Raw, with nothing else, is not safe for launch.** At the tunings that
   are quiet on the pad, a single reading 3 to 6 kPa low declares a launch.
   That is the defect DD-040 fixed (N24). Only the fast tuning shrugs it off,
   and it is the noisiest on the pad.
3. **Limiting the innovation inside the filter does better than the median
   in front of it.** One or two bad readings of any size do nothing; it
   takes 80 to 160 ms of consecutive bad data, which is a run, and a run is
   data. It is one comparison in the update, not another filter, and it
   costs no delay where the median costs 20 ms.
4. **The limit has a price at slow tunings.** It slews, so a slow filter
   falls behind a violent launch: 0.6 s late at 100 g. The fast tuning is
   unaffected to 100 g. So the pad and boost want a fast filter.
5. **One tuning does not serve the whole flight.** Fast for the pad and
   boost, where the signal is huge; slow and model-led near apogee in thin
   air. With the state in height and speed that shift happens through the
   measurement's slope, which this pressure-state test does not exercise.
6. **T+0 needs its own method**, such as the first reading past a small
   height looked up in a short history, as FLT-LAUNCH-03 does now.

## Recommendation

Raw readings into one Kalman filter, with its innovation limited, a fast
tuning on the pad and through boost, the reference taken from its own
delayed state, and no median or low-pass in front. To carry into the
modelling study: the same test on recorded pad and flight data from each
board, the height-and-speed form of the state, the hand-over of tuning from
boost to coast, and T+0.
