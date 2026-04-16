---
date: 2026-04-06T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 442a2f1ceb4af3876d74430a66fd9476d19a0a6f
branch: main
repository: pyro_fw
topic: "Continuous ground level calibration during PAD_IDLE"
tags: [research, codebase, pressure_processing, flight_states, altitude, calibration, pad_idle]
status: complete
last_updated: 2026-04-06
last_updated_by: Joseph Julicher
---

# Research: Continuous Ground Level Calibration During PAD_IDLE

**Date**: 2026-04-06  
**Researcher**: Joseph Julicher  
**Git Commit**: `442a2f1ceb4af3876d74430a66fd9476d19a0a6f`  
**Branch**: main  
**Repository**: pyro_fw

## Research Question

We need to add a continuous ground level calibration while in pad idle to prevent extended time on the pad from having an incorrect altitude. Currently atmospheric pressure changes cause the ground level to drift up. The new calibration system must slowly follow the change in atmospheric pressure until the flight starts.

## Summary

Ground pressure is established **once** at boot during `BOOT_CALIBRATE` by averaging 10 raw sensor samples and is never updated again. Once the FSM enters `PAD_IDLE`, the value is frozen in both `pp.ground_pressure` (inside `pressure_processing.c`) and `ctx->ground_pressure` (inside `flight_context_t`). Any slow atmospheric drift after that point will cause altitude readings to accumulate error — appearing as a non-zero (positive or negative) altitude while the rocket is stationary on the pad. The altitude used for launch detection (`LAUNCH_ALT_CM = 1000 cm = 10 m`) and all telemetry is computed relative to this frozen reference.

The IIR filter (`τ = 500 ms`) only smooths the raw pressure reading; it does not update the ground reference. So a slow pressure change over many minutes fully passes through the IIR and drives the altitude output away from zero.

## Detailed Findings

### 1. Where Ground Pressure Is Set (and Not Updated)

**One-time boot calibration** in `pressure_processing.c:117–149`:

```c
void pp_start_cal(void) {
    pp.cal_sum = 0;
    pp.cal_count = 0;
    pp.state = PP_CALIBRATING;
}
```

Called from `action_cal_init()` at `flight_states.c:400–403`. While in `PP_CALIBRATING` state, each call to `pp_feed()` accumulates raw Pa into `pp.cal_sum`. After exactly `PP_CAL_SAMPLES = 10` samples (`pressure_processing.h:38`), the average is written and the state flips to `PP_RUNNING`:

```c
pp.ground_pressure   = (int32_t)(pp.cal_sum / PP_CAL_SAMPLES);
pp.filtered_pressure = pp.ground_pressure;   // primes the IIR
pp.filter_initialized = true;
pp.state = PP_RUNNING;
```

After this, `pp.ground_pressure` is **never written again** in the firmware. No function in `pressure_processing.c` updates it outside of calibration.

**Copy in flight context** at `flight_states.c:405–412` (`action_ground_cal()`):

```c
ctx->ground_pressure   = pp_ground_pressure();
ctx->filtered_pressure = ctx->ground_pressure;
```

`ctx->ground_pressure` is likewise never updated during `PAD_IDLE`, `ASCENT`, `DESCENT`, or `LANDED`.

### 2. Where Ground Pressure Is Used

**Altitude calculation** — `pressure_processing.c:77–88` (`pp_pressure_to_altitude_cm()`):

```c
float ratio  = (float)pressure_pa / (float)ground_pressure_pa;
float alt_m  = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
```

`ground_pressure_pa` is always `pp.ground_pressure` — the frozen boot value. This function is called inside `pp_feed()` on every running sample at `pressure_processing.c:161`:

```c
int32_t alt_cm = pp_pressure_to_altitude_cm(filtered, pp.ground_pressure);
ring_push(alt_cm, timestamp_ms);
```

**Launch detection** — `flight_states.c:229–263` (`detect_pad_idle()`):

- `LAUNCH_ALT_CM = 1000` (10 m AGL) and `LAUNCH_SPEED_CMS = 500` (5 m/s) are the thresholds.
- The altitude used is the IIR-filtered pressure converted to cm relative to `pp.ground_pressure`.
- If ground pressure has drifted high (atmospheric pressure dropped), the displayed altitude will be positive even at rest, eroding the 10 m launch threshold margin.

**Telemetry** — `telemetry_formatter.c:64–67`:
- `alt_cm` in `$PYRO` sentence is sourced directly from the altitude ring buffer output — also relative to `pp.ground_pressure`.
- `press_pa` in `$PYRO` is `ctx->filtered_pressure` (the raw IIR Pa output, not altitude).

**Flight log header** — `flight_states.c:559`:
```c
"# Ground Pa: %ld\n", (long)ctx->ground_pressure
```

### 3. IIR Filter Behavior and Its Relationship to the Problem

The IIR filter is in `pressure_processing.c:53–66`:

```c
int32_t diff  = raw_pressure - pp.filtered_pressure;
int32_t alpha = (dt_ms * 1000) / (500 + dt_ms);   // τ = 500 ms
int32_t step  = (diff * alpha) / 1000;
```

At 50 Hz (`dt_ms = 20`), `alpha = 20000 / 520 ≈ 38` out of 1000, so each sample advances the filter ~3.8% toward the new value. The filter time constant is 500 ms — it fully follows pressure changes that are slower than a few seconds. This means slow atmospheric drift (minutes to hours) passes through the filter without attenuation and arrives at `pp_pressure_to_altitude_cm()` unchanged.

The filter **does not protect against drift**; it only attenuates noise and vibration. The root cause is that `ground_pressure` is a fixed reference, not a slow-following baseline.

### 4. PAD_IDLE Detector — Full Per-Tick Flow

`detect_pad_idle()` at `flight_states.c:232–264`:

1. Serial ground-test command processing (`hal_serial_readline()` → `ground_test_handle_command()`).
2. Continuity check and buzzer update every 1000 ms.
3. `pp_read(&sample)` — drain one altitude sample from the ring buffer.
4. Compute `pad_speed_cms = (altitude - last_altitude) * 1000 / dt_ms`.
5. `buf_add(0, altitude_cm)` — store in ring buffer with flight-time = 0.
6. Update `ctx->last_altitude`, `ctx->last_sample`.
7. Launch check: return `SEVT_LAUNCH` if `altitude > 1000 cm && pad_speed_cms > 500`.

The per-tick gating is `now - ctx->last_sample < 10` (line 240) — samples arriving faster than 10 ms are skipped (but drained from the ring). The effective altitude update rate in PAD_IDLE is therefore ≤100 Hz, practically matching the 50 Hz sensor rate.

### 5. Pressure Processing Public API

`pressure_processing.h` exposes these functions relevant to a ground-tracking update:

| Function | Signature | Purpose |
|---|---|---|
| `pp_init()` | `void pp_init(void)` | Reset everything to idle |
| `pp_start_cal()` | `void pp_start_cal(void)` | Begin calibration (10-sample average) |
| `pp_cal_done()` | `bool pp_cal_done(void)` | True when 10 samples accumulated |
| `pp_ground_pressure()` | `int32_t pp_ground_pressure(void)` | Read current `pp.ground_pressure` |
| `pp_feed()` | `void pp_feed(int32_t pa, uint32_t ms)` | Push raw sensor reading |
| `pp_read()` | `bool pp_read(altitude_sample_t *)` | Pop one altitude sample |
| `pp_last_filtered_pa()` | `int32_t pp_last_filtered_pa(void)` | Last IIR output in Pa |
| `pp_filter_pressure()` | `int32_t pp_filter_pressure(int32_t, uint32_t)` | Exposed for unit testing |
| `pp_pressure_to_altitude_cm()` | `int32_t pp_pressure_to_altitude_cm(int32_t, int32_t)` | Exposed for unit testing |
| `pp_test_prime()` | `void pp_test_prime(int32_t)` | Test bypass — sets ground pressure directly |

There is no API to update `ground_pressure` incrementally without a full recalibration cycle. A new function would need to be added.

### 6. State Machine Constraints

- `pp.ground_pressure` is written only inside `pp_start_cal()` / `pp_feed()` during `PP_CALIBRATING`.
- `ctx->ground_pressure` is written only inside `action_ground_cal()`, which fires on the `BOOT_CALIBRATE → PAD_IDLE` transition.
- The transition from `PAD_IDLE` to `ASCENT` fires `action_launch()` (`flight_states.c:416–432`), which does **not** touch ground pressure.
- Any new ground-tracking logic in PAD_IDLE must stop before the `SEVT_LAUNCH` transition fires.

## Code References

- `src/pressure_processing.c:20–42` — static `pp` struct (all pressure pipeline state)
- `src/pressure_processing.c:53–66` — IIR filter implementation (`τ = 500 ms`)
- `src/pressure_processing.c:77–88` — `pp_pressure_to_altitude_cm()` — hypsometric formula
- `src/pressure_processing.c:117–149` — `pp_start_cal()` and calibration accumulation in `pp_feed()`
- `src/pressure_processing.c:155–168` — `pp_feed()` running path (calls filter then altitude)
- `src/pressure_processing.h:28–36` — `altitude_sample_t`, `PP_RING_SIZE`, `PP_CAL_SAMPLES`
- `src/flight_states.c:229–230` — `LAUNCH_ALT_CM`, `LAUNCH_SPEED_CMS` thresholds
- `src/flight_states.c:232–264` — `detect_pad_idle()` full implementation
- `src/flight_states.c:400–412` — `action_cal_init()` and `action_ground_cal()`
- `src/flight_states.c:477–485` — state transition table
- `src/flight_states.h:86–142` — `flight_context_t` struct
- `src/hal_hardware.c:125–132` — `pres_append()` → `pp_feed()` call site
- `src/hal_hardware.c:240–245` — 50 Hz sensor init
- `src/telemetry_formatter.c:64–67` — `$PYRO` sentence fields
- `test/test_flight_states.c:180` — `pp_test_prime()` usage pattern

## Architecture Documentation

The pressure pipeline is a strict one-way data path:

```
Sensor (50 Hz)
  → pres_append() [hal_hardware.c]
    → pp_feed() [pressure_processing.c]
      → IIR filter (τ=500ms)
        → pp_pressure_to_altitude_cm(filtered_pa, ground_pressure)  ← ground_pressure FIXED
          → ring_push(altitude_cm, timestamp_ms)
            → pp_read() consumed by detect_pad_idle() / detect_ascent() / etc.
```

`pp.ground_pressure` sits outside this data path — it is only read (never written) during `PP_RUNNING`. It is set once during `PP_CALIBRATING` and then frozen. The altitude output is therefore AGL-relative to a single snapshot taken at power-on.

The IIR filter operates on **absolute Pa** (not altitude), so slow pressure drift passes through fully. There is no feedback path from the altitude output back to `ground_pressure`.

## Key Quantities for Implementation

| Quantity | Value | Location |
|---|---|---|
| Calibration sample count | 10 samples | `pressure_processing.h:38` |
| Sensor rate | 50 Hz | `hal_hardware.c:241` |
| IIR time constant | 500 ms | `pressure_processing.c:58` |
| Launch altitude threshold | 1000 cm (10 m) | `flight_states.c:229` |
| Launch speed threshold | 500 cm/s (5 m/s) | `flight_states.c:230` |
| Ring buffer size | 32 samples (~640 ms at 50 Hz) | `pressure_processing.h:35` |
| Altitude ceiling clamp | 800,000 cm (8000 m) | `pressure_processing.c:70` |

## Open Questions

- What drift rate (Pa/min) is acceptable before launch detection margin is materially impacted? This determines how fast the ground-tracking filter must respond.
- Should the new ground-tracking only run during `PAD_IDLE`, or also during `BOOT_CALIBRATE`?
- Should `ctx->ground_pressure` (the copy in flight context) also be updated, or only `pp.ground_pressure`? The flight log header writes `ctx->ground_pressure` — should the log record the value at the moment of launch or the initial boot value?
- Should the ground-tracking stop cleanly when `SEVT_LAUNCH` fires, or is it sufficient that `action_launch()` doesn't touch ground pressure?
