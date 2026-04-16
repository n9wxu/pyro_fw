# Telemetry v1.2 Compliance Implementation Plan

## Overview

Update `pyro_fw` firmware telemetry and state machine to comply with ground-station-interface-spec Revision 1.2. The spec split the single `DESCENT` state into three states (FALLING=2, DROGUE=3, CHUTE=4), moved LANDED from code 3 to code 5, and clarified that apogee is an event (not a state). All test files must be updated to match.

## Current State Analysis

**Spec v1.1 state codes** (what firmware currently emits):
- 0=PAD, 1=ASCENT, 2=DESCENT, 3=LANDED

**Spec v1.2 state codes** (target):
- 0=PAD, 1=ASCENT, 2=FALLING, 3=DROGUE, 4=CHUTE, 5=LANDED

The firmware has one internal `DESCENT` state. `state_to_telem_id()` maps `DESCENT→2` and `LANDED→3`. The entire descent phase (free-fall, drogue, main chute) collapses into a single detector function (`detect_descent()`) that fires pyros and detects landing.

Key files:
- `src/flight_states.h` — `flight_state_t` enum, `state_event_t` enum
- `src/flight_states.c` — `state_to_telem_id()`, detectors[], transitions[], detector functions, `flight_update_outputs()` rate gating
- `test/test_flight_states.c` — unit tests (39 tests)
- `test/test_integration.c` — integration tests referencing `DESCENT`
- `test/test_closedloop.c` — closed-loop tests referencing `DESCENT`

### Key Discoveries

- `detect_descent()` (`flight_states.c:366-416`) does three things: reads altitude/speed, calls `try_fire_pyros()`, and detects landing. This logic must be redistributed across three new detector functions.
- `try_fire_pyros()` (`flight_states.c:92-109`) fires both channels independently each call based on mode conditions. Both channels can fire in the same call if both conditions are met.
- `action_apogee()` (`flight_states.c:485-495`) sets `apogee_detected=true`, emits `$PYRO_APO`, and immediately calls `try_fire_pyros()` — this fires drogue (pyro1) at apogee if PYRO_MODE_FALLEN is configured.
- Rate gating (`flight_states.c:711`): `ASCENT || DESCENT → 100ms`. Must include all three new descent states.
- `test_FLT_APO_01_detects_apogee` (`test_flight_states.c:284`) asserts `ctx.current_state == DESCENT` — must change to `FALLING`.
- `test_FLT_LAND_01_detects_landing` (`test_flight_states.c:292`) starts with `ctx.current_state = DESCENT` — must change to `CHUTE_DESCENT`.
- `test_TEL_07_state_mapping` (`test_flight_states.c:391`) tests PAD→0, ASCENT→1 — must add FALLING→2, DROGUE_DESCENT→3, CHUTE_DESCENT→4, LANDED→5.
- `test_TEL_06_altitude_and_speed` (`test_flight_states.c:423`) passes `DESCENT`, asserts `st == 2` — must update to use `FALLING` (code still 2).
- `test_integration.c:234,242` — checks `ctx.current_state == DESCENT` and asserts `saw_descent` — must change to check any of the three new states.
- `test_closedloop.c:404` — `run_flight()` helper records apogee when `ctx.current_state == DESCENT` — must change to `FALLING`.

## Desired End State

The firmware emits state codes 0–5 per spec v1.2. The state machine has three distinct descent states. All tests pass. No existing behavior changes for PAD, ASCENT, or LANDED phases.

**Verification**: `cmake --build build --target host_tests integration_tests closedloop_tests` passes with zero failures.

## What We're NOT Doing

- Not wiring `batt` or `temp` fields (spec says 0 is acceptable; tracked as separate gap)
- Not implementing extended flags (bits 6-11) — spec marks them optional
- Not changing pyro firing logic or modes — only the state wrappers change
- Not adding skip-state transitions for "no drogue configured" edge case (follow-up work)
- Not changing JSON telemetry variants — they mirror the NMEA changes automatically
- Not touching `src/telemetry_formatter.c` — `state_to_telem_id()` bridges the enum change transparently

## Implementation Approach

Replace the single `DESCENT` enum value with three new values (`FALLING`, `DROGUE_DESCENT`, `CHUTE_DESCENT`). Add two new state events to drive the new transitions. Redistribute `detect_descent()` logic across three new detector functions. Update `state_to_telem_id()`, the detectors array, the transitions table, and the rate-gating condition. Update all test references.

---

## Phase 1: Update `flight_state_t` and `state_event_t` in `flight_states.h`

### Overview

Replace `DESCENT` with three new enum values and add two new events. This is the foundational change — everything else depends on these new names.

### Changes Required

#### 1.1 `src/flight_states.h` — `flight_state_t` enum

**File**: `src/flight_states.h:11-20`

Replace:
```c
typedef enum {
    BOOT_SETTLE = 0,
    BOOT_CONTINUITY,
    BOOT_CALIBRATE,
    PAD_IDLE,
    ASCENT,
    DESCENT,
    LANDED,
    STATE_COUNT
} flight_state_t;
```

With:
```c
typedef enum {
    BOOT_SETTLE = 0,
    BOOT_CONTINUITY,
    BOOT_CALIBRATE,
    PAD_IDLE,
    ASCENT,
    FALLING,         /* free-fall before drogue fires */
    DROGUE_DESCENT,  /* drogue deployed, before main chute fires */
    CHUTE_DESCENT,   /* main chute deployed, descending to landing */
    LANDED,
    STATE_COUNT
} flight_state_t;
```

#### 1.2 `src/flight_states.h` — `state_event_t` enum

**File**: `src/flight_states.h:23-32`

Add `SEVT_DROGUE` and `SEVT_CHUTE` after `SEVT_APOGEE`:
```c
typedef enum {
    SEVT_NONE = 0,
    SEVT_DONE,
    SEVT_TIMER,
    SEVT_CAL_DONE,
    SEVT_LAUNCH,
    SEVT_ARMED,
    SEVT_APOGEE,
    SEVT_DROGUE,   /* pyro1 (drogue) fired — transition FALLING→DROGUE_DESCENT */
    SEVT_CHUTE,    /* pyro2 (main) fired — transition DROGUE_DESCENT→CHUTE_DESCENT */
    SEVT_LANDING,
} state_event_t;
```

### Success Criteria

#### Automated Verification
- [x] Project compiles (will fail until Phase 2 fixes all references): `cmake --build build 2>&1 | grep -c "error:" | xargs -I{} test {} -eq 0`

---

## Phase 2: Update `flight_states.c`

### Overview

Four sub-tasks: update `state_to_telem_id()`, add three new detector functions (splitting `detect_descent()`), update the detectors array and transitions table, and update rate gating.

### Changes Required

#### 2.1 `src/flight_states.c` — `state_to_telem_id()` at line 691

**File**: `src/flight_states.c:691-704`

Replace entire function:
```c
static uint8_t state_to_telem_id(flight_state_t state) {
    switch (state) {
    case PAD_IDLE:
        return 0;
    case ASCENT:
        return 1;
    case FALLING:
        return 2;
    case DROGUE_DESCENT:
        return 3;
    case CHUTE_DESCENT:
        return 4;
    case LANDED:
        return 5;
    default:
        return 0;
    }
}
```

#### 2.2 `src/flight_states.c` — Replace `detect_descent()` with three new detector functions

**File**: `src/flight_states.c:366-416` (current `detect_descent()`)

Replace the single `detect_descent()` function with three functions. The existing body splits as follows:
- **detect_falling()**: reads altitude/speed, calls `try_fire_pyros()`, all fault/verify/refire checks. Transitions on pyro1 fired.
- **detect_drogue_descent()**: reads altitude/speed, calls `try_fire_pyros()` (for main chute). Transitions on pyro2 fired.
- **detect_chute_descent()**: reads altitude/speed only. Contains the landing detection logic (copied verbatim from current `detect_descent()`).

```c
/* [FLT-APO→FALL] Free-fall phase: fires drogue (pyro1), transitions on pyro1_fired */
static state_event_t detect_falling(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;
    uint32_t dt = ts - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, FALLING);
    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    check_refire(ctx, now);

    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    if (ctx->pyro1_fired)
        return SEVT_DROGUE;
    return SEVT_NONE;
}

/* [FALL→DROGUE] Drogue descent: fires main chute (pyro2), transitions on pyro2_fired */
static state_event_t detect_drogue_descent(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;
    uint32_t dt = ts - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, DROGUE_DESCENT);
    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    check_refire(ctx, now);

    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    if (ctx->pyro2_fired)
        return SEVT_CHUTE;
    return SEVT_NONE;
}

/* [DROGUE→CHUTE] Main-chute descent: landing detection only, no more pyro firing */
#define LANDING_SPEED_CMS 500

static state_event_t detect_chute_descent(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;
    uint32_t dt = ts - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, CHUTE_DESCENT);

    /* Normal landing: stable altitude + low speed + near ground */
    bool altitude_stable = abs(altitude - ctx->last_altitude) < 100;
    bool speed_low = abs(ctx->vertical_speed_cms) < 200;
    bool near_ground = altitude < 3000;

    if (altitude_stable && speed_low && near_ground) {
        if (ctx->landing_stable_since == 0)
            ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            ctx->last_altitude = altitude;
            ctx->last_sample = now;
            return SEVT_LANDING;
        }
    } else {
        ctx->landing_stable_since = 0;
    }

    /* [DD-015] Landing timeout */
    uint32_t timeout_s = ctx->config.landing_timeout;
    if (timeout_s > 0 && ctx->descent_start_time > 0 && (now - ctx->descent_start_time) >= timeout_s * 1000 &&
        abs(ctx->vertical_speed_cms) < LANDING_SPEED_CMS) {
        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return SEVT_LANDING;
    }

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return SEVT_NONE;
}
```

Also remove the `#define LANDING_SPEED_CMS 500` that currently precedes `detect_descent()` at line 364 (it moves into the new function above).

#### 2.7 `src/flight_states.c` — Remove `try_fire_pyros()` from `action_apogee()`

**File**: `src/flight_states.c:485-495`

`action_apogee()` currently calls `try_fire_pyros()` before the state transitions. With the new model, pyro firing happens in `detect_falling()` on the first tick after entering `FALLING`. Remove the call:

```c
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = now;
    ctx->descent_start_time = now; /* [DD-015] start landing timeout */
    buf_tag_event(ctx, EVT_APOGEE);
    telemetry_apogee(ctx->max_altitude, now - ctx->launch_time);
    /* pyro firing handled by detect_falling() on first tick in FALLING state */
}
```

#### 2.3 `src/flight_states.c` — detectors array at line 510

**File**: `src/flight_states.c:510-518`

Replace:
```c
static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_SETTLE] = detect_boot_settle,
    [BOOT_CONTINUITY] = detect_boot_continuity,
    [BOOT_CALIBRATE] = detect_boot_calibrate,
    [PAD_IDLE] = detect_pad_idle,
    [ASCENT] = detect_ascent,
    [DESCENT] = detect_descent,
    [LANDED] = detect_landed,
};
```

With:
```c
static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_SETTLE]      = detect_boot_settle,
    [BOOT_CONTINUITY]  = detect_boot_continuity,
    [BOOT_CALIBRATE]   = detect_boot_calibrate,
    [PAD_IDLE]         = detect_pad_idle,
    [ASCENT]           = detect_ascent,
    [FALLING]          = detect_falling,
    [DROGUE_DESCENT]   = detect_drogue_descent,
    [CHUTE_DESCENT]    = detect_chute_descent,
    [LANDED]           = detect_landed,
};
```

#### 2.4 `src/flight_states.c` — transitions table at line 520

**File**: `src/flight_states.c:520-528`

Replace:
```c
static const transition_t transitions[] = {
    {BOOT_SETTLE,    SEVT_TIMER,   BOOT_CONTINUITY, NULL},
    {BOOT_CONTINUITY,SEVT_DONE,    BOOT_CALIBRATE,  action_cal_init},
    {BOOT_CALIBRATE, SEVT_CAL_DONE,PAD_IDLE,        action_ground_cal},
    {PAD_IDLE,       SEVT_LAUNCH,  ASCENT,          action_launch},
    {ASCENT,         SEVT_ARMED,   ASCENT,          action_armed},
    {ASCENT,         SEVT_APOGEE,  DESCENT,         action_apogee},
    {DESCENT,        SEVT_LANDING, LANDED,          action_landing},
};
```

With:
```c
static const transition_t transitions[] = {
    {BOOT_SETTLE,     SEVT_TIMER,   BOOT_CONTINUITY, NULL},
    {BOOT_CONTINUITY, SEVT_DONE,    BOOT_CALIBRATE,  action_cal_init},
    {BOOT_CALIBRATE,  SEVT_CAL_DONE,PAD_IDLE,        action_ground_cal},
    {PAD_IDLE,        SEVT_LAUNCH,  ASCENT,          action_launch},
    {ASCENT,          SEVT_ARMED,   ASCENT,          action_armed},
    {ASCENT,          SEVT_APOGEE,  FALLING,         action_apogee},
    {FALLING,         SEVT_DROGUE,  DROGUE_DESCENT,  NULL},
    {DROGUE_DESCENT,  SEVT_CHUTE,   CHUTE_DESCENT,   NULL},
    {CHUTE_DESCENT,   SEVT_LANDING, LANDED,          action_landing},
};
```

#### 2.5 `src/flight_states.c` — rate gating in `flight_update_outputs()` at line 711

**File**: `src/flight_states.c:711`

Replace:
```c
uint32_t interval = (ctx->current_state == ASCENT || ctx->current_state == DESCENT) ? 100 : 1000;
```

With:
```c
bool high_rate_state = (ctx->current_state == ASCENT ||
                        ctx->current_state == FALLING ||
                        ctx->current_state == DROGUE_DESCENT ||
                        ctx->current_state == CHUTE_DESCENT);
uint32_t interval = high_rate_state ? 100 : 1000;
```

#### 2.6 Check `buf_add()` calls for `DESCENT` references

Search `src/flight_states.c` for any remaining `DESCENT` references outside the deleted function and update:
- In `action_apogee()` or any other action that might pass `DESCENT` as a state argument — update to `FALLING`.
- Verify no other code paths reference the removed `DESCENT` enum value.

### Success Criteria

#### Automated Verification
- [x] Firmware compiles for RP2040 target: `cmake --build build`
- [x] Host tests compile (still failing tests expected until Phase 3): `cmake --build build --target host_tests`

---

## Phase 3: Update `test/test_flight_states.c`

### Overview

Seven specific test functions need changes. Four have state machine assertions or starting states that reference the removed `DESCENT` enum, and one needs new cases added for the new state codes.

### Changes Required

#### 3.1 `test_FLT_APO_01_detects_apogee` — line 284

**File**: `test/test_flight_states.c:284`

The apogee transition now lands in `FALLING` (not `DESCENT`):

```c
// Before:
TEST_ASSERT_EQUAL(DESCENT, ctx.current_state);

// After:
TEST_ASSERT_EQUAL(FALLING, ctx.current_state);
```

#### 3.2 `test_FLT_LAND_01_detects_landing` — line 292

**File**: `test/test_flight_states.c:292`

Landing detection now lives in `detect_chute_descent()`. Set starting state to `CHUTE_DESCENT`:

```c
// Before:
ctx.current_state = DESCENT;

// After:
ctx.current_state = CHUTE_DESCENT;
ctx.pyro1_fired = true;  /* drogue already deployed */
ctx.pyro2_fired = true;  /* main already deployed */
```

The `pyro_fired` flags are set because in `CHUTE_DESCENT` both pyros have already fired. This doesn't affect the landing detection logic itself but keeps the context consistent.

#### 3.3 `test_TEL_07_state_mapping` — line 391

**File**: `test/test_flight_states.c:391-403`

Extend with new state code assertions for all 4 new/changed states:

```c
void test_TEL_07_state_mapping(void) {
    flight_context_t ctx = {0};

    /* PAD_IDLE → state_id 0 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "PYRO,0,0,") != NULL);

    /* ASCENT → state_id 1 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, ASCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",1,") != NULL);

    /* FALLING → state_id 2 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, FALLING);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",2,") != NULL);

    /* DROGUE_DESCENT → state_id 3 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, DROGUE_DESCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",3,") != NULL);

    /* CHUTE_DESCENT → state_id 4 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, CHUTE_DESCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",4,") != NULL);

    /* LANDED → state_id 5 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, LANDED);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",5,") != NULL);
}
```

#### 3.4 `test_TEL_06_altitude_and_speed` — line 416

**File**: `test/test_flight_states.c:423,436`

`DESCENT` is removed; use `FALLING` (which maps to state_id 2, keeping the `st == 2` assertion valid):

```c
// Before:
send_telemetry(&ctx, 5000, 25000, DESCENT);
// ...
TEST_ASSERT_EQUAL(2, st);

// After:
send_telemetry(&ctx, 5000, 25000, FALLING);
// ...
TEST_ASSERT_EQUAL(2, st);
```

#### 3.5 `test_TEL_07_boot_maps_to_zero` — line 477 (verify no DESCENT reference)

Search this test and any others for `DESCENT`. If found, update to the appropriate new state.

#### 3.6 Add new tests: descent state transitions

Add the following tests after `test_FLT_APO_01_detects_apogee`:

```c
/* Verify pyro1 fire → FALLING→DROGUE_DESCENT transition */
void test_FLT_DROGUE_01_transitions_on_pyro1_fire(void) {
    flight_context_t ctx = {0};
    ctx.current_state = FALLING;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.apogee_detected = true;
    ctx.pyro1_continuity_good = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 5000;
    ctx.vertical_speed_cms = -500;
    ctx.launch_time = 0;
    ctx.last_sample = 0;

    /* pyro1 fires (simulated by setting flag directly) */
    ctx.pyro1_fired = true;

    mock_pressure.pressure_pa = 101325.0f - 600.0f; /* ~5000 cm */
    mock_time_ms = 100;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(DROGUE_DESCENT, ctx.current_state);
}

/* Verify pyro2 fire → DROGUE_DESCENT→CHUTE_DESCENT transition */
void test_FLT_CHUTE_01_transitions_on_pyro2_fire(void) {
    flight_context_t ctx = {0};
    ctx.current_state = DROGUE_DESCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.apogee_detected = true;
    ctx.pyro1_fired = true;
    ctx.pyro2_continuity_good = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 3000;
    ctx.vertical_speed_cms = -300;
    ctx.launch_time = 0;
    ctx.last_sample = 0;

    /* pyro2 fires */
    ctx.pyro2_fired = true;

    mock_pressure.pressure_pa = 101325.0f - 360.0f; /* ~3000 cm */
    mock_time_ms = 100;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(CHUTE_DESCENT, ctx.current_state);
}
```

#### 3.7 Register new tests in `main()`

**File**: `test/test_flight_states.c` — `main()` function (lines 731-796)

Add after the `RUN_TEST(test_FLT_APO_01_detects_apogee)` line:
```c
RUN_TEST(test_FLT_DROGUE_01_transitions_on_pyro1_fire);
RUN_TEST(test_FLT_CHUTE_01_transitions_on_pyro2_fire);
```

### Success Criteria

#### Automated Verification
- [x] All unit tests pass: `cmake --build build --target host_tests && ./build/host_tests`

---

## Phase 4: Update `test/test_integration.c` and `test/test_closedloop.c`

### Overview

Both test helpers check for `ctx.current_state == DESCENT` to track that the rocket reached the descent phase. Since `DESCENT` no longer exists, these must check for any of the three new descent states.

### Changes Required

#### 4.1 `test/test_integration.c` — `DESCENT` references at lines 234 and 242

**File**: `test/test_integration.c:228-243`

The flight state loop checks for `DESCENT` to set `saw_descent`:

```c
// Before:
if (ctx.current_state == DESCENT)
    saw_descent = true;
// ...
TEST_ASSERT_TRUE_MESSAGE(saw_descent, "Never in DESCENT");

// After:
if (ctx.current_state == FALLING ||
    ctx.current_state == DROGUE_DESCENT ||
    ctx.current_state == CHUTE_DESCENT)
    saw_descent = true;
// ...
TEST_ASSERT_TRUE_MESSAGE(saw_descent, "Never in FALLING/DROGUE/CHUTE");
```

Also update the second instance at line 360 (if present — the same pattern in a different test):
```c
// Before:
if (ctx.current_state == DESCENT && descent_start == 0)

// After:
if ((ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT ||
     ctx.current_state == CHUTE_DESCENT) && descent_start == 0)
```

#### 4.2 `test/test_closedloop.c` — `DESCENT` references at lines 404, 898

**File**: `test/test_closedloop.c:404` — in `run_flight()` helper

This records apogee timing when the state first becomes `DESCENT`. Change to check for `FALLING` (the first descent sub-state):

```c
// Before:
if (ctx.current_state == DESCENT && !res.reached_descent) {

// After:
if (ctx.current_state == FALLING && !res.reached_descent) {
```

**File**: `test/test_closedloop.c:898`

Same pattern — check for `FALLING`:
```c
// Before:
if (ctx.current_state == DESCENT)

// After:
if (ctx.current_state == FALLING ||
    ctx.current_state == DROGUE_DESCENT ||
    ctx.current_state == CHUTE_DESCENT)
```

Also check `test/test_closedloop.c:501` — the assertion message references `DESCENT`:
```c
// Before:
snprintf(m, sizeof(m), "%s: no DESCENT", l);

// After:
snprintf(m, sizeof(m), "%s: no FALLING/DROGUE/CHUTE", l);
```

And `reached_descent` struct field and assertions at lines 501-503 — update the message only, not the boolean flag name.

#### 4.3 Verify all `DESCENT` references are gone

After changes, run:
```bash
grep -rn "\bDESCENT\b" src/ test/
```
Expected output: zero matches (the enum value no longer exists).

### Success Criteria

#### Automated Verification
- [x] All tests pass: `cmake --build build --target host_tests integration_tests closedloop_tests`
- [x] No remaining `DESCENT` references: `grep -rn "\bDESCENT\b" src/ test/ | wc -l` returns 0

---

## Testing Strategy

### Unit Tests (`test/test_flight_states.c`)
- Two new tests: `test_FLT_DROGUE_01` and `test_FLT_CHUTE_01` cover the new state transitions
- Existing apogee and landing tests updated to use correct states
- Telemetry mapping test extended to cover all 6 state codes

### Integration Tests (`test/test_integration.c`)
- Full flight CSV simulation through all phases — `saw_descent` flag now satisfied by any of the three new descent states

### Closed-Loop Tests (`test/test_closedloop.c`)
- Physics engine tests track apogee at `FALLING` entry (same timing as before, just renamed)
- Landing assertions unchanged (`LANDED` state is unmodified)

---

## Edge Cases and Follow-up Work

1. **Pyro1 never fires**: If `pyro1_continuity_good` is false and pyro1 is disabled, `detect_falling()` never returns `SEVT_DROGUE` and the rocket stays in `FALLING` forever. Follow-up: add a skip condition in `detect_falling()` such as `if (!ctx->pyro1_continuity_good && ctx->pyro2_fired) return SEVT_DROGUE`.

2. **`action_apogee()` no longer calls `try_fire_pyros()`**: The firmware enters `FALLING` before attempting to fire pyros. `detect_falling()` fires on the first tick in `FALLING` and calls `try_fire_pyros()` there. PYRO_MODE_FALLEN will fire pyro1 on that first tick.

3. **`LANDING_SPEED_CMS` macro**: Currently defined before `detect_descent()`. In the new code it moves to just before `detect_chute_descent()`. Remove the old definition at line 364.

---

## References

- Spec: `docs/ground-station-interface-spec.md` (Revision 1.2)
- Research: `thoughts/shared/research/2026-04-15-telemetry-format-vs-spec.md`
- Current `state_to_telem_id()`: `src/flight_states.c:691-704`
- Current `detect_descent()`: `src/flight_states.c:366-416`
- Current transitions table: `src/flight_states.c:520-528`
- Current rate gating: `src/flight_states.c:711`
- `test_FLT_APO_01_detects_apogee`: `test/test_flight_states.c:263`
- `test_FLT_LAND_01_detects_landing`: `test/test_flight_states.c:290`
- `test_TEL_07_state_mapping`: `test/test_flight_states.c:391`
- `test_TEL_06_altitude_and_speed`: `test/test_flight_states.c:416`
