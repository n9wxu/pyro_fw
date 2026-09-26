# Prompt: Barometric Mach Lockout for a Model Rocket Pyro Controller (Raw Pressure, No Flight Parameters)

## Role

You are an experienced avionics engineer who designs flight computers for high-power model rockets. You are teaching a capable embedded developer how a Mach lockout works and helping them implement one in a barometric pyro controller. Explain the reasoning behind each decision, not only the numbers, so the developer understands why the algorithm is safe. Use complete sentences and precise engineering language.

## System context

- The flight computer has **one sensor: a barometric pressure sensor**. There is no accelerometer, gyro, GPS, or temperature input used by the flight logic.
- All flight logic operates on **raw pressure in pascals (Pa)** and its time derivatives. Altitude is computed only for logging and display, never for decisions.
- Before launch, the controller averages ambient pressure on the pad and stores it as the ground baseline **p₀**. It also measures the sensor's pressure noise **σ** (Pa) on the pad.
- The controller fires pyro channels for **apogee (drogue)** and **main** deployment.
- Operating envelope: pad temperature **10 °C to 45 °C**, launch site elevation **0 m to 2,000 m ASL**, flights up to roughly **9 km AGL**, and speeds well beyond Mach 1.
- The logic contains **no temperature term** and does not depend on site elevation.

## Design constraint: no flight parameters

The user does not enter anything about the rocket or the motor: no burn time, no expected apogee, no simulated time to apogee, no drag figure, and no filter tuning. The airframe's behavior is unknown until it has flown, and the controller must work correctly on the first flight of any rocket.

Every constant in the design must therefore be one of the following, and you must state which category each one falls into:

1. **A physical bound that holds for any rocket**, such as gravity or the speed of sound across the temperature envelope.
2. **A statistical confidence level**, expressed as a multiple of the pad-measured noise σ.
3. **A fixed design constant of the controller itself**, such as the sample rate and fit window length, which is the same for every flight.

The main deployment height is a recovery setting chosen by the flyer, not a flight parameter, and may remain configurable.

The algorithm needs to know only one thing about the flight: **whether the rocket went fast enough that the pressure data may be corrupted.**

## The problem a Mach lockout solves

Explain the following to the developer:

1. As a rocket approaches Mach 1, shock waves form on the airframe. Once the rocket is supersonic, the static ports sit in flow that has passed through a shock, so the pressure they sense is not ambient pressure.
2. **The corruption lasts for the entire time the rocket is supersonic, not only during the transonic crossings.** The error typically begins near Mach 0.85, changes abruptly as the rocket passes Mach 1 in each direction, and varies continuously with Mach number in between. Its magnitude and sign depend on airframe shape and port placement, so it cannot be modeled or subtracted out.
3. Because the error depends on Mach, both the pressure level and its derivatives are wrong throughout supersonic flight. The dynamic pressure near Mach 1 is comparable to the static pressure itself, so even a small change in the port's pressure coefficient as Mach changes produces a pressure error of several percent. During boost, that error can grow fast enough to make the rocket appear to slow down, stop, or descend.
4. A pressure rise looks to a barometric altimeter like a loss of altitude. If the apogee logic sees that, it fires the drogue while the rocket is still supersonic, which can destroy the recovery system or the airframe.
5. Two airframes break naive lockouts. A draggy rocket on a large motor goes supersonic briefly and then slows quickly, so a long fixed timer is still active at apogee. A low-drag rocket coasts supersonic for a long time, so a short fixed timer expires while the data is still corrupted. A user-entered delay fixes neither case without knowing the flight in advance.

## The idea behind the algorithm

Teach the algorithm as three rules:

1. **Flag "too fast" while the data is still clean.** The rocket's climb rate is measured reliably below Mach 0.85. If it exceeds about Mach 0.62–0.75, the rocket might go supersonic, so the flag is set before any corruption can begin.
2. **Once the flag is set, distrust the data until it proves itself.** Release the lock only after the data has shown, continuously for a fixed time, the unmistakable signature of a subsonic rocket coasting upward: a smooth pressure history, still ascending, slower than the release threshold, and decelerating at least as hard as gravity alone requires.
3. **Never let the lock prevent a deployment.** If the lock is still set when clean data shows the rocket has fallen back below the altitude where the flag was set, fire the drogue anyway.

Explain why rule 2 requires every condition together rather than any single one. A Mach-dependent error can briefly fake any one symptom. For example, it can make the climb rate look slow, or make the rocket look like it is decelerating. Faking all of them at once, smoothly, for a full second, while the motor is thrusting or the rocket is supersonic, requires the error to fit a quadratic almost perfectly and to overwhelm the true acceleration. That is far less likely than faking any single symptom. Be candid that this is a probabilistic argument, not a proof, and describe the remaining risk in the assumptions section.

Explain why rule 2 never misses apogee. A coasting rocket decelerates by at least 1 g, and near apogee drag becomes negligible. The time to coast from the release speed (at most about 200 m/s) to apogee is (v_t/g)·atan(v/v_t), where v_t is the rocket's terminal velocity. Even for a very draggy rocket with v_t = 20 m/s, this is about 3 s, and a rocket that draggy cannot reach Mach 1 in the first place. Typical supersonic rockets have 8–20 s of subsonic coast after release. The release decision takes at most about 1.5 s after the data becomes clean (one fit window plus the release window), so the controller is always unlocked before apogee. Clean data returns near Mach 0.85, above the release speed, so release latency does not eat into the coast.

## Physics: working in raw pressure

Derive and explain the hydrostatic relation:

dp/dz = −ρ·g and ρ = p / (R·T)

so the fractional pressure rate is proportional to vertical velocity:

**−ṗ / p = g·v / (R·T)**

Constants: g = 9.80665 m/s², R = 287.05 J/(kg·K), γ = 1.4.

- The comparison **−ṗ > k·p** needs no logarithm, no altitude conversion, and no temperature measurement.
- Across the envelope, local air temperature ranges from about 216 K (tropopause floor) to 318 K (45 °C pad).
- One unit of Mach corresponds to −ṗ/p = g·√(γ / (R·T)), which is **0.0384 s⁻¹ at 318 K** and **0.0465 s⁻¹ at 216 K**.
- Site elevation drops out because every threshold is relative to the current pressure p or the pad baseline p₀.

The acceleration relation is p̈/p ≈ g·a_decel / (R·T) during upward deceleration. **1 g corresponds to about 0.00105 to 0.00155 in p̈/p (s⁻²).** The lapse rate adds a small term worth at most about 0.1 g at release speeds.

Sign convention: while ascending, ṗ is negative. While a coasting rocket decelerates, −ṗ shrinks toward zero, so p̈ is positive. Be explicit about this in code and comments.

## Estimator: fixed-window quadratic fit

Do not use a Kalman filter, because its process noise must be tuned to the vehicle. Use a **fixed-window least-squares quadratic fit**:

- Fit p(t) = c₀ + c₁·t + c₂·t² to the most recent N raw samples at a fixed rate (for example, N = 50 at 100 Hz, a 0.5 s window). These are design constants of the controller and never change between flights.
- **Evaluate the fit at the newest sample**, not at the window center. The endpoint estimate has no lag bias for constant acceleration, which matters for setting the flag in time during a hard boost.
- Because the window and rate are fixed, ṗ and p̈ at the endpoint are each a dot product of the raw samples with a precomputed constant coefficient vector. Derive these coefficients, scale them to integers, and use them directly. No floating point or matrix operations are needed in flight.
- A fit is **clean** when its RMS residual is at most 2σ and no sample lies more than 4σ from the fit. Both limits are statistical and use the pad-measured σ, which is constant in Pa and therefore independent of altitude and site elevation.
- Show that the noise on each estimate is σ·√(Σcᵢ²) for its coefficient vector, and explain why raw differentiation of pressure is unusable, especially for p̈.
- Explain that a step in pressure (a shock crossing the ports, an ejection charge) makes every fit containing that step unclean for one window length. That is the desired behavior, not a limitation.

## Thresholds

Use p in Pa, ṗ in Pa/s, and p̈ in Pa/s². All integer forms fit in int32 for realistic pressures.

| Purpose | Condition | Integer form | Category and meaning |
|---|---|---|---|
| Launch detect | p̈ < −0.0025·p on clean fits for 50–100 ms | `10000*pddot < -25*p` | Physical: about 2.1–2.4 g upward |
| Minimum-altitude arm | p < 0.9965·p₀ | `10000*p < 9965*p0` | Design constant: about 30 m AGL |
| Set "too fast" flag | −ṗ > 0.029·p on a clean fit | `1000*(-pdot) > 29*p` | Physical: true Mach 0.62–0.75 across the envelope |
| Release: slow | 0 < −ṗ < 0.022·p | `pdot < 0 && 1000*(-pdot) < 22*p` | Physical: ascending, true Mach below 0.47–0.57 |
| Release: decelerating | p̈ ≥ 0.0009·p | `10000*pddot >= 9*p` | Physical: at least about 0.8 g of deceleration, which gravity guarantees for any coasting rocket and no thrusting rocket shows |
| Release: sustained | every fit clean and meeting both conditions above for 1 s continuously | — | Design constant |
| Apogee | fitted ṗ > 0 for 10 consecutive clean fits, and p ≥ p_min·1.0005 | `2000*(p - p_min) >= p_min` | Physical: 3–5 m below the recorded peak |
| Lock fallback | locked, fitted ṗ > 0 on clean fits for 1 s, and p > p_flag | — | Physical: back below the altitude where the flag was set |

Explain how each value was chosen:

- **Set flag** uses the hottest air (smallest rate per Mach), so it always fires before the corruption onset near Mach 0.85. In colder air it fires earlier, which is the safe direction. A rocket whose peak speed falls between the flag threshold and Mach 0.85 is flagged unnecessarily, but it releases normally within about 1.5 s of burnout.
- **Release: slow** also uses the hottest air, so the true Mach at release never exceeds about 0.57. It also requires the rocket to be ascending, because the release must witness an upward coast. A reading of apparent descent while locked is exactly what corrupted boost data produces.
- **Release: decelerating** is the key parameter-free check. A thrusting rocket accelerates upward. A coasting rocket decelerates by at least 1 g plus drag. The floor sits slightly below 1 g in the hottest air to allow for estimate noise. There is no upper limit, because drag is unknown before flight.
- **Lock fallback** exists only to prevent a ballistic landing if something unforeseen keeps the lock set. p_flag is the pressure recorded at the moment the flag was set. A pressure error large enough to exceed the entire altitude gained since then is not credible, so a clean, sustained descent past that level is real. This deployment will be late, but it is never early.

## State machine

1. **PAD.** Average p₀ with a slow filter and measure σ. Pyro channels disarmed.
2. **ASCENT.** Entered on launch detect. Track p_min (the lowest pressure seen on clean fits) while the flag is not set. If the flag condition is met, record p_flag and enter LOCKED. Apogee detection is active here, so a subsonic flight never locks and behaves like a plain barometric altimeter.
3. **LOCKED.** Apogee detection is disabled and p_min is not updated. The fit keeps running. Maintain a release timer that counts clean fits meeting both release conditions and resets to zero on any fit that is unclean or fails either condition. When the timer reaches 1 s, reset p_min to the current pressure and return to ASCENT with the flag cleared. If the fallback condition is met first, fire the drogue and go to DESCENT.
4. **DESCENT.** After each ejection charge, ignore pressure for 1–2 s and let fits become clean again, because the charge pressurizes the avionics bay. Fire the main when clean fits show p > p_main (the pressure ratio for the chosen main height) with sustained descent.
5. **LANDED.** Pressure stable near p₀ for several seconds. Disarm all channels.

Explain that returning from LOCKED to ASCENT with the flag cleared cannot re-lock the rocket on the way down to apogee, because the rocket has already been confirmed subsonic and decelerating. Clean fits cannot then exceed the flag threshold.

For the flight record, log raw pressure for the entire flight and mark the locked interval. Report peak altitude from p_min, which excludes the locked interval, and flag it as a lower bound if the lock released within 2 s of apogee.

## Pyro safety requirements

- No channel can fire in PAD or before the minimum-altitude arm condition is met.
- The drogue cannot fire from the apogee condition while locked. Only the fallback may fire it during lockout.
- Each channel fires once, with a defined pulse duration, and the event is logged.
- Continuity checks before flight, with the result reported to the user.
- A hardware arming switch independent of software.
- A defined response to sensor failure (stuck value, out-of-range value, loss of communication). Bad samples are treated as unclean, so a failed sensor holds the lock and never causes a deployment.

## Hardware guidance to include

- Place static ports at least 4–5 body diameters behind the nose-cone shoulder, away from rail buttons and other protrusions.
- Use three or four ports spaced evenly around the circumference so that crossflow averages out.
- Seal the avionics bay so that ports are the only path for air, and size the ports for the bay volume.
- Good port placement reduces the supersonic error but never removes the need for a lockout.

## What to deliver

1. An explanation of the problem, the physics, and the three rules, following the sections above.
2. A derivation of each threshold from first principles, with the temperature envelope worked through numerically and each constant labeled with its category.
3. The derivation of the endpoint fit coefficients for the chosen window, their integer scaling, and the noise figures for ṗ and p̈.
4. C code for a bare-metal microcontroller, with no floating point in the flight path, implementing the fit, the state machine, the lockout, and pyro gating. Keep all constants in one block with comments giving their physical meaning and category.
5. A desktop test harness that replays simulated flights and prints the state timeline and pyro events. Include at least:
   - a subsonic flight that never sets the flag;
   - a flight peaking between Mach 0.65 and 0.85 that sets the flag and releases normally;
   - a draggy airframe reaching Mach 1.5 with rapid post-burnout deceleration;
   - a low-drag airframe coasting supersonic for 10 s or more;
   - a hot 45 °C pad at 2,000 m and a cold 10 °C pad at sea level;
   - a persistent, Mach-dependent pressure error for the whole supersonic period with steps at both Mach 1 crossings, tested with both signs, including a case where the error grows during boost fast enough to make the rocket appear to slow down or descend;
   - an ejection-charge pressure pulse at apogee;
   - a sensor dropout and a stuck sensor during coast.

   For each profile, report whether the drogue fired, the true Mach and time to apogee at release, and the delay between true apogee and firing.
6. A list of assumptions and the remaining risks. In particular, quantify how large and how smooth a supersonic pressure error would have to be to fake a full second of the release signature during boost, and explain what the fallback does if that ever happens.

Do not add any user-entered flight parameter, filter tuning value, drag limit, or timer that depends on the specific rocket. If the design seems to need one, redesign that part so it relies on a physical bound, a statistical limit, or a fixed design constant instead.
