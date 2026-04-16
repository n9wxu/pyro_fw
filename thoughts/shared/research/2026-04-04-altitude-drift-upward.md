---
date: 2026-04-04T21:31:08+0000
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "Why altitude drifts upward with long uptime"
tags: [research, codebase, pressure_processing, flight_states, altitude, calibration, barometric-drift]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
---

# Research: Why altitude drifts upward with long uptime

**Date**: 2026-04-04T21:31:08+0000
**Researcher**: Joseph Julicher
**Git Commit**: 111637357a337d93f1c1f3f3953d3e908eff2717
**Branch**: main
**Repository**: pyro_fw

## Research Question

Why is altitude drifting upward? The uptime is 3000s, altitude is 22ft, pressure is 101697 Pa. The unit is live on pyro.local.

## Summary

The altitude drift is caused by **one-shot ground calibration** combined with **zero-clamping** of negative altitude. The device establishes `ground_pressure` from 10 raw samples (~200ms of data) during `BOOT_CALIBRATE` approximately 2.7 seconds after power-on. This reference is never updated. Any subsequent change in absolute barometric pressure is indistinguishable from physical altitude change.

At 3000s uptime with the reported values (pressure 101697 Pa, altitude 22ft ≈ 671cm), back-calculating yields an implied ground pressure at calibration time of ~101,778 Pa — a drop of ~81 Pa over 50 minutes. This 1.6 Pa/min drift rate is consistent with normal atmospheric variation (weather fronts, diurnal pressure cycles).

The zero-clamp in `pp_pressure_to_altitude_cm` creates **asymmetric** drift behavior: pressure that decreases since boot (lower pressure = higher apparent altitude) accumulates directly. Pressure that increases since boot would yield negative altitude, which is clamped to 0. This is why you observe the device "drifting up" rather than oscillating near zero.

The codebase explicitly acknowledges barometric drift as a known phenomenon — the launch detection threshold (`LAUNCH_ALT_CM = 1000` = 10m, `LAUNCH_SPEED_CMS = 500` = 5 m/s) is specifically designed to prevent false launches from this drift (`flight_states.c:224-228`).

## Detailed Findings

### 1. Full Altitude Data Path

```
MS5607/BMP280 @ 50Hz
    ↓  pres_tick() [hal_hardware.c:163]
    ↓  pres_append() → pp_feed(raw_pa, timestamp_ms) [hal_hardware.c:125-132]
    ↓  pp_feed() IIR filter (τ=500ms) → pp_pressure_to_altitude_cm() [pressure_processing.c:131-168]
    ↓  ring buffer (32 entries)
    ↓  detect_pad_idle() reads pp_read() [flight_states.c:245-248]
    ↓  ctx->last_altitude
    ↓  update_status() → g_status.altitude_cm [main_hardware.c:34]
    ↓  /api/status JSON "alt_cm" field [http_server.c:164]
```

`g_status.pressure_pa` in the API is the **IIR-filtered** pressure, not raw:
- Set via `ctx->filtered_pressure = pp_last_filtered_pa()` in `detect_pad_idle()` (`flight_states.c:256`)
- Then `g_status.pressure_pa = ctx->filtered_pressure` in `update_status()` (`main_hardware.c:37`)

### 2. Calibration — One-Shot at Boot (~2.7s)

`flight_states.c:190-193` — `detect_boot_calibrate`:
```c
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    return pp_cal_done() ? SEVT_CAL_DONE : SEVT_NONE;
}
```

`pressure_processing.c:117-148` — `pp_start_cal` / `pp_feed` in calibration:
```c
void pp_start_cal(void) {
    pp.cal_sum = 0;
    pp.cal_count = 0;
    pp.state = PP_CALIBRATING;
}
// ... in pp_feed():
case PP_CALIBRATING:
    pp.cal_sum += raw_pressure_pa;
    pp.cal_count++;
    if (pp.cal_count >= PP_CAL_SAMPLES) {   // PP_CAL_SAMPLES = 10
        pp.ground_pressure = (int32_t)(pp.cal_sum / PP_CAL_SAMPLES);
        // ...transitions to PP_RUNNING
    }
```

- `PP_CAL_SAMPLES = 10` (`pressure_processing.h:38`)
- Sensor rate: 50 Hz (`hal_hardware.c:235`)
- Calibration window: 10 samples × 20ms = **200ms** of raw pressure averaged
- Calibration starts after 2500ms boot-settle (`detect_boot_settle`, `flight_states.c:174`)
- Total: `ground_pressure` is frozen **~2.7s after power-on**

**`ground_pressure` is never updated after this transition.** There is no re-calibration path, no timer-based update, and no mechanism for the flight software to call `pp_start_cal()` again.

### 3. Altitude Calculation and Zero-Clamp

`pressure_processing.c:77-88` — `pp_pressure_to_altitude_cm`:
```c
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    float ratio = (float)pressure_pa / (float)ground_pressure_pa;
    float alt_m = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
    int32_t alt_cm = (int32_t)(alt_m * 100.0f);
    if (alt_cm > MAX_ALTITUDE_CM)
        alt_cm = MAX_ALTITUDE_CM;
    if (alt_cm < 0)
        alt_cm = 0;          // ← zero-clamp
    return alt_cm;
}
```

The hypsometric formula `h = 44330 × (1 − (P/P₀)^(1/5.2561))`:
- When `P < P₀` (current pressure lower than calibration): `ratio < 1` → altitude **positive**, shown as drift upward
- When `P > P₀` (current pressure higher than calibration): `ratio > 1` → altitude **negative**, **clamped to 0**

This is why altitude drift is **asymmetric**: upward drift accumulates, downward drift shows as zero.

### 4. Numeric Verification Against Reported Values

Given uptime=3000s, pressure=101697 Pa, altitude=22ft (≈671cm):

Back-solving for `ground_pressure`:
```
671cm = 44330m × (1 - (101697/P₀)^(1/5.2561)) × 100
6.71/44330 = 1 - (101697/P₀)^0.190263
(101697/P₀)^0.190263 = 0.998487
ln(101697/P₀) = ln(0.998487)/0.190263 = -0.000795
P₀ = 101697 / e^(-0.000795) ≈ 101,778 Pa
```

**Pressure change**: 101,778 − 101,697 = **81 Pa decrease** since calibration
**Drift rate**: 81 Pa / 3000s = 0.027 Pa/s = **1.62 Pa/min** — well within normal atmospheric variation

### 5. Drift Acknowledged in Code — Launch Guards

`flight_states.c:224-231`:
```c
/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-RATE-01, DD-016]
 * Strengthened launch confirmation requires ALL of:
 *   1. Filtered altitude > 10m (1000cm)
 *   2. Vertical speed > 5 m/s (500 cm/s)
 * This prevents false launch from barometric drift or thermal expansion. */
#define LAUNCH_ALT_CM 1000
#define LAUNCH_SPEED_CMS 500
```

The current 22ft (671cm) drift is **below the 1000cm launch threshold**. A false launch also requires speed > 500 cm/s, which slow barometric drift cannot produce.

### 6. IIR Filter Behavior at Steady-State

`pressure_processing.c:53-66` — `pp_filter_pressure` (τ=500ms):
```c
int32_t alpha = (dt_ms * 1000) / (500 + dt_ms);
int32_t step = (diff * alpha) / 1000;
if (step == 0 && diff != 0)
    step = (diff > 0) ? 1 : -1;   // minimum step prevents stall
pp.filtered_pressure += step;
```

At steady state (dt_ms = 20ms for 50Hz), `alpha = 20000/520 ≈ 38`. For a slowly drifting pressure (1.62 Pa/min = 0.54 mPa/sample), the filter tracks the raw pressure essentially exactly — the IIR filter's τ=500ms is irrelevant at this timescale. The reported 101697 Pa filtered value accurately reflects the current atmospheric pressure.

### 7. g_status.pressure_pa vs Raw Pressure

The API reports `pressure_pa` as the IIR-filtered value:
- `main_hardware.c:37`: `g_status.pressure_pa = ctx->filtered_pressure`
- `ctx->filtered_pressure = pp_last_filtered_pa()` at `flight_states.c:256`
- At slow drift rates the filtered value is effectively identical to raw

## Code References

- `src/pressure_processing.h:38` — `PP_CAL_SAMPLES = 10`
- `src/pressure_processing.c:77-88` — `pp_pressure_to_altitude_cm` with zero-clamp at line 85
- `src/pressure_processing.c:117-148` — `pp_start_cal` and calibration accumulation in `pp_feed`
- `src/pressure_processing.c:53-66` — IIR filter, τ=500ms, minimum-step anti-stall
- `src/flight_states.c:190-194` — `detect_boot_calibrate`, polls `pp_cal_done()`
- `src/flight_states.c:224-231` — launch thresholds defending against barometric drift
- `src/flight_states.c:256` — `ctx->filtered_pressure = pp_last_filtered_pa()`
- `src/hal_hardware.c:125-132` — `pres_append` calls `pp_feed(raw_pa, timestamp_ms)`
- `src/hal_hardware.c:232-236` — `hal_pressure_init`, starts async sampling at 50 Hz
- `src/main_hardware.c:34-37` — `update_status` copies `last_altitude` and `filtered_pressure` to `g_status`

## Architecture Documentation

**Calibration lifecycle**:
- `BOOT_SETTLE` (2500ms) → `BOOT_CONTINUITY` → `BOOT_CALIBRATE` (10 samples × 20ms = 200ms) → `PAD_IDLE`
- `pp_start_cal()` called once in `action_cal_init` on `BOOT_CONTINUITY → BOOT_CALIBRATE` transition
- `pp.ground_pressure` fixed after 10 samples; state machine transitions to `PP_RUNNING`
- No path in the state machine calls `pp_start_cal()` again during `PAD_IDLE`

**Pressure pipeline**:
- 50 Hz async state machine (MS5607: 3-phase, BMP280: single-phase)
- All samples funnel through `pp_feed()` — single producer
- Flight software is sole consumer via `pp_read()` — ring buffer decouples rates
- `pp_last_filtered_pa()` provides debug/telemetry access without consuming ring entries

**Zero-clamping**: Designed for a flight computer that never goes underground. Altitude of 0 means "on the ground or below calibration pressure." Negative altitudes are not meaningful for this application.

## Related Research

- [2026-04-04-test-network-http-staleness.md](2026-04-04-test-network-http-staleness.md) — HTTP test analysis, notes that `hal_config_load` writes defaults to `config.ini` on first boot (config.ini always exists after first boot)

## Open Questions

- Whether `g_status.pressure_pa` should report raw vs filtered — currently filtered, which at low drift rates is effectively identical to raw
- How long the device has been running before calibration pressure significantly diverges from current pressure (depends on weather; normal atmospheric variation is 100–300 Pa/hour for fast-moving fronts)
