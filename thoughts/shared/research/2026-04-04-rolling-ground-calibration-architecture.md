---
date: 2026-04-04T21:37:59+0000
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "Rolling ground calibration updated until flight — existing architecture map"
tags: [research, codebase, pressure_processing, flight_states, calibration, ground_pressure, config, pad_idle]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
---

# Research: Rolling Ground Calibration — Existing Architecture Map

**Date**: 2026-04-04T21:37:59+0000
**Researcher**: Joseph Julicher
**Git Commit**: 111637357a337d93f1c1f3f3953d3e908eff2717
**Branch**: main
**Repository**: pyro_fw

## Research Question

We will need to create a new ground level calibration that is updated up until flight. A rocket can sit on the launch pad for many minutes and the altitude can drift and create an erroneous apogee.

## Summary

The current ground calibration is a **one-shot average of 10 raw samples (~200ms)** taken during `BOOT_CALIBRATE`, approximately 2.7 seconds after power-on. The result is frozen in `pp.ground_pressure` and never updated while the device is in `PAD_IDLE`. Every layer above `pressure_processing.c` — altitude computation, launch detection, CSV logging — derives its reference from this single frozen value.

This document maps every location that reads, writes, or depends on `ground_pressure` so the full scope of a rolling-calibration change is visible.

---

## Detailed Findings

### 1. The `pp` (pressure_processing) Module — Where Ground Pressure Lives

**File**: `src/pressure_processing.c`, `src/pressure_processing.h`

The single authoritative instance of `ground_pressure` is inside the module-private `pp` struct:

```c
// pressure_processing.c:20-42
static struct {
    pp_state_t state;          // PP_IDLE | PP_CALIBRATING | PP_RUNNING

    /* Calibration */
    int64_t cal_sum;
    int cal_count;
    int32_t ground_pressure;   // ← the frozen reference

    /* IIR filter state */
    int32_t filtered_pressure;
    bool filter_initialized;
    uint32_t last_timestamp;
    ...
} pp;
```

**State machine** (`pp_state_t`): three values only
- `PP_IDLE` — discards every incoming sample
- `PP_CALIBRATING` — accumulates raw samples into `cal_sum`/`cal_count`
- `PP_RUNNING` — applies IIR filter then converts to altitude

There is **no `PP_RECAL` or `PP_LOCKED` state** in the current design.

#### Current Calibration Flow

```c
// pressure_processing.c:117-149
void pp_start_cal(void) {
    pp.cal_sum = 0;
    pp.cal_count = 0;
    pp.state = PP_CALIBRATING;
}

// inside pp_feed(), PP_CALIBRATING branch:
pp.cal_sum += raw_pressure_pa;
pp.cal_count++;
if (pp.cal_count >= PP_CAL_SAMPLES) {            // PP_CAL_SAMPLES = 10
    pp.ground_pressure = (int32_t)(pp.cal_sum / PP_CAL_SAMPLES);
    pp.filtered_pressure = pp.ground_pressure;   // prime IIR to ground
    pp.filter_initialized = true;
    pp.last_timestamp = timestamp_ms;
    pp.state = PP_RUNNING;
    // ← transitions to PP_RUNNING; pp_start_cal() never called again
}
```

After this transition **no code path in the firmware calls `pp_start_cal()` again**. Grep confirms `pp_start_cal` appears in exactly one call site (`flight_states.c:402`).

#### Public API for ground_pressure

```c
// pressure_processing.h
void    pp_start_cal(void);           // resets accumulator, enters PP_CALIBRATING
bool    pp_cal_done(void);            // true iff state == PP_RUNNING
int32_t pp_ground_pressure(void);     // returns pp.ground_pressure (read-only)
void    pp_test_prime(int32_t pa);    // test-only: set ground_pressure + skip cal
```

**There is no setter** for `pp.ground_pressure` in the public API (other than the test-only `pp_test_prime`).

#### Where ground_pressure is consumed within pp

```c
// pressure_processing.c:161 — used on every sample once PP_RUNNING
int32_t alt_cm = pp_pressure_to_altitude_cm(filtered, pp.ground_pressure);
```

```c
// pressure_processing.c:77-88 — the hypsometric formula
int32_t pp_pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    float ratio = (float)pressure_pa / (float)ground_pressure_pa;
    float alt_m = 44330.0f * (1.0f - powf(ratio, 1.0f / 5.2561f));
    int32_t alt_cm = (int32_t)(alt_m * 100.0f);
    if (alt_cm > MAX_ALTITUDE_CM) alt_cm = MAX_ALTITUDE_CM;
    if (alt_cm < 0) alt_cm = 0;   // ← zero-clamp; no negative altitude
    return alt_cm;
}
```

Changing `pp.ground_pressure` takes effect **immediately** on the next call to `pp_feed()`.

---

### 2. `flight_context_t` — The Shadow Copy in the Flight Layer

**File**: `src/flight_states.h:91`

```c
typedef struct flight_context_t {
    ...
    int32_t ground_pressure;    // ← mirrors pp.ground_pressure at cal-done time
    ...
} flight_context_t;
```

This field is set **exactly once** in `action_ground_cal` when `BOOT_CALIBRATE → PAD_IDLE`:

```c
// flight_states.c:405-413
static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->ground_pressure = pp_ground_pressure();  // copy from pp
    ctx->filtered_pressure = ctx->ground_pressure;
}
```

After this, `ctx->ground_pressure` is:
1. **Reported in the flight CSV header** (`action_launch` → `hal_log_start`, `flight_states.c:429`)
2. **Passed to `hal_log_start`** which writes it to the CSV file (`hal_hardware.c:686,704`)
3. **Written to the legacy ring-buffer CSV** (`flight_save_csv`, `flight_states.c:566`)
4. **NOT re-read from `pp_ground_pressure()`** during `PAD_IDLE` — it is only a snapshot

`ctx->ground_pressure` does **not feed back** into altitude calculation — that comes exclusively through `pp_feed()` → `pp_pressure_to_altitude_cm(filtered, pp.ground_pressure)`.

---

### 3. How `detect_pad_idle` Reads Altitude During `PAD_IDLE`

**File**: `src/flight_states.c:232-264`

```c
static state_event_t detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    // ... ground test, rate gate, continuity check ...

    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;               // no new sample yet

    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;

    uint32_t dt = (ctx->last_sample > 0) ? (ts - ctx->last_sample) : 10;
    if (dt > 0)
        ctx->pad_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    ctx->filtered_pressure = pp_last_filtered_pa();
    buf_add(ctx, 0, ctx->filtered_pressure, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = ts;

    bool alt_ok   = altitude > LAUNCH_ALT_CM;       // 1000 cm = 10 m
    bool speed_ok = ctx->pad_speed_cms > LAUNCH_SPEED_CMS; // 500 cm/s = 5 m/s
    return (alt_ok && speed_ok) ? SEVT_LAUNCH : SEVT_NONE;
}
```

**Key observation**: `altitude` here comes from the pp ring buffer (`pp_read`), which was produced by `pp_feed()` using `pp.ground_pressure` as the denominator. If `pp.ground_pressure` were updated to track the current IIR-filtered pressure, `altitude` would continuously re-center toward 0. The launch detection thresholds (`alt_ok`, `speed_ok`) operate on these altitude values.

`detect_pad_idle` has **no timer** for triggering a recalibration — it runs every 10ms by the sample-rate gate (`now - ctx->last_sample < 10`).

---

### 4. Launch Detection Thresholds That Depend on Ground-Relative Altitude

**File**: `src/flight_states.c:229-231`

```c
#define LAUNCH_ALT_CM   1000   // 10 m
#define LAUNCH_SPEED_CMS 500   // 5 m/s
```

Both conditions must be true simultaneously (dual-gate). The comment explicitly names barometric drift as the threat these thresholds defend against:

```c
/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-RATE-01, DD-016]
 * Strengthened launch confirmation requires ALL of:
 *   1. Filtered altitude > 10m (1000cm)
 *   2. Vertical speed > 5 m/s (500 cm/s)
 * This prevents false launch from barometric drift or thermal expansion. */
```

A rolling calibration that keeps `pp.ground_pressure` close to the current IIR-filtered pressure would reduce the altitude seen by this gate, relaxing the drift concern. The speed gate would remain the critical guard against false launches during the calibration update itself.

---

### 5. Where `ctx->ground_pressure` Is Written to Disk

Three write paths, all in the context of a flight record:

| Location | When | Content |
|----------|------|---------|
| `hal_log_start` (`flight_states.c:429`) | `action_launch` — at launch detection | CSV header `# Ground Pa: <value>` |
| `flight_save_csv` (`flight_states.c:566`) | HTTP `/api/flight.csv` ring-buffer dump | CSV header `# Ground Pa: <value>` |
| `hal_sim.c:285,301` | Simulation `hal_log_start` | same CSV header |

If `ctx->ground_pressure` is kept in sync with rolling `pp.ground_pressure` updates, the CSV header will record the **last** updated ground reference before launch — the value in effect at the moment the rocket left the pad.

---

### 6. Config Fields — What Exists and What's Available for a New Parameter

**File**: `src/config_fields.h` — the single source of truth for all config fields.

```c
#define CONFIG_FIELDS(X)                                \
    X(STR,   id,              "id",              "PYRO001")   \
    X(STR,   name,            "name",            "My Rocket") \
    X(MODE,  pyro1_mode,      "pyro1_mode",      PYRO_MODE_DELAY) \
    X(U16,   pyro1_value,     "pyro1_value",     0)           \
    X(MODE,  pyro2_mode,      "pyro2_mode",      PYRO_MODE_AGL)  \
    X(U16,   pyro2_value,     "pyro2_value",     300)         \
    X(UNITS, units,           "units",           1)           \
    X(U8,    beep_mode,       "beep_mode",       0)           \
    X(U8,    max_coast_s,     "max_coast_s",     30)          \
    X(U8,    telem_format,    "telem_format",    0)           \
    X(U8,    telem_rate_hz,   "telem_rate_hz",   10)          \
    X(U8,    log_rate_hz,     "log_rate_hz",     50)          \
    X(BOOL,  log_enabled,     "log_enabled",     true)        \
    X(BOOL,  buzzer_startup,  "buzzer_startup",  true)        \
    X(U8,    backup_timer,    "backup_timer",    30)          \
    X(U8,    landing_timeout, "landing_timeout", 60)
```

**No field related to calibration exists.** Adding a new field requires one line in `config_fields.h`; the X-macro system automatically propagates it to the struct, INI parser, serializer, and defaults.

---

### 7. Tests That Touch Calibration and PAD_IDLE Ground Reference

**File**: `test/test_flight_states.c`

| Test | Line | What It Does |
|------|------|-------------|
| `test_FLT_BOOT_08_calibrates_ground` | 113 | Verifies `ctx.ground_pressure` within 100 Pa of mock after `boot_to_pad_idle` |
| `test_FLT_BOOT_10_no_false_launch_on_drift` | 622 | 200 Pa step after calibration must NOT trigger launch — IIR filter absorption test |
| `test_PAD_IDLE_noise_no_false_launch` | 663 | ±25 Pa alternating noise at 50 Hz must NOT trigger launch — altitude gate test |
| `test_FLT_LAUNCH_02_stays_on_ground` | 159 | Flat pressure = stays PAD_IDLE |
| `test_FLT_LAUNCH_01_detects_ascent` | 174 | 150 Pa drop (>1000 cm + speed) triggers launch — uses `pp_test_prime` |
| `test_SNS_PRES_04_single_data_path` | 702 | IIR filter smoothness over 100 steps at 50 Hz |
| `test_FLT_BOOT_08_calibrates_ground` | 113 | `ctx.ground_pressure` ≈ mock pressure after full boot sequence |

**Pattern in all PAD_IDLE / ASCENT / DESCENT tests**: `ctx.ground_pressure` is set directly (`ctx.ground_pressure = 101325`) and `pp_test_prime(101325)` is called to prime the pp layer. This pattern means these tests are **not** testing a rolling calibration scenario.

The `boot_to_pad_idle` helper (`test_flight_states.c:18`) runs the full boot sequence:
```c
static void boot_to_pad_idle(flight_context_t *ctx) {
    /* BOOT_SETTLE (2600ms) → BOOT_CONTINUITY → BOOT_CALIBRATE (10 × 110ms) */
    ...
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx->current_state);
}
```

---

### 8. The Test Infrastructure Mock for Pressure

**File**: `test/mocks.h`, `test/hal_test.c`

```c
typedef struct {
    float pressure_pa;
    float temperature_c;
    int sensor_type;
} mock_pressure_t;
extern mock_pressure_t mock_pressure;
```

Mock pressure is a single global value; the test sets it and `hal_tasks_tick` → `pres_tick` → `pres_append` → `pp_feed` propagates it. Changing `mock_pressure.pressure_pa` between steps simulates drift or step changes. This is the mechanism used by `test_FLT_BOOT_10_no_false_launch_on_drift` to simulate post-calibration drift.

---

### 9. `pp_test_prime` — The Test-Only Bypass

**File**: `src/pressure_processing.c:111-115`

```c
void pp_test_prime(int32_t ground_pressure_pa) {
    pp.state = PP_RUNNING;
    pp.ground_pressure = ground_pressure_pa;
    pp.filter_initialized = false; /* first pp_feed will prime the IIR */
}
```

This is the only non-`pp_start_cal` path that can write `pp.ground_pressure`. It is documented as test-only and is guarded by the `pressure_processing.h` comment "Test helper: prime pp to PP_RUNNING with given ground pressure." It is the pattern that shows what a `pp_update_ground(pa)` function would look like in the implementation layer.

---

## Code References

| File | Lines | Description |
|------|-------|-------------|
| `src/pressure_processing.h:38` | 38 | `PP_CAL_SAMPLES = 10` |
| `src/pressure_processing.c:20-42` | 20-42 | `pp` struct — `ground_pressure` lives here |
| `src/pressure_processing.c:53-66` | 53-66 | IIR filter (τ=500ms, minimum-step anti-stall) |
| `src/pressure_processing.c:77-88` | 77-88 | `pp_pressure_to_altitude_cm` — hypsometric + zero-clamp |
| `src/pressure_processing.c:111-115` | 111-115 | `pp_test_prime` — the only non-boot setter for `pp.ground_pressure` |
| `src/pressure_processing.c:117-149` | 117-149 | `pp_start_cal` + calibration accumulation in `pp_feed` |
| `src/pressure_processing.c:151-167` | 151-167 | `PP_RUNNING` branch in `pp_feed` — uses `pp.ground_pressure` |
| `src/flight_states.h:91` | 91 | `ctx->ground_pressure` shadow copy in `flight_context_t` |
| `src/flight_states.c:400-413` | 400-413 | `action_ground_cal` — the only place `ctx->ground_pressure` is set from `pp_ground_pressure()` |
| `src/flight_states.c:224-231` | 224-231 | Launch detection thresholds — `LAUNCH_ALT_CM=1000`, `LAUNCH_SPEED_CMS=500` |
| `src/flight_states.c:232-264` | 232-264 | `detect_pad_idle` — reads altitude from pp ring, computes speed, checks launch |
| `src/flight_states.c:402` | 402 | **Only call site** of `pp_start_cal()` — in `action_cal_init` |
| `src/flight_states.c:429` | 429 | `hal_log_start(&ctx->config, ctx->ground_pressure)` — frozen at launch |
| `src/flight_states.c:566` | 566 | `flight_save_csv` writes `ctx->ground_pressure` to CSV header |
| `src/config_fields.h:25-41` | 25-41 | All 16 config fields — none related to calibration |
| `test/test_flight_states.c:605-654` | 605-654 | `test_FLT_BOOT_10_no_false_launch_on_drift` — 200 Pa post-cal step test |
| `test/test_flight_states.c:663-690` | 663-690 | `test_PAD_IDLE_noise_no_false_launch` — ±25 Pa noise test |

---

## Architecture Documentation

### Full `ground_pressure` Data Flow (current state)

```
Power on
    ↓
BOOT_SETTLE (2500ms)
    ↓
BOOT_CONTINUITY → action_cal_init → pp_start_cal()  [pp.state = PP_CALIBRATING]
    ↓
BOOT_CALIBRATE:  10 × pp_feed() → pp.ground_pressure = avg(10 raw samples)
                                   pp.filtered_pressure = pp.ground_pressure  ← IIR primed
                                   pp.state = PP_RUNNING
    ↓
action_ground_cal → ctx->ground_pressure = pp_ground_pressure()   ← shadow copy
    ↓
PAD_IDLE (indefinite):
    every ~20ms:  pp_feed(raw_pa) → IIR filter → pp_pressure_to_altitude_cm(filtered, pp.ground_pressure)
                                   → ring buffer → detect_pad_idle reads via pp_read()
    ← pp.ground_pressure NEVER CHANGES during PAD_IDLE ←
    ↓
SEVT_LAUNCH → action_launch → hal_log_start(ctx->config, ctx->ground_pressure)
    ↓
ASCENT / DESCENT / LANDED (flight)
```

### pp Layer Internal State Transitions

```
PP_IDLE ──pp_start_cal()──► PP_CALIBRATING ──10 samples──► PP_RUNNING
                                                              │
                                                              │ stays here forever
                                                              ◄─────────────────┘
```

### Two Separate `ground_pressure` Variables

| Variable | Location | Updated when | Used for |
|----------|----------|-------------|----------|
| `pp.ground_pressure` | `pressure_processing.c` (private) | Once during `PP_CALIBRATING` | Computing altitude on every sample |
| `ctx->ground_pressure` | `flight_states.h` (in `flight_context_t`) | Once in `action_ground_cal` | CSV header at launch, CSV export |

These two variables are set to the same value at boot and never diverge afterward in the current code.

### Config Field Addition Pattern

Adding a new `U8` config field named `recal_interval_s` with a default of `10`:
```c
// config_fields.h — one line addition:
X(U8, recal_interval_s, "recal_interval_s", 10)
```
This automatically generates: struct member `cfg.recal_interval_s`, INI parser key `"recal_interval_s"`, serializer output, and default value. No changes needed to `config.c`, `config.h`, or any parsing code.

---

## Related Research

- [2026-04-04-altitude-drift-upward.md](2026-04-04-altitude-drift-upward.md) — Quantitative analysis of the drift mechanism with live device data (uptime=3000s, pressure=101697 Pa, altitude=22ft). Contains the numeric derivation showing 81 Pa change ≈ 22ft of drift at 1.6 Pa/min.
- [2026-04-04-test-network-http-staleness.md](2026-04-04-test-network-http-staleness.md) — HTTP test staleness analysis; confirms `hal_config_load` writes defaults on first boot.

## Open Questions

- What timer granularity and window size should the rolling calibration use (e.g., IIR update vs. re-accumulate N samples every M seconds)?
- Should `ctx->ground_pressure` be kept in sync with rolling updates so the CSV header reflects the last ground reference at launch?
- How does the rolling calibration interact with `test_FLT_BOOT_10_no_false_launch_on_drift`? That test simulates a 200 Pa step and expects PAD_IDLE to be maintained — with rolling calibration, the step would eventually be absorbed, which is correct behavior.
- Should the rolling update freeze when the device detects it has been moved (speed > threshold) to avoid absorbing a pre-launch vertical transport?
