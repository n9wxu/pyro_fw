/*
 * The flight state machine: a detector per state raises an event, and the
 * transition table maps it. See docs/flight_states.md "How it runs".
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

/* Overridden by src/lua/lua_app.c where Lua is linked. Here, not in the HAL,
 * because the host tests link this file without hal_common.c. */
__attribute__((weak)) bool lua_app_ready_or_absent(void) {
    return true;
}

/* A platform with no ground test pin. */
__attribute__((weak)) bool hal_ground_test_asserted(void) {
    return false;
}
#include "board_id.h"
#include "flight_states.h"
#include "brownout.h"
#include "pressure_processing.h"
#include "pressure_fit.h"
#include "mach_lockout.h"
#include "telemetry_formatter.h"
#include "ground_test.h"
#include "buzzer.h"
#include "beep_store.h"
#include "pyro_release.h"
#include "loop_period.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ring buffer ──────────────────────────────────────────────────── */

/* States whose samples belong in the flight log. A set, not an ordering:
 * BOOT_SENSOR and FAULT are numbered after LANDED. */
static bool state_is_logged(flight_state_t st) {
    switch (st) {
    case ASCENT:
    case FALLING:
    case DROGUE_DESCENT:
    case CHUTE_DESCENT:
    case LANDED:
        return true;
    default:
        return false;
    }
}

/* Every sample goes to the log; its plan decides what it keeps
 * [FLT-LOG-07, log_plan.h]. */
void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    uint8_t thrust = st == ASCENT && ctx->under_thrust; /* [FLT-ASC-03] */
    if (state_is_logged((flight_state_t)st)) {
        hal_log_sample(time_ms, pressure, altitude, st, thrust, EVT_NONE);
    }
    if (ctx->buf_count == FLIGHT_BUF_SIZE) {
        ctx->buf_tail = (ctx->buf_tail + 1) % FLIGHT_BUF_SIZE;
        ctx->buf_count--;
    }
    flight_sample_t *s = &ctx->flight_buffer[ctx->buf_head];
    s->time_ms = time_ms;
    s->pressure_pa = pressure;
    s->altitude_cm = altitude;
    s->state = st;
    s->under_thrust = thrust;
    s->event = EVT_NONE;
    ctx->buf_head = (ctx->buf_head + 1) % FLIGHT_BUF_SIZE;
    ctx->buf_count++;
}

static const flight_sample_t *buf_newest(const flight_context_t *ctx) {
    return &ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE];
}

/* An event row in the flight log, against the newest sample. */
static void log_event(flight_context_t *ctx, uint8_t event) {
    const flight_sample_t *s = buf_newest(ctx);
    if (hal_log_active() && state_is_logged((flight_state_t)s->state))
        hal_log_sample(s->time_ms, s->pressure_pa, s->altitude_cm, s->state, s->under_thrust, event);
}

static void buf_tag_event(flight_context_t *ctx, uint8_t event) { /* [DAT-03] */
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].event = event;
    log_event(ctx, event);
}

#define MAX_ALTITUDE_CM 800000 /* [SNS-ALT-02, PYR-ALT-01] */

/* [USB-01, USB-08] */
static bool grounded_on_usb(const flight_context_t *ctx) {
    return ctx->usb_attached && !ctx->test_mode;
}

static int32_t cm_to_units(int32_t cm, uint8_t units) {
    switch (units) {
    case 1:
        return cm / 100;
    case 2:
        return cm * 100 / 3048;
    default:
        return cm;
    }
}

/* ── Pyro firing logic ────────────────────────────────────────────── */

/* [PYR-MODE-01..05, PYR-ALT-01] AGL and FALLEN read the fit's height, which
 * does not lag; DELAY counts from where the fit put the apogee.
 * See IMPLEMENTATION.md "Altitude Limitations". */
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected)
        return false; /* [PYR-SAFE-04] */
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    int32_t clamped = ((int32_t)value > max_units) ? max_units : (int32_t)value;
    int32_t h = ctx->trigger_height_cm;
    int32_t peak = ctx->peak_height_cm > 0 ? ctx->peak_height_cm : ctx->max_altitude;
    int32_t fallen = cm_to_units(peak - h, ctx->config.units);
    int32_t agl = cm_to_units(h > 0 ? h : 0, ctx->config.units);
    int32_t speed = cm_to_units(-ctx->vertical_speed_cms, ctx->config.units);
    uint32_t delay_s = (hal_time_ms() - ctx->apogee_time) / 1000;
    switch (mode) {
    case PYRO_MODE_FALLEN:
        return fallen >= clamped;
    case PYRO_MODE_AGL:
        return agl <= clamped;
    case PYRO_MODE_SPEED:
        return speed >= clamped;
    case PYRO_MODE_DELAY:
        return delay_s >= value;
    default:
        return false;
    }
}

/* [PYR-DEPLOY-02] BUSY is not a failure: the caller asks again next tick. */
pyro_fire_result_t flight_pyro_energise(uint8_t channel) {
    if (hal_pyro_is_firing())
        return PYRO_BUSY;
    hal_pyro_fire(channel);
    return hal_pyro_is_firing() ? PYRO_ENERGISED : PYRO_REFUSED;
}

/* [PYR-FIRE-01, PYR-SAFE-03, SYS-DEPLOY-01] Every in-flight fire goes
 * through here. */
static bool fire_channel(flight_context_t *ctx, int ch, uint32_t now) {
    pyro_fire_result_t r = flight_pyro_energise((uint8_t)ch);
    if (r == PYRO_BUSY)
        return false;
    if (ch == 1)
        ctx->pyro1_fire_time = now;
    else
        ctx->pyro2_fire_time = now;
    if (r == PYRO_REFUSED) {
        if (ch == 1)
            ctx->pyro1_refused = true;
        else
            ctx->pyro2_refused = true;
        buf_tag_event(ctx, ch == 1 ? EVT_PYRO1_REFUSED : EVT_PYRO2_REFUSED);
        return false;
    }
    if (ch == 1) {
        ctx->pyro1_fired = true;
        ctx->pyro1_verified = false;
    } else {
        ctx->pyro2_fired = true;
        ctx->pyro2_verified = false;
    }
    buf_tag_event(ctx, ch == 1 ? EVT_PYRO1_FIRE : EVT_PYRO2_FIRE);
    telemetry_pyro_fire(ch, ctx->last_altitude, now - ctx->launch_time);
    return true;
}

static bool channel_waiting(bool fired, bool refused, bool continuity) {
    return !fired && !refused && continuity;
}

/* [PYR-MODE-06, DD-048] An unclean fit is waited out for at most this long,
 * then believed, so a swinging canopy cannot hold back the main. The wait
 * restarts at each charge. */
#define UNCLEAN_WAIT_MS 2000u

/* After a charge, an unclean fit reading no lower than the last clean fit
 * carried on ballistically is believed: a charge in the bay reads the rocket
 * lower, never faster, and a canopy's opening shock must not hold back a main
 * set just below the drogue. */
static bool below_ballistic(const flight_context_t *ctx) {
    if (ctx->clean_ms == 0)
        return true;
    float dt = (float)(ctx->last_sample - ctx->clean_ms) / 1000.0f;
    float p = ctx->clean_pa + ctx->clean_pdot * dt + 0.5f * ctx->clean_pddot * dt * dt;
    return ctx->fit_pa > p;
}

static bool pressure_believed(const flight_context_t *ctx, uint32_t now) {
    if (ctx->fit_clean)
        return true;
    /* [SNS-PRES-10, SNS-PRES-11] A failed sensor is waited out for good. */
    if (ctx->fit_suspect)
        return false;
    if ((ctx->pyro1_fired && now - ctx->pyro1_fire_time < UNCLEAN_WAIT_MS) ||
        (ctx->pyro2_fired && now - ctx->pyro2_fire_time < UNCLEAN_WAIT_MS))
        return !below_ballistic(ctx);
    return ctx->last_sample + 1u - ctx->unclean_since >= UNCLEAN_WAIT_MS;
}

static bool trigger_met(flight_context_t *ctx, uint8_t mode, uint16_t value, uint32_t now) {
    if (mode != PYRO_MODE_DELAY && !pressure_believed(ctx, now))
        return false;
    return should_fire_pyro(ctx, mode, value);
}

/* [PYR-DEPLOY-02] A channel whose trigger was met stays due while the other's
 * pulse holds the common: by then that charge has spoiled the fit, and the
 * trigger must not be asked again. */
static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
    if (channel_waiting(ctx->pyro1_fired, ctx->pyro1_refused, ctx->pyro1_continuity_good) &&
        (ctx->pyro1_due || trigger_met(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value, now)))
        ctx->pyro1_due = !fire_channel(ctx, 1, now) && !ctx->pyro1_refused;
    if (channel_waiting(ctx->pyro2_fired, ctx->pyro2_refused, ctx->pyro2_continuity_good) &&
        (ctx->pyro2_due || trigger_met(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value, now)))
        ctx->pyro2_due = !fire_channel(ctx, 2, now) && !ctx->pyro2_refused;
}

/* [PYR-FAULT-02, PYR-FAULT-03] */
static void check_pyro_fault(flight_context_t *ctx) {
    if (ctx->pyro1_fired && !ctx->pyro1_fault && hal_pyro_fault(1)) {
        ctx->pyro1_fault = true;
        buf_tag_event(ctx, EVT_PYRO1_FAULT);
    }
    if (ctx->pyro2_fired && !ctx->pyro2_fault && hal_pyro_fault(2)) {
        ctx->pyro2_fault = true;
        buf_tag_event(ctx, EVT_PYRO2_FAULT);
    }
}

/* [PYR-VERIFY-01] The verdict is the board's first reading after the pulse;
 * until a check has run since, a fired channel reads none of good, open and
 * shorted (hal.h). Not asked before the longest pulse a board drives, for a
 * HAL whose reading is live. */
#define VERIFY_AFTER_MS 500u

static bool verify_due(bool fired, bool verified, uint32_t fire_time, uint32_t now) {
    return fired && !verified && now - fire_time > VERIFY_AFTER_MS;
}

static void verify_channel(flight_context_t *ctx, uint8_t ch, bool *verified, bool *failed) {
    hal_continuity_t c = {0};
    hal_pyro_get(ch, &c);
    if (!c.good && !c.open && !c.shorted)
        return;
    *verified = true;
    if (c.good && !c.open) {
        *failed = true;
        buf_tag_event(ctx, ch == 1 ? EVT_PYRO1_NOPEN : EVT_PYRO2_NOPEN);
    }
}

static void check_post_fire_verify(flight_context_t *ctx, uint32_t now) {
    bool due1 = verify_due(ctx->pyro1_fired, ctx->pyro1_verified, ctx->pyro1_fire_time, now);
    bool due2 = verify_due(ctx->pyro2_fired, ctx->pyro2_verified, ctx->pyro2_fire_time, now);
    if (!due1 && !due2)
        return;
    hal_pyro_sample();
    if (due1)
        verify_channel(ctx, 1, &ctx->pyro1_verified, &ctx->pyro1_verify_fail);
    if (due2)
        verify_channel(ctx, 2, &ctx->pyro2_verified, &ctx->pyro2_verify_fail);
}

/* ── Event detectors ──────────────────────────────────────────────── */

/* [FLT-BOOT-09] The sensor's settle before calibration. */
#define BOOT_SETTLE_MS 2500u

/* [GND-TEST-05] The settle also watches the ground test pin. */
static state_event_t detect_boot_settle(flight_context_t *ctx, uint32_t now) {
    bool held = hal_ground_test_asserted();
    if (held && !ctx->gt_held)
        ctx->gt_held_since = now;
    ctx->gt_held = held;
    if (now - ctx->boot_timer < BOOT_SETTLE_MS)
        return SEVT_NONE;
    ctx->gt_requested = held && now - ctx->gt_held_since >= GT_BOOT_HOLD_MS;
    return SEVT_TIMER;
}

static bool assess_recovery(flight_context_t *ctx, uint32_t now, state_event_t *evt);
static void read_continuity(flight_context_t *ctx);

/* [FLT-BOOT-05] The sensor is tested first, and a failure is terminal: the
 * continuity verdict is worth nothing on a board that cannot see a launch.
 * See docs/flight_states.md "BOOT_SENSOR (9)". */
static state_event_t detect_boot_sensor(flight_context_t *ctx, uint32_t now) {
    extern void hal_telemetry_send(const char *sentence);
    if (ctx->sensor_type == SENSOR_PENDING) {
        int sensor = hal_pressure_sensor();
        if (sensor < 0 && now - ctx->boot_timer < SENSOR_BRINGUP_MS)
            return SEVT_NONE;
        ctx->sensor_type = sensor < 0 ? 0u : (uint8_t)sensor;
    }
    if (ctx->sensor_type == 0) {
        hal_telemetry_send("!SENSOR FAIL - no pressure sensor answered\r\n");
        ctx->diag |= DIAG_SENSOR_FAIL;
        return SEVT_FAULT;
    }
    if (!ctx->fs_ok) {
        hal_telemetry_send("!FS FAIL - filesystem did not mount\r\n");
        ctx->diag |= DIAG_FS_FAIL;
        return SEVT_FAULT;
    }
    /* [FLT-BROWN-02] Lingers until the history holds a speed. */
    state_event_t rec = SEVT_NONE;
    if (!assess_recovery(ctx, now, &rec)) {
        return SEVT_NONE;
    }
    if (rec != SEVT_NONE) {
        return rec;
    }
    return SEVT_DONE;
}

/* Terminal. See docs/flight_states.md "LANDED (8) / FAULT (10)". */
static state_event_t detect_fault(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    return SEVT_NONE;
}

/* ── Brownout recovery [FLT-BROWN-02, DD-026, DD-041] ──────────────
 *
 * Measured against the marker's ground, never a fresh calibration: in the
 * air that would call the current height zero. */
/* A board that cannot answer by then boots cold. */
#define RECOVERY_DEADLINE_MS 4000u

/* The level is the median of the newest 250 ms of history; the speed is that
 * against the median of a window ending 350 ms earlier. Medians, because two
 * bad readings in a row pass the median of three, and two-reading speed noise
 * (1.7 m/s RMS) sits too close to RECOVER_SPEED_CMS. */
#define RECOVERY_LEVEL_MS 250u
#define RECOVERY_SPAN_MS 600u
#define RECOVERY_MIN_READINGS 8

static void mach_flag(flight_context_t *ctx, int32_t p, uint32_t ts);

/* [FLT-MACH-06] A recovered ascent cannot know whether it is supersonic. */
static void recover_locked(flight_context_t *ctx, int32_t level, uint32_t ts, int32_t alt_cm) {
    extern void hal_telemetry_send(const char *sentence);
    mach_flag(ctx, level, ts);
    hal_log_sample(0, level, alt_cm, ASCENT, 0, EVT_MACH_LOCK);
    hal_telemetry_send("!MACH LOCK\r\n");
}

static bool recovery_cold(flight_context_t *ctx, cold_reason_t why) {
    ctx->recovery = (uint8_t)RECOVER_COLD;
    ctx->recovery_why = (uint8_t)why;
    return true;
}

static bool assess_recovery(flight_context_t *ctx, uint32_t now, state_event_t *evt) {
    *evt = SEVT_NONE;
    pad_marker_t m;
    int n = hal_fs_read_cached(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    if (n != (int)sizeof(m) || !pad_marker_valid(&m))
        return recovery_cold(ctx, COLD_NO_MARKER);
    if (grounded_on_usb(ctx))
        return recovery_cold(ctx, COLD_ON_USB);
    if (now - ctx->boot_timer >= RECOVERY_DEADLINE_MS)
        return recovery_cold(ctx, COLD_NO_SAMPLE);

    uint32_t oldest, newest;
    int32_t level, before;
    if (!pp_history_span(&oldest, &newest) || newest - oldest < RECOVERY_SPAN_MS ||
        !pp_history_median(newest - RECOVERY_LEVEL_MS, newest, RECOVERY_MIN_READINGS, &level) ||
        !pp_history_median(newest - RECOVERY_SPAN_MS, newest - (RECOVERY_SPAN_MS - RECOVERY_LEVEL_MS),
                           RECOVERY_MIN_READINGS, &before)) {
        return false; /* not enough history yet; asked again next tick */
    }

    int32_t alt_agl = pp_pressure_to_altitude_cm(level, m.ground_pressure_pa);
    int32_t alt_before = pp_pressure_to_altitude_cm(before, m.ground_pressure_pa);
    int32_t speed = (alt_agl - alt_before) * 1000 / (int32_t)(RECOVERY_SPAN_MS - RECOVERY_LEVEL_MS);

    recovery_t r = brownout_assess((reset_cause_t)ctx->reset_cause, true, alt_agl, speed);
    ctx->recovery = (uint8_t)r;
    if (r == RECOVER_COLD)
        return recovery_cold(ctx, ctx->reset_cause == RESET_POWER_EVENT ? COLD_AT_GROUND : COLD_NOT_POWER);
    if (r != RECOVER_ASCENT && r != RECOVER_DESCENT)
        return true;

    /* A new log, opened mid-air: T+0 is the moment of recovery. */
    pp_resume_flight(m.ground_pressure_pa, level);
    pp_set_sigma((float)m.sigma_mpa / 1000.0f);
    /* [FLT-BROWN-06, PYR-SAFE-01] BOOT_CONTINUITY is skipped on this path. */
    read_continuity(ctx);
    ctx->diag |= DIAG_BROWNOUT;
    ctx->ground_pressure = m.ground_pressure_pa;
    ctx->filtered_pressure = level;
    ctx->launch_time = newest;
    ctx->max_altitude = alt_agl;
    ctx->last_altitude = alt_agl;
    ctx->last_height = pp_pressure_to_height_cm(level, m.ground_pressure_pa);
    ctx->trigger_height_cm = ctx->last_height;
    ctx->last_sample = newest;
    ctx->vertical_speed_cms = speed;
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(0, ctx->filtered_pressure, alt_agl, ASCENT, 0, EVT_LAUNCH);
    if (r == RECOVER_ASCENT)
        recover_locked(ctx, level, newest, alt_agl);
    *evt = (r == RECOVER_ASCENT) ? SEVT_RECOVER_ASCENT : SEVT_RECOVER_DESCENT;
    return true;
}

static void read_continuity(flight_context_t *ctx) {
    hal_continuity_t c1, c2;
    hal_pyro_sample();
    hal_pyro_get(1, &c1);
    hal_pyro_get(2, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
}

/* [FLT-BOOT-07, GND-TEST-05] Ground test mode is taken once the continuity
 * has been read [PYR-SAFE-01]. */
static state_event_t detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    read_continuity(ctx);
    ctx->boot_timer = now;
    return ctx->gt_requested ? SEVT_GROUND_TEST : SEVT_DONE;
}

/* A sensor that answered at bring-up but gives no samples: on the pad it
 * could never detect a launch, so it is a fault. */
#define CAL_TIMEOUT_MS 10000u

/* [FLT-BOOT-08] */
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (pp_cal_done())
        return SEVT_CAL_DONE;
    if (now - ctx->boot_timer >= CAL_TIMEOUT_MS) {
        extern void hal_telemetry_send(const char *sentence);
        hal_telemetry_send("!CAL TIMEOUT - sensor produced no samples\r\n");
        ctx->diag |= DIAG_SENSOR_FAIL;
        return SEVT_FAULT;
    }

    return SEVT_NONE;
}

static bool mode_is_altitude(uint8_t mode) {
    return mode == PYRO_MODE_AGL || mode == PYRO_MODE_FALLEN || mode == PYRO_MODE_SPEED;
}

/* [CFG-04] A channel with an igniter the flight software means to fire: a
 * released channel is a Lua output, and a disabled one has nothing on it. */
static bool channel_expects_igniter(const flight_context_t *ctx, uint8_t ch) {
    uint8_t mode = ch == 1 ? ctx->config.pyro1_mode : ctx->config.pyro2_mode;
    return mode != PYRO_MODE_NONE && !pyro_release_is_released(ch);
}

/* What is wrong, as DIAG_* bits; beep_reason_for_diag() is what to do. */
static uint16_t pad_faults(const flight_context_t *ctx, const hal_continuity_t *c1, const hal_continuity_t *c2) {
    uint16_t d = 0;
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    if ((mode_is_altitude(ctx->config.pyro1_mode) && ctx->config.pyro1_value > max_units) ||
        (mode_is_altitude(ctx->config.pyro2_mode) && ctx->config.pyro2_value > max_units)) {
        d |= DIAG_CFG_RANGE;
    }
    if (!c1->good && channel_expects_igniter(ctx, 1)) {
        d |= c1->open ? DIAG_P1_OPEN : DIAG_P1_SHORT;
    }
    if (!c2->good && channel_expects_igniter(ctx, 2)) {
        d |= c2->open ? DIAG_P2_OPEN : DIAG_P2_SHORT;
    }
    return d;
}

/* Four outcomes, the four things an operator can do at a rocket. What cannot
 * be fixed at the pad outranks what can; channel 1 is named before 2. */
beep_reason_t beep_reason_for_diag(uint16_t diag) {
    if (diag & DIAG_FATAL_ANY) {
        return BR_SYSTEM_FAILURE;
    }
    if (diag & (DIAG_P1_OPEN | DIAG_P1_SHORT)) {
        return BR_CHECK_PYRO_1;
    }
    if (diag & (DIAG_P2_OPEN | DIAG_P2_SHORT)) {
        return BR_CHECK_PYRO_2;
    }
    return BR_OK_TO_FLY;
}

/* [PYR-CONT-01] At least once a second: due a loop early, so the 20 ms grid
 * never stretches it past the second. */
#define CONT_CHECK_MS (1000u - LOOP_PERIOD_MS)

static bool sample_continuity(flight_context_t *ctx, uint32_t now, hal_continuity_t *c1, hal_continuity_t *c2) {
    if (now - ctx->last_cont_check < CONT_CHECK_MS)
        return false;
    hal_pyro_sample();
    hal_pyro_get(1, c1);
    hal_pyro_get(2, c2);
    ctx->pyro1_continuity_good = c1->good;
    ctx->pyro2_continuity_good = c2->good;
    ctx->pyro1_adc = c1->raw_adc;
    ctx->pyro2_adc = c2->raw_adc;
    ctx->last_cont_check = now;
    return true;
}

static void update_continuity_and_buzzer(flight_context_t *ctx, uint32_t now) { /* [PYR-CONT-01, PYR-ALT-02] */
    hal_continuity_t c1, c2;
    if (!sample_continuity(ctx, now, &c1, &c2))
        return;

    /* [FLT-BOOT-15, PYR-CONT-03] */
    ctx->diag = (uint16_t)((ctx->diag & ~DIAG_PAD_ANY) | pad_faults(ctx, &c1, &c2));

    /* [USB-02] Diagnosed, for the screen, but not said. */
    if (grounded_on_usb(ctx))
        return;

    /* The first announcement waits for a script to be running, not still
     * compiling: the beep is all an operator at the pad has. */
    if (!ctx->buzzer_started && !lua_app_ready_or_absent())
        return;

    /* [PYR-CONT-03] Said again whenever the answer changes. */
    beep_reason_t r = beep_reason_for_diag(ctx->diag);
    if (ctx->buzzer_started && r == (beep_reason_t)ctx->last_reason)
        return;
    ctx->buzzer_started = true;
    ctx->last_reason = (uint8_t)r;
    beep_say(r);
}

/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-LAUNCH-07, DD-016] The height rejects
 * weather drift; the speed rejects a slow rise. */
#define LAUNCH_ALT_CM 3048 /* 100 ft */
#define LAUNCH_SPEED_CMS 500
#define LAUNCH_RISE_CM 50 /* [FLT-LAUNCH-03] T+0 is the first sample above this */

/* [GND-CAL-06] Long enough that no gust lasts it. */
#define GND_RESEED_MS 5000u
#define GND_STILL_CMS 100 /* 1 m/s */

/* [FLT-LAUNCH-07, FLT-APO-01] Two bad readings in a row pass the median of
 * three. Durations, never sample counts: at another rate a count is another
 * hold. */
#define LAUNCH_HOLD_MS 100u
#define APOGEE_HOLD_MS 60u

/* True once cond has held continuously for hold_ms of sample time. A sample
 * time of 0 is legitimate, so the start is kept one past it. */
static bool held(bool cond, uint32_t *since, uint32_t ts, uint32_t hold_ms) {
    if (!cond) {
        *since = 0;
        return false;
    }
    if (*since == 0)
        *since = ts + 1u;
    return ts + 1u - *since >= hold_ms;
}

/* [FLT-BROWN-01, DD-033] The pad marker, written once, long before the launch
 * shock it protects against. Here, not in the PAD_IDLE detector, which runs
 * with the flash window shut. A failure costs only the recovery path, so it
 * is not retried. */
void flight_flash_service(flight_context_t *ctx, uint32_t now) {
    /* [FLT-BROWN-04] hal.h has no delete, so an invalid marker is written,
     * once the log has let go of the filesystem [WEB-API-08]. */
    if (ctx->current_state == LANDED && !ctx->marker_spent) {
        if (hal_log_active())
            return;
        ctx->marker_spent = true;
        pad_marker_t spent;
        memset(&spent, 0, sizeof(spent));
        (void)hal_fs_write_file(PAD_MARKER_PATH, (const char *)&spent, (int)sizeof(spent));
        return;
    }
    if (ctx->current_state != PAD_IDLE || grounded_on_usb(ctx) || ctx->marker_written ||
        now - ctx->boot_timer < PAD_MARKER_DWELL_MS) {
        return;
    }
    ctx->marker_written = true; /* once, whatever the write does */
    pad_marker_t m;
    pad_marker_fill(&m, pp_ground_pressure(), (uint32_t)(pp_sigma_pa() * 1000.0f));
    (void)hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m));
}

/* The change in the unclamped filtered height since the last sample. */
static int32_t two_point_speed(const flight_context_t *ctx, const altitude_sample_t *s) {
    uint32_t dt = s->timestamp_ms - ctx->last_sample;
    if (dt == 0 || ctx->last_sample == 0)
        return ctx->vertical_speed_cms;
    return (s->height_cm - ctx->last_height) * 1000 / (int32_t)dt;
}

/* [FLT-ASC-02, DD-048] Short of a fit, the first second after power-on, the
 * two-point speed. */
static int32_t sample_speed(const flight_context_t *ctx, const altitude_sample_t *s) {
    return s->fit_valid ? s->speed_cms : two_point_speed(ctx, s);
}

/* [FLT-LAUNCH-07] A burst of bad readings spoils every fit for a second, far
 * longer than the launch hold, but the two-point speed only for the burst. */
static int32_t launch_speed(const flight_context_t *ctx, const altitude_sample_t *s) {
    return s->fit_clean ? s->speed_cms : two_point_speed(ctx, s);
}

static void take_fit(flight_context_t *ctx, const altitude_sample_t *s) {
    uint32_t ts = s->timestamp_ms;
    ctx->fit_clean = s->fit_clean;
    ctx->fit_suspect = s->fit_suspect;
    ctx->fit_pa = s->fit_pa;
    if (s->fit_clean) {
        ctx->clean_pa = s->fit_pa;
        ctx->clean_pdot = s->fit_pdot;
        ctx->clean_pddot = s->fit_pddot;
        ctx->clean_ms = ts;
    }
    if (!s->fit_clean) {
        ctx->clean_since = 0;
        if (ctx->unclean_since == 0)
            ctx->unclean_since = ts + 1u;
    } else if (held(true, &ctx->clean_since, ts, PFIT_WINDOW_US / 1000u)) {
        ctx->unclean_since = 0;
    }
    ctx->trigger_height_cm = s->fit_valid ? s->fit_height_cm : s->height_cm;
}

static void pad_mach_flag(flight_context_t *ctx, const altitude_sample_t *s, uint32_t ts);

/* [SNS-PRES-10] Said once each time it sticks, after the sample's row so the
 * event carries its time. The DIAG bit stays. */
static void note_stuck(flight_context_t *ctx, const altitude_sample_t *s) {
    extern void hal_telemetry_send(const char *sentence);
    if (!s->sensor_stuck) {
        ctx->sensor_stuck = false;
        return;
    }
    if (ctx->sensor_stuck)
        return;
    ctx->sensor_stuck = true;
    ctx->diag |= DIAG_SENSOR_STUCK;
    buf_tag_event(ctx, EVT_SENSOR_STUCK);
    hal_telemetry_send("!SENSOR STUCK\r\n");
}

#define SENSOR_LOST_MS 500u /* [SNS-PRES-11] */

static void watch_sensor(flight_context_t *ctx, uint32_t now) {
    extern void hal_telemetry_send(const char *sentence);
    if (ctx->sensor_lost || now - ctx->last_sample < SENSOR_LOST_MS)
        return;
    ctx->sensor_lost = true;
    ctx->diag |= DIAG_SENSOR_LOST;
    buf_tag_event(ctx, EVT_SENSOR_LOST);
    hal_telemetry_send("!SENSOR LOST\r\n");
}

static state_event_t detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    /* [GND-TEST-01..04, DD-011] Before the sample gate, so commands drain
     * promptly. */
    char cmd_buf[64];
    if (hal_serial_readline(cmd_buf, sizeof(cmd_buf)))
        ground_test_handle_command(&ctx->gt, cmd_buf, ctx, now);
    ground_test_update(&ctx->gt, now);

    update_continuity_and_buzzer(ctx, now);

    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;

    ctx->pad_speed_cms = launch_speed(ctx, &sample);
    take_fit(ctx, &sample);

    if (sample.rise_cm <= LAUNCH_RISE_CM) {
        ctx->pad_rising = false;
    } else if (!ctx->pad_rising) {
        ctx->pad_rising = true;
        ctx->pad_rise_ms = ts;
    }
    pad_mach_flag(ctx, &sample, ts);

    ctx->filtered_pressure = pp_last_filtered_pa();

    if (pp_ground_rejecting_ms(ts) >= GND_RESEED_MS && abs(ctx->pad_speed_cms) < GND_STILL_CMS) {
        extern void hal_telemetry_send(const char *sentence);
        pp_ground_reseed();
        hal_telemetry_send("!GND reseed\r\n");
        ctx->boot_timer = now; /* the marker's dwell starts again */
        ctx->marker_written = false;
    }

    ctx->ground_pressure = pp_ground_pressure(); /* [GND-CAL-01] */

    buf_add(ctx, 0, ctx->filtered_pressure, altitude, PAD_IDLE);
    note_stuck(ctx, &sample);
    ctx->last_altitude = altitude;
    ctx->last_height = sample.height_cm;
    ctx->last_sample = ts;

    bool alt_ok = altitude > LAUNCH_ALT_CM;
    bool speed_ok = ctx->pad_speed_cms > LAUNCH_SPEED_CMS;
    bool launch = held(alt_ok && speed_ok && !grounded_on_usb(ctx), &ctx->launch_held_since, ts, LAUNCH_HOLD_MS);
    return launch ? SEVT_LAUNCH : SEVT_NONE; /* [USB-01] */
}

/* [FLT-ASC-04..07, DD-017] Armed once faster than this and then slower,
 * climbing: a burn, then a coast. */
#define ARM_SPEED_CMS 1000

/* [FLT-APO-01] The fitted pressure this far above the lowest a clean fit
 * showed: 0.6-0.9 m below the peak from sea level to 9 km, 3 Pa at 9 km
 * against the fit's half-pascal noise. */
#define APOGEE_DROP 1.0001f

/* ── The Mach lockout [FLT-MACH-02..07, DD-049] ──────────────────────
 * See docs/flight_states.md "The Mach lockout". */
#define MACH_RELEASE_MS 1000u /* a design constant: the signature, held */
#define MACH_LOWER_BOUND_MS 2000u

static void mach_note(flight_context_t *ctx, uint8_t evt, const char *line) {
    extern void hal_telemetry_send(const char *sentence);
    buf_tag_event(ctx, evt);
    hal_telemetry_send(line);
}

static void mach_flag(flight_context_t *ctx, int32_t p, uint32_t ts) {
    ctx->mach_lock = true;
    ctx->p_flag_pa = p;
    ctx->mach_flag_ms = ts;
    ctx->release_since = 0;
    ctx->fallback_since = 0;
}

/* The peak starts again here: nothing from the locked interval may be it. */
static void mach_release(flight_context_t *ctx, const altitude_sample_t *s, uint32_t ts) {
    ctx->mach_lock = false;
    ctx->mach_released = true;
    ctx->mach_release_ms = ts;
    ctx->p_min_pa = s->fit_pa;
    ctx->peak_height_cm = s->fit_height_cm;
    mach_note(ctx, EVT_MACH_UNLOCK, "!MACH UNLOCK\r\n");
}

/* Never let the lock prevent a deployment: a flagged flight was a real one,
 * so the fallback arms what arming missed. */
static void mach_fall_back(flight_context_t *ctx, uint32_t ts) {
    ctx->mach_lock = false;
    ctx->mach_fallback = true;
    if (!ctx->pyros_armed) {
        ctx->pyros_armed = true;
        ctx->armed_time = ts;
    }
    ctx->apogee_fit_ms = ts;
    mach_note(ctx, EVT_MACH_FALLBACK, "!MACH FALLBACK\r\n");
}

/* Before any release, any fit and the 40 ms rate: setting the flag is the
 * safe direction, and through a 66 g boost's first second the fit reads the
 * climb 120 m/s slow. After one, only a clean fit: a bad reading must not
 * lock out the apogee just ahead. */
static bool mach_flag_due(const flight_context_t *ctx, const altitude_sample_t *s, int32_t p) {
    if (s->fit_suspect)
        return false; /* a sensor coming back jumps: that is no climb [SNS-PRES-10] */
    if (ctx->mach_released)
        return s->fit_clean && mach_too_fast(p, s->fit_pdot);
    return mach_too_fast(p, s->fit_pdot) || mach_too_fast(p, s->short_pdot);
}

/* [FLT-MACH-02] The flag is the climb's from T+0: a 66 g boost is past Mach
 * 0.85 before the launch detector trips. A flag no launch follows for this
 * long was a bad reading. */
#define PAD_FLAG_FORGET_MS 10000u

static void pad_mach_flag(flight_context_t *ctx, const altitude_sample_t *s, uint32_t ts) {
    if (ctx->mach_lock && !ctx->pad_rising && ts - ctx->mach_flag_ms >= PAD_FLAG_FORGET_MS) {
        ctx->mach_lock = false;
        ctx->mach_flag_ms = 0;
        ctx->p_flag_pa = 0;
    }
    if (ctx->mach_lock || !ctx->pad_rising || !s->fit_valid)
        return;
    int32_t p = mach_round_clamp(s->fit_pa, MACH_RATE_CLAMP);
    if (mach_flag_due(ctx, s, p))
        mach_flag(ctx, s->fit_clean ? p : s->raw_pa, ts);
}

static state_event_t mach_lockout(flight_context_t *ctx, const altitude_sample_t *s, uint32_t ts) {
    if (!s->fit_valid)
        return SEVT_NONE;
    int32_t p = mach_round_clamp(s->fit_pa, MACH_RATE_CLAMP);
    if (!ctx->mach_lock) {
        if (mach_flag_due(ctx, s, p)) {
            /* A spoiled fit's pressure is not where the rocket was. */
            mach_flag(ctx, s->fit_clean ? p : s->raw_pa, ts);
            mach_note(ctx, EVT_MACH_LOCK, "!MACH LOCK\r\n");
        }
        return SEVT_NONE;
    }
    bool coasting = s->fit_clean && mach_slow_ascent(p, s->fit_pdot) && mach_decelerating(p, s->fit_pddot);
    if (held(coasting, &ctx->release_since, ts, MACH_RELEASE_MS)) {
        mach_release(ctx, s, ts);
        return SEVT_NONE;
    }
    bool back = s->fit_clean && s->fit_pdot > 0.0f && p > ctx->p_flag_pa;
    if (!held(back, &ctx->fallback_since, ts, MACH_RELEASE_MS))
        return SEVT_NONE;
    mach_fall_back(ctx, ts);
    return SEVT_APOGEE;
}

/* [FLT-ASC-04..07, FLT-MACH-06, DD-017] Descending counts: a sensor failed
 * near apogee must not close the window for good, and apogee has its own
 * tests. */
static bool arming_gate_met(const flight_context_t *ctx) {
    return !ctx->pyros_armed && ctx->arm_height && !ctx->fit_suspect && ctx->max_speed_cms >= ARM_SPEED_CMS &&
           ctx->vertical_speed_cms < ARM_SPEED_CMS;
}

/* The peak is the lowest pressure a clean fit has shown. */
static void track_peak(flight_context_t *ctx, const altitude_sample_t *s) {
    if (s->fit_clean && (ctx->p_min_pa <= 0.0f || s->fit_pa < ctx->p_min_pa)) {
        ctx->p_min_pa = s->fit_pa;
        ctx->peak_height_cm = s->fit_height_cm;
    }
}

/* [PYR-MODE-05] Where the fit's rate crossed zero: pdot/pddot before this
 * sample, if that lies within the fit's window. */
static uint32_t apogee_back(uint32_t ts, const altitude_sample_t *s) {
    if (s->fit_pddot <= 0.0f)
        return ts;
    float back_ms = s->fit_pdot / s->fit_pddot * 1000.0f;
    if (back_ms < 0.0f || back_ms > (float)(PFIT_WINDOW_US / 1000u))
        return ts;
    return ts - (uint32_t)back_ms;
}

/* The sample's speed, thrust [FLT-ASC-03] and row, and the arming height. */
static void ascent_take(flight_context_t *ctx, const altitude_sample_t *s) {
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    ctx->vertical_speed_cms = sample_speed(ctx, s);
    take_fit(ctx, s);
    ctx->under_thrust = s->fit_valid ? s->accel_cms2 > 0 : ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;
    if (ctx->vertical_speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = ctx->vertical_speed_cms;
    int32_t p = s->fit_valid ? mach_round_clamp(s->fit_pa, MACH_RATE_CLAMP) : ctx->filtered_pressure;
    if (mach_above_arm_height(p, ctx->ground_pressure))
        ctx->arm_height = true;

    buf_add(ctx, s->timestamp_ms - ctx->launch_time, ctx->filtered_pressure, s->altitude_cm, ASCENT);
    ctx->last_altitude = s->altitude_cm;
    ctx->last_height = s->height_cm;
    ctx->last_sample = s->timestamp_ms; /* [FLT-RATE-05] */
}

/* [FLT-MACH-07] The reported peak is the lowest pressure a clean fit showed
 * outside the lock; the filtered altitude's only until a clean fit exists. */
static void ascent_peak(flight_context_t *ctx, int32_t altitude) {
    if (ctx->p_min_pa > 0.0f) {
        int32_t h = ctx->peak_height_cm;
        ctx->max_altitude = h < 0 ? 0 : (h > MAX_ALTITUDE_CM ? MAX_ALTITUDE_CM : h);
    } else if (altitude > ctx->max_altitude) {
        ctx->max_altitude = altitude;
    }
}

/* [FLT-APO-01, FLT-APO-04, FLT-MACH-05] No timer may force it [DD-022]. */
static bool apogee_seen(flight_context_t *ctx, const altitude_sample_t *s, uint32_t ts) {
    bool cond = ctx->pyros_armed && !ctx->apogee_detected && !ctx->mach_lock && s->fit_clean && s->fit_pdot > 0.0f;
    if (!held(cond, &ctx->apogee_held_since, ts, APOGEE_HOLD_MS) || s->fit_pa < APOGEE_DROP * ctx->p_min_pa)
        return false;
    ctx->apogee_fit_ms = apogee_back(ts, s);
    return true;
}

/* Every decision here is on sample time [FLT-RATE-05]; `now` only watches for
 * samples that stop coming. */
static state_event_t detect_ascent(flight_context_t *ctx, uint32_t now) {
    watch_sensor(ctx, now);
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    uint32_t ts = sample.timestamp_ms;
    ascent_take(ctx, &sample);
    ctx->sensor_lost = false;
    note_stuck(ctx, &sample);
    state_event_t lock_evt = mach_lockout(ctx, &sample, ts);
    if (!ctx->mach_lock)
        track_peak(ctx, &sample);
    ascent_peak(ctx, sample.altitude_cm);

    if (lock_evt != SEVT_NONE)
        return lock_evt;
    if (arming_gate_met(ctx))
        return SEVT_ARMED;
    return apogee_seen(ctx, &sample, ts) ? SEVT_APOGEE : SEVT_NONE;
}

/* ── Descent [FLT-DESC-01, DD-023] ──────────────────────────────────
 * Read from the rocket, never from the firing log.
 * See docs/flight_states.md "How a descent phase is decided". */

#define DESC_DROGUE_CMS 3500
#define DESC_MAIN_CMS 1000

/* [FLT-AIR-01, DD-079] */
static int32_t pad_air_speed(const flight_context_t *ctx) {
    float k = ctx->air_scale > 0.0f ? ctx->air_scale : 1.0f;
    return (int32_t)((float)ctx->vertical_speed_cms * k);
}

#define DESC_DWELL_MS 1200
#define DESC_TOL_MIN_CMS 250
#define DESC_TOL_FRAC 4
#define DESC_FAIL_MS 1000 /* a rate the phase cannot explain, held this long */

typedef enum { BAND_FAST = 0, BAND_DROGUE, BAND_MAIN } desc_band_t;

static int32_t descent_rate(const flight_context_t *ctx) {
    int32_t v = pad_air_speed(ctx);
    return v < 0 ? -v : v;
}

static desc_band_t descent_band(int32_t rate) {
    if (rate <= DESC_MAIN_CMS)
        return BAND_MAIN;
    if (rate <= DESC_DROGUE_CMS)
        return BAND_DROGUE;
    return BAND_FAST;
}

static int32_t desc_tolerance(int32_t rate) {
    int32_t tol = rate / DESC_TOL_FRAC;
    return tol < DESC_TOL_MIN_CMS ? DESC_TOL_MIN_CMS : tol;
}

/* True once the rate has stayed in one band, near one value, for the dwell
 * of sample time [FLT-RATE-05]. */
static bool descent_settled(flight_context_t *ctx, uint32_t ts, desc_band_t *out) {
    int32_t v = pad_air_speed(ctx);
    if (v >= 0) {
        ctx->desc_band_since = 0;
        return false;
    }
    int32_t rate = descent_rate(ctx);
    desc_band_t b = descent_band(rate);
    int32_t tol = desc_tolerance(rate);
    int32_t drift = v - ctx->desc_ref_cms;
    if (drift < 0)
        drift = -drift;

    if (b != (desc_band_t)ctx->desc_band || drift > tol || ctx->desc_band_since == 0) {
        ctx->desc_band = (uint8_t)b;
        ctx->desc_ref_cms = v;
        ctx->desc_band_since = ts + 1u;
        return false;
    }
    *out = b;
    return ts + 1u - ctx->desc_band_since >= DESC_DWELL_MS;
}

/* A rate the phase cannot explain, held past a gust. Not stability: a
 * shredded drogue is accelerating. */
static bool band_exceeded(flight_context_t *ctx, uint32_t ts, int32_t ceiling) {
    return held(descent_rate(ctx) > ceiling, &ctx->desc_fail_since, ts, DESC_FAIL_MS);
}

/* [FLT-EMRG-01..04, PYR-REFIRE-01, PYR-REFIRE-02]
 * See docs/flight_states.md "The emergency ladder". */
#define EMRG_DROGUE_GRACE_MS 2000u /* [PYR-REFIRE-01, FLT-EMRG-01] */
#define EMRG_MAX_REFIRE 1          /* [PYR-REFIRE-01] */
/* Shortens the retry's grace only, and set high so that an ordinary failed
 * drogue reaches the retry on the grace. */
#define EMRG_MAIN_PANIC_CMS 9000

/* [FLT-EMRG-01, FLT-EMRG-02] Measured once the drogue has had its grace. A
 * rate falling by more than the tolerance is a canopy biting, and starts the
 * hold again. The grace runs on the loop clock; the hold on sample time. */
static bool drogue_failing(flight_context_t *ctx, uint32_t now, uint32_t drogue_cmd_ms) {
    uint32_t ts = ctx->last_sample;
    int32_t rate = ctx->vertical_speed_cms < 0 ? descent_rate(ctx) : 0;
    /* [SNS-PRES-10, SNS-PRES-11] A failed sensor's speed is no evidence. */
    if (rate <= DESC_DROGUE_CMS || ctx->fit_suspect || now - drogue_cmd_ms < EMRG_DROGUE_GRACE_MS) {
        ctx->emrg_fail_since = 0;
        return false;
    }
    if (ctx->emrg_fail_since == 0 || rate < ctx->emrg_fail_ref_cms - desc_tolerance(ctx->emrg_fail_ref_cms)) {
        ctx->emrg_fail_since = ts + 1u;
        ctx->emrg_fail_ref_cms = rate;
        return false;
    }
    return ts + 1u - ctx->emrg_fail_since >= DESC_FAIL_MS;
}

/* [PYR-REFIRE-01] */
static void retry_drogue(flight_context_t *ctx, uint32_t now, bool canopy_working) {
    bool panic = ctx->vertical_speed_cms < 0 && descent_rate(ctx) >= EMRG_MAIN_PANIC_CMS && !ctx->fit_suspect;
    if (canopy_working || (!panic && now - ctx->pyro1_fire_time < EMRG_DROGUE_GRACE_MS))
        return;
    /* A refused retry is spent too. */
    if (fire_channel(ctx, 1, now) || ctx->pyro1_refused)
        ctx->pyro1_refires++;
}

/* canopy_working: settled in a band a canopy could explain, not the fast one. */
static void emergency_ladder(flight_context_t *ctx, uint32_t now, bool canopy_working) {
    bool drogue_commanded = ctx->pyro1_fired || ctx->pyro1_refused;
    if (!drogue_commanded || ctx->pyro2_fired || ctx->pyro2_refused)
        return;

    if (ctx->pyro1_verify_fail && ctx->pyro1_refires < EMRG_MAX_REFIRE) {
        retry_drogue(ctx, now, canopy_working);
        return;
    }

    /* [CFG-04] Only a main the configuration has, with its igniter. */
    if (!channel_expects_igniter(ctx, 2) || !ctx->pyro2_continuity_good ||
        !drogue_failing(ctx, now, ctx->pyro1_fire_time))
        return;
    if (fire_channel(ctx, 2, now)) {
        ctx->main_forced = true;
        log_event(ctx, EVT_MAIN_FORCED);
    }
}

#define LAND_STEP_MAX_CM 100   /* [FLT-LAND-01] */
#define LAND_SPEED_MAX_CMS 200 /* [FLT-LAND-02, FLT-LAND-07] */
#define LAND_AGL_MAX_CM 3000   /* [FLT-LAND-03] */
#define LAND_HOLD_MS 1000u     /* [FLT-LAND-01, FLT-LAND-07] */

/* [FLT-DESC-02, FLT-LAND-01..03, FLT-LAND-07, DD-015] */
static bool landing_detected(flight_context_t *ctx, uint32_t now, int32_t prev_altitude) {
    int32_t altitude = ctx->last_altitude;
    bool still = abs(ctx->vertical_speed_cms) < LAND_SPEED_MAX_CMS;
    bool resting = abs(altitude - prev_altitude) < LAND_STEP_MAX_CM && still && altitude < LAND_AGL_MAX_CM;
    if (held(resting, &ctx->landing_stable_since, ctx->last_sample, LAND_HOLD_MS))
        return true;

    /* [FLT-LAND-07] Above the pad's elevation AGL never comes near zero. Not
     * on a failed sensor: a stuck one reads as still. */
    uint32_t timeout_s = ctx->config.landing_timeout;
    bool timed_out =
        timeout_s > 0 && ctx->descent_start_time > 0 && (now - ctx->descent_start_time) >= timeout_s * 1000;
    return held(timed_out && still && !ctx->fit_suspect, &ctx->still_since, ctx->last_sample, LAND_HOLD_MS);
}

/* The sample becomes last_altitude before any trigger is tested; the previous
 * altitude is handed back for landing detection. */
static bool descent_sample(flight_context_t *ctx, uint32_t now, flight_state_t st, int32_t *prev_out) {
    watch_sensor(ctx, now);
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return false;
    ctx->sensor_lost = false;
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    ctx->vertical_speed_cms = sample_speed(ctx, &sample);
    ctx->air_scale = pp_air_scale(ctx->filtered_pressure, ctx->ground_pressure);
    take_fit(ctx, &sample);
    buf_add(ctx, sample.timestamp_ms - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, st);
    note_stuck(ctx, &sample);
    ctx->last_sample = sample.timestamp_ms;
    *prev_out = ctx->last_altitude;
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_height = sample.height_cm;
    return true;
}

/* Both channels may fire here: a low flight puts both out on one event. */
static state_event_t detect_falling(flight_context_t *ctx, uint32_t now) {
    int32_t prev;
    if (!descent_sample(ctx, now, FALLING, &prev))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, ctx->last_sample, &band);

    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    if (landing_detected(ctx, now, prev))
        return SEVT_LANDING;
    if (settled && band == BAND_MAIN)
        return SEVT_CHUTE;
    if (settled && band == BAND_DROGUE)
        return SEVT_DROGUE;
    return SEVT_NONE;
}

/* A rate above the drogue band, held, sends the machine back to free fall. */
static state_event_t detect_drogue_descent(flight_context_t *ctx, uint32_t now) {
    int32_t prev;
    if (!descent_sample(ctx, now, DROGUE_DESCENT, &prev))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, ctx->last_sample, &band);

    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    if (landing_detected(ctx, now, prev))
        return SEVT_LANDING;
    if (settled && band == BAND_MAIN)
        return SEVT_CHUTE;
    if (band_exceeded(ctx, ctx->last_sample, DESC_DROGUE_CMS))
        return SEVT_FREEFALL;
    return SEVT_NONE;
}

/* try_fire_pyros() runs here too: the phase is a diagnosis, not a licence to
 * cancel the flight plan. */
static state_event_t detect_chute_descent(flight_context_t *ctx, uint32_t now) {
    int32_t prev;
    if (!descent_sample(ctx, now, CHUTE_DESCENT, &prev))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, ctx->last_sample, &band);
    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    return landing_detected(ctx, now, prev) ? SEVT_LANDING : SEVT_NONE;
}

#define LANDED_ROW_MS 1000u

/* [FLT-LAND-06, FLT-RATE-04] */
static state_event_t detect_landed(flight_context_t *ctx, uint32_t now) {
    (void)now;
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    ctx->filtered_pressure = pp_last_filtered_pa();
    /* A row a second, so the ring keeps the flight's events. */
    if (ctx->landed_row_ms == 0 || sample.timestamp_ms - ctx->landed_row_ms >= LANDED_ROW_MS) {
        buf_add(ctx, sample.timestamp_ms - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, LANDED);
        ctx->landed_row_ms = sample.timestamp_ms;
    }
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_height = sample.height_cm;
    ctx->last_sample = sample.timestamp_ms;
    return SEVT_NONE;
}

/* ── Transition actions ───────────────────────────────────────────── */

/* Forever (a repeat count of 0): silent would look like a pass. */
static void say_fault(void) {
    beep_spec_t sp = beep_for(BR_SYSTEM_FAILURE);
    const beep_personality_t *p = beep_codes_active(beep_store_current());
    buzzer_play_spec(&sp, p->gap_ms, 0);
}

static void action_fault(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->last_reason = (uint8_t)BR_SYSTEM_FAILURE;
    if (!grounded_on_usb(ctx))
        say_fault();
}

static void action_cal_init(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    pp_start_cal();
}

static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    ctx->boot_timer = now; /* the pad marker's dwell is time on the pad */
    ctx->ground_pressure = pp_ground_pressure();
    ctx->filtered_pressure = ctx->ground_pressure;
}

/* [FLT-LAUNCH-03..05, GND-CAL-04/05] */
static void action_launch(flight_context_t *ctx, uint32_t now) {
    buzzer_stop();
    ctx->launch_time = ctx->pad_rising ? ctx->pad_rise_ms : now; /* [FLT-LAUNCH-03] */
    (void)pp_ground_freeze_before(ctx->launch_time);
    ctx->ground_pressure = pp_ground_pressure();

    /* The base of the first ASCENT speed. */
    ctx->last_altitude = pp_pressure_to_altitude_cm(ctx->filtered_pressure, ctx->ground_pressure);
    ctx->last_height = pp_pressure_to_height_cm(ctx->filtered_pressure, ctx->ground_pressure);

    /* The ring's newest sample is a PAD_IDLE one, which the log does not
     * take, so the LAUNCH row is written here. */
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(ctx->last_sample - ctx->launch_time, ctx->filtered_pressure, ctx->last_altitude, ASCENT, 0,
                   EVT_LAUNCH);
    buf_tag_event(ctx, EVT_LAUNCH);
    /* [FLT-MACH-02] Flagged on the climb before the launch was sure. */
    if (ctx->mach_lock) {
        extern void hal_telemetry_send(const char *sentence);
        hal_log_sample(ctx->last_sample - ctx->launch_time, ctx->filtered_pressure, ctx->last_altitude, ASCENT, 0,
                       EVT_MACH_LOCK);
        hal_telemetry_send("!MACH LOCK\r\n");
    }
}

/* [FLT-BROWN-02] A rocket already descending is past apogee. */
static void action_recovered_descent(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = now;
    ctx->descent_start_time = now;
    ctx->pyros_armed = true;
    ctx->armed_time = now;
    buf_tag_event(ctx, EVT_APOGEE);
}

/* [FLT-ASC-05, DD-017] */
static void action_armed(flight_context_t *ctx, uint32_t now) {
    ctx->pyros_armed = true;
    ctx->armed_time = now;
    buf_tag_event(ctx, EVT_ARMED);
}

/* [FLT-APO-02/03] */
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = ctx->apogee_fit_ms ? ctx->apogee_fit_ms : now; /* [PYR-MODE-05] */
    /* [FLT-MACH-07] A lock let go this close to apogee may have hidden the
     * top of the climb from the peak. */
    ctx->peak_lower_bound = ctx->mach_released && ctx->apogee_time - ctx->mach_release_ms < MACH_LOWER_BOUND_MS;
    ctx->descent_start_time = now; /* [FLT-LAND-07] */
    buf_tag_event(ctx, EVT_APOGEE);
    telemetry_apogee(ctx->max_altitude, now - ctx->launch_time);
}

/* [FLT-LAND-05, BUZ-03, DAT-06] */
static void action_landing(flight_context_t *ctx, uint32_t now) {
    ctx->landing_time = now;
    buf_tag_event(ctx, EVT_LANDING);
    telemetry_landing(ctx->max_altitude, now - ctx->launch_time);
    buzzer_play_altitude(cm_to_units(ctx->max_altitude, ctx->config.units));
    hal_log_stop();
}

/* ── Ground test [GND-TEST-05..11] ────────────────────────────────── */

static void action_ground_test(flight_context_t *ctx, uint32_t now) {
    extern void hal_telemetry_send(const char *sentence);
    bool p1 = channel_expects_igniter(ctx, 1), p2 = channel_expects_igniter(ctx, 2);
    gt_seq_begin(&ctx->gt_seq, p1, p2, hal_ground_test_asserted(), now);
    char line[40];
    snprintf(line, sizeof(line), "!GT MODE p1=%s p2=%s\r\n", p1 ? "on" : "off", p2 ? "on" : "off");
    hal_telemetry_send(line);
}

/* [GND-TEST-11] Terminal until the next power-up. The continuity is still
 * sampled, quietly: MK1C fires only a channel its presence test has seen. */
static state_event_t detect_ground_test(flight_context_t *ctx, uint32_t now) {
    extern void hal_telemetry_send(const char *sentence);
    hal_continuity_t c1, c2;
    sample_continuity(ctx, now, &c1, &c2);

    gt_phase_t was = ctx->gt_seq.phase;
    gt_action_t a = gt_seq_step(&ctx->gt_seq, hal_ground_test_asserted(), now);
    if (a.sound != GT_SOUND_NONE)
        buzzer_play_ground_test(a.sound);
    char line[40];
    if (a.fire) {
        pyro_fire_result_t r = flight_pyro_energise(a.fire);
        gt_fire_t g = r == PYRO_BUSY ? GT_FIRE_BUSY : (r == PYRO_ENERGISED ? GT_FIRE_ENERGISED : GT_FIRE_REFUSED);
        gt_seq_fired(&ctx->gt_seq, a.fire, g, now);
        if (g != GT_FIRE_BUSY) {
            snprintf(line, sizeof(line), "!GT FIRE %u %s\r\n", (unsigned)a.fire,
                     g == GT_FIRE_ENERGISED ? "fired" : "refused");
            hal_telemetry_send(line);
        }
    }
    if (ctx->gt_seq.phase != was) {
        snprintf(line, sizeof(line), "!GT %s\r\n", gt_seq_phase_name(ctx->gt_seq.phase));
        hal_telemetry_send(line);
    }
    return SEVT_NONE;
}

/* ── State machine ────────────────────────────────────────────────── */

static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_SETTLE] = detect_boot_settle,
    [BOOT_CONTINUITY] = detect_boot_continuity,
    [BOOT_CALIBRATE] = detect_boot_calibrate,
    [PAD_IDLE] = detect_pad_idle,
    [ASCENT] = detect_ascent,
    [FALLING] = detect_falling,
    [DROGUE_DESCENT] = detect_drogue_descent,
    [CHUTE_DESCENT] = detect_chute_descent,
    [LANDED] = detect_landed,
    [BOOT_SENSOR] = detect_boot_sensor,
    [FAULT] = detect_fault,
    [GROUND_TEST] = detect_ground_test,
};

static const transition_t transitions[] = {
    {BOOT_SETTLE, SEVT_TIMER, BOOT_SENSOR, NULL},
    {BOOT_SENSOR, SEVT_DONE, BOOT_CONTINUITY, NULL},
    /* No calibration in the air. Armed on the descent path only: one still
     * climbing goes through the arming gate like any other flight. */
    {BOOT_SENSOR, SEVT_RECOVER_ASCENT, ASCENT, NULL},
    {BOOT_SENSOR, SEVT_RECOVER_DESCENT, FALLING, action_recovered_descent},
    {BOOT_SENSOR, SEVT_FAULT, FAULT, action_fault},
    {BOOT_CALIBRATE, SEVT_FAULT, FAULT, action_fault},
    {BOOT_CONTINUITY, SEVT_DONE, BOOT_CALIBRATE, action_cal_init},
    {BOOT_CONTINUITY, SEVT_GROUND_TEST, GROUND_TEST, action_ground_test},
    {BOOT_CALIBRATE, SEVT_CAL_DONE, PAD_IDLE, action_ground_cal},
    {PAD_IDLE, SEVT_LAUNCH, ASCENT, action_launch},
    {ASCENT, SEVT_ARMED, ASCENT, action_armed},
    {ASCENT, SEVT_APOGEE, FALLING, action_apogee},
    {FALLING, SEVT_DROGUE, DROGUE_DESCENT, NULL},
    /* A low flight puts both canopies out on one event. */
    {FALLING, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    {DROGUE_DESCENT, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    /* A drogue that shreds: back to where the ladder can escalate. */
    {DROGUE_DESCENT, SEVT_FREEFALL, FALLING, NULL},
    /* [FLT-DESC-02] */
    {FALLING, SEVT_LANDING, LANDED, action_landing},
    {DROGUE_DESCENT, SEVT_LANDING, LANDED, action_landing},
    {CHUTE_DESCENT, SEVT_LANDING, LANDED, action_landing},
};

#define NUM_TRANSITIONS (sizeof(transitions) / sizeof(transitions[0]))

/* A state number the machine does not know (corrupt RAM) cannot be flown on,
 * and is said as a fault: PAD_IDLE would re-open launch detection mid-flight
 * and beep "ready" with nothing behind it. */
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state >= STATE_COUNT) {
        action_fault(ctx, now);
        return FAULT;
    }

    detect_fn detect = detectors[ctx->current_state];
    if (!detect)
        return ctx->current_state;

    state_event_t evt = detect(ctx, now);
    if (evt == SEVT_NONE)
        return ctx->current_state;

    for (int i = 0; i < (int)NUM_TRANSITIONS; i++) {
        if (transitions[i].from == ctx->current_state && transitions[i].event == evt) {
            if (transitions[i].action)
                transitions[i].action(ctx, now);
            return transitions[i].to;
        }
    }
    return ctx->current_state;
}

/* ── CSV export ───────────────────────────────────────────────────── */

int flight_save_csv(flight_context_t *ctx) {
    if (ctx->buf_count == 0)
        return -1;
    hal_file_t *f = hal_fs_open("flight.csv", false);
    if (!f)
        return -1;

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "# " PYRO_BOARD_NAME " Flight Data\n# ID: %.8s\n# Name: %.8s\n"
                     "# Pyro1: %s %u\n# Pyro2: %s %u\n"
                     "# Units: %s\n# Ground Pa: %ld\n# Max Alt cm: %ld\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
                     ctx->config.id, ctx->config.name, config_mode_name(ctx->config.pyro1_mode),
                     ctx->config.pyro1_value, config_mode_name(ctx->config.pyro2_mode), ctx->config.pyro2_value,
                     ctx->config.units == 2   ? "ft"
                     : ctx->config.units == 1 ? "m"
                                              : "cm",
                     (long)ctx->ground_pressure, (long)ctx->max_altitude);
    hal_fs_write(f, line, n);

    uint16_t idx = ctx->buf_tail;
    for (int i = 0; i < ctx->buf_count; i++) {
        flight_sample_t *s = &ctx->flight_buffer[idx];
        n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)s->time_ms, (long)s->pressure_pa,
                     (long)s->altitude_cm, s->state, s->under_thrust, flight_event_name(s->event));
        hal_fs_write(f, line, n);
        idx = (idx + 1) % FLIGHT_BUF_SIZE;
    }
    hal_fs_close(f);
    return 0;
}

/* ── Init and output ──────────────────────────────────────────────── */

static flight_context_t *g_flight_ctx = NULL;

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    config_set_defaults(&ctx->config);

    hal_config_load(&ctx->config); /* [FLT-BOOT-02] */
    telemetry_init(&ctx->config);
    buzzer_init();
    beep_store_load(NULL, 0); /* the shipped table, until the file is read */
    pp_init();
    /* Read before anything can reset the registers [FLT-BROWN-02]. */
    ctx->reset_cause = (uint8_t)hal_reset_cause();
    hal_pressure_init();
    int sensor = hal_pressure_sensor();
    ctx->sensor_type = sensor < 0 ? SENSOR_PENDING : (uint8_t)sensor;
    ctx->fs_ok = hal_fs_healthy();
    hal_pyro_init(); /* [FLT-BOOT-06] */
    ctx->boot_timer = hal_time_ms();

    ctx->current_state = BOOT_SETTLE;
    ground_test_init(&ctx->gt);
    g_flight_ctx = ctx;
}

/* ── Config reload ──────────────────────────────────────────────── */

cfg_apply_t flight_config_apply(flight_context_t *ctx, const config_t *new_config) {
    if (ctx->current_state != PAD_IDLE)
        return CFG_NOT_ON_PAD;
    if (new_config->pyro1_mode > PYRO_MODE_DELAY || new_config->pyro2_mode > PYRO_MODE_DELAY || new_config->units > 2)
        return CFG_INVALID;
    ctx->config = *new_config;
    telemetry_init(&ctx->config);
    return CFG_APPLIED;
}

cfg_apply_t flight_config_reload(flight_context_t *ctx) {
    if (ctx->current_state != PAD_IDLE)
        return CFG_NOT_ON_PAD;
    config_t new_config;
    config_set_defaults(&new_config);
    if (hal_config_load(&new_config) < 0)
        return CFG_LOAD_FAILED;
    return flight_config_apply(ctx, &new_config);
}

flight_context_t *flight_get_context(void) {
    return g_flight_ctx;
}

flight_state_t flight_get_state(void) {
    return g_flight_ctx ? g_flight_ctx->current_state : BOOT_SETTLE;
}

/* Sets, not orderings: BOOT_SENSOR and FAULT are numbered after LANDED, so a
 * `>= PAD_IDLE` test includes them. */
static bool state_is_airborne(flight_state_t st) {
    switch (st) {
    case ASCENT:
    case FALLING:
    case DROGUE_DESCENT:
    case CHUTE_DESCENT:
        return true;
    default:
        return false;
    }
}

/* [TEL-04, TEL-05] The ground-station contract has six states and no FAULT:
 * state 0 would tell a tracker a booting or failed board is ready to fly. */
static bool state_sends_telemetry(flight_state_t st) {
    return st == PAD_IDLE || st == LANDED || state_is_airborne(st);
}

static void grounding_changed(flight_context_t *ctx, bool was, uint32_t now) {
    bool is = grounded_on_usb(ctx);
    /* [GND-TEST-06] The ground test's buzzer is its countdown: an attach
     * chirp must not cut it off. */
    if (is == was || ctx->current_state == GROUND_TEST)
        return;
    if (is) {
        buzzer_play_usb_ok();
        ctx->buzzer_started = false;
        return;
    }
    switch (ctx->current_state) {
    case PAD_IDLE:
        /* The bench was not the pad: the marker's dwell starts again. */
        ctx->boot_timer = now;
        ctx->marker_written = false;
        break;
    case LANDED:
        buzzer_play_altitude(cm_to_units(ctx->max_altitude, ctx->config.units));
        break;
    case FAULT:
        say_fault();
        break;
    default:
        break;
    }
}

void flight_set_usb_attached(flight_context_t *ctx, bool attached, uint32_t now) {
    if (attached == ctx->usb_attached || state_is_airborne(ctx->current_state))
        return;
    bool was = grounded_on_usb(ctx);
    ctx->usb_attached = attached;
    grounding_changed(ctx, was, now);
}

void flight_set_test_mode(flight_context_t *ctx, bool on, uint32_t now) {
    if (on == ctx->test_mode || state_is_airborne(ctx->current_state))
        return;
    bool was = grounded_on_usb(ctx);
    ctx->test_mode = on;
    grounding_changed(ctx, was, now);
}

const char *flight_recovery_text(const flight_context_t *ctx) {
    if (ctx->recovery == RECOVER_COLD)
        return brownout_cold_name((cold_reason_t)ctx->recovery_why);
    return brownout_recovery_name((recovery_t)ctx->recovery);
}

uint32_t flight_elapsed_ms(const flight_context_t *ctx, uint32_t now) {
    if (state_is_airborne(ctx->current_state))
        return now - ctx->launch_time;
    if (ctx->current_state == LANDED)
        return ctx->landing_time - ctx->launch_time;
    return 0;
}

const char *flight_diag_name(uint16_t bit) {
    switch (bit) {
    case DIAG_SENSOR_FAIL:
        return "sensor_fail";
    case DIAG_FS_FAIL:
        return "fs_fail";
    case DIAG_CFG_RANGE:
        return "cfg_range";
    case DIAG_P1_OPEN:
        return "pyro1_open";
    case DIAG_P1_SHORT:
        return "pyro1_short";
    case DIAG_P2_OPEN:
        return "pyro2_open";
    case DIAG_P2_SHORT:
        return "pyro2_short";
    case DIAG_BROWNOUT:
        return "brownout_recovered";
    case DIAG_SENSOR_STUCK:
        return "sensor_stuck";
    case DIAG_SENSOR_LOST:
        return "sensor_lost";
    default:
        return "";
    }
}

/* See docs/ground-station-interface-spec.md "4. Telemetry State Codes (the Fixed Contract)". */
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

/* [TEL-03, CFG-SUBSYS-01] The in-flight cadence is telem_rate_hz, bounded by
 * what the UART can carry; 0 takes the TEL-03 default. */
#define TELEM_DEFAULT_HZ 10
#define TELEM_MAX_HZ 50

static uint32_t telem_interval_ms(const config_t *cfg) {
    uint32_t hz = cfg->telem_rate_hz ? cfg->telem_rate_hz : TELEM_DEFAULT_HZ;
    if (hz > TELEM_MAX_HZ)
        hz = TELEM_MAX_HZ;
    return 1000u / hz;
}

#define FAULT_REPORT_MS 5000 /* [TEL-05] */

static void report_fault(flight_context_t *ctx, uint32_t now) {
    if (ctx->last_telemetry != 0 && now - ctx->last_telemetry < FAULT_REPORT_MS)
        return;
    char line[96];
    int n = snprintf(line, sizeof(line), "!FAULT");
    for (uint16_t bit = 1; bit != 0 && n > 0 && n < (int)sizeof(line) - 24; bit <<= 1) {
        if ((ctx->diag & bit) && *flight_diag_name(bit))
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", flight_diag_name(bit));
    }
    snprintf(line + n, sizeof(line) - (size_t)n, "\r\n");
    hal_telemetry_send(line);
    ctx->last_telemetry = now;
}

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    if (ctx->current_state == FAULT) {
        report_fault(ctx, now);
        return;
    }
    if (!state_sends_telemetry(ctx->current_state))
        return;

    uint32_t interval = state_is_airborne(ctx->current_state) ? telem_interval_ms(&ctx->config) : 1000;
    if (now - ctx->last_telemetry < interval)
        return;
    telemetry_snapshot_t snap = {
        .seq = ctx->telemetry_seq,
        .state_id = state_to_telem_id(ctx->current_state),
        .thrust = (ctx->current_state == ASCENT && ctx->under_thrust) ? 1u : 0u,
        .alt_cm = ctx->last_altitude,
        .max_alt_cm = ctx->max_altitude,
        .speed_cms = ctx->vertical_speed_cms,
        .press_pa = ctx->filtered_pressure,
        .p1_adc = ctx->pyro1_adc,
        .p2_adc = ctx->pyro2_adc,
        .time_ms = flight_elapsed_ms(ctx, now),
        .flags = 0,
    };
    if (ctx->pyro1_continuity_good)
        snap.flags |= TELEM_FLAG_P1_CONT;
    if (ctx->pyro2_continuity_good)
        snap.flags |= TELEM_FLAG_P2_CONT;
    if (ctx->pyro1_fired)
        snap.flags |= TELEM_FLAG_P1_FIRED;
    if (ctx->pyro2_fired)
        snap.flags |= TELEM_FLAG_P2_FIRED;
    if (ctx->pyros_armed)
        snap.flags |= TELEM_FLAG_ARMED;
    if (ctx->apogee_detected)
        snap.flags |= TELEM_FLAG_APOGEE;
    telemetry_state(&snap);
    ctx->telemetry_seq++;
    ctx->last_telemetry = now;
}
