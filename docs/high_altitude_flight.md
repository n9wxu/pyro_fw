# High-Altitude Flight: What The Bench Found, And The Options

Status: open, a decision for the user. 2026-09-29.

The bench flight (DD-078, `POST /api/sim/flight`) flies a profile through
the whole flight software on a real board, and the same profiles fly on the
host (`test/test_closedloop.c`, `fly_profile()`). The first flights above
9 km found four things. One is fixed; one is a safety limit that needs a
design decision; two are accuracy notes.

## 1. The descent ladder read thin air as a failed drogue (fixed, DD-079)

A drogue that settles at 25 m/s over the pad falls at about 200 m/s at
30 km. The emergency ladder (FLT-EMRG-01) judged that against 35 m/s and put
the main out at 29 km, four seconds after apogee. Rates are now judged in the
pad's air (FLT-AIR-01, `pp_air_scale()`), and a drogue that is really failing
is still caught at 30 km (`test_FLT_AIR_01_a_failed_drogue_is_seen_at_30_km`).

## 2. The Mach lock does not release above its envelope (open)

`docs/mach_lockout.md` sizes the lockout for flights to about 9 km, with
σ = 1.2 Pa. Above that, the release's "decelerating" test cannot be passed
reliably, the lock holds to the end of the coast, and apogee comes from the
fallback: fitted ṗ > 0 and p > p_flag, where p_flag is the pressure when
the flag went up, a second or so off the pad. **The drogue then fires 100 to
190 m above the pad, and on a real flight the rocket has fallen ballistic
from apogee to there.**

### Evidence

On the host, profiles from `flight_sim.h` (boost 3 s, 4 s from 20 km),
through `fly_profile()` with Gaussian sensor noise, several seeds each:

| Apogee | σ = 1 to 1.2 Pa (datasheet) | σ = 3 Pa (MK1C measured) |
|---|---|---|
| 9-14 km | on time, 5 of 5 (1.2 Pa) | on time or 1-3 s late, 5 of 5 |
| 15 km | on time, 3 of 3 (1 Pa) | fallback in 2 of 3, 154-161 m AGL |
| 18-22 km | on time; fallback in 1 of 3 at 22 km | fallback, 3 of 3, 119-193 m AGL |
| 25 km | fallback in 1 of 3, 170 m AGL | fallback, 3 of 3, 2-170 m AGL |
| 30 km | fallback, 3 of 3, 152 m AGL | fallback, 3 of 3, 96-152 m AGL |

On MK1C-SD (serial 02373331FF2A), noise-free profiles (the bench replaces
the reading, so its only noise is the 1 Pa quantisation):

- 10 km: released at 35.5 s, apogee on time.
- 30 km, first flight: never released; drogue and main together at 150 m,
  646.7 s into a 674 s flight.
- 30 km, second flight: released at 69.9 s, 12 s before apogee.

### Why

The release asks every fit for one second to show p̈ ≥ 0.0009 p, 0.58 g
of deceleration. One g of deceleration is ρg² of p̈, which falls with the
air, while the fit's p̈ noise is 4.34 Pa/s² per 1.2 Pa of sensor noise at
any height:

| Height | 1 g of p̈ | the 0.58 g floor, in noise σ at 1.2 Pa | at 3 Pa |
|---|---|---|---|
| 9 km | 44.8 Pa/s² | 6.0 | 2.4 |
| 15 km | 18.6 | 2.5 | 1.0 |
| 20 km | 8.5 | 1.1 | 0.45 |
| 30 km | 1.7 | 0.23 | 0.09 |

Below about one σ, fifty fits in a row all passing is luck.

### Options for the release

- **A. Lengthen the fit for the release as the pressure falls.** p̈'s noise
  goes as T^-2.5 at a fixed rate: a 4 s window has 1/32 of a 1 s window's.
  At 30 km and 3 Pa that puts the floor 3σ clear. Costs: the history must
  hold 4 s (`PP_HIST_SIZE` 256, +1 kB), the fit runs over 200 samples (its
  cost on MK1B's M0+ to be measured), and the release comes 3 s later,
  harmless in a coast that lasts tens of seconds up there. The speed and
  apogee fits keep their second. Recommended: the physics of the test stays
  the same, and only its averaging grows with need.
- **B. Drop the deceleration test where it cannot be judged.** Release on
  "slow and climbing" alone once p̈'s noise exceeds the floor. The test is
  there to refuse a rocket still under thrust, and a sustainer lit high is
  exactly such a rocket; this gives that protection up where it matters.
- **C. Release on the clock.** From the flag, the coast cannot outlast
  v/g; release when the time a ballistic coast from the flag's speed would
  take has passed. Needs the flag's speed, which is what the lock distrusts.
- **D. Keep the envelope and say so.** Refuse, on the pad, a configuration
  that expects more than 9 km (a new "expected apogee" setting), and document
  the limit. Honest, and it flies nobody high.

### Options for the fallback

The fallback's p > p_flag guards against a supersonic port faking a descent.
It is also what puts the drogue at 150 m when the lock is held. Whatever the
release becomes:

- **E. A descent held longer.** Locked, fitted ṗ > 0 on clean fits for N s
  (say 3 s), and at least as long since burnout as a shock could last, with
  no p > p_flag. A port faking descent does not do so smoothly for 3 s of
  clean fits.
- **F. Keep p > p_flag** and rely on A to make the fallback rare.

A with E is the pair that fixes both ends. Neither is started: the lockout is
flight-critical, has its own derivation, and changing it is the user's call.

## 3. The altitude formula above 11 km (accuracy)

`pp_pressure_to_height_cm()` is the troposphere's formula,
44330 (1 - (p/p0)^(1/5.2561)). Above 11 km the air stops cooling and the
formula reads short: 25.3 km at a true 30 km. Deployment triggers are low,
and apogee is found from the pressure minimum, so no decision moves; the
reported altitude is also clamped at 8000 m by SNS-ALT-02. DD-079 takes the
formula's error out of the descent rates. A reported apogee above 11 km would
need the stratosphere's layers.

## 4. A high pad reads its AGL triggers about 3 % low (accuracy)

The same formula assumes 288.15 K at the pad. From a 1500 m pad, where the
standard gives 278.4 K, a main set for 300 m fires at 290 m
(`test_FLT_AIR_01_a_high_pad_flies_20_km`).
