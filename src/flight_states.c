/*
 * Flight state machine: event-driven with transition table.
 * See REQUIREMENTS.md for requirement definitions.
 * See TRACEABILITY.md for requirement-to-test mapping.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

/* Overridden by src/lua/lua_app.c where Lua is linked. Weak and defined here
 * rather than in the HAL so that every build resolves it -- the host test
 * binaries link this file without hal_common.c. A board with no Lua is ready
 * the moment the flight code asks. */
__attribute__((weak)) bool lua_app_ready_or_absent(void) {
    return true;
}
#include "board_id.h"
#include "flight_states.h"
#include "brownout.h"
#include "pressure_processing.h"
#include "telemetry_formatter.h"
#include "ground_test.h"
#include "buzzer.h"
#include "beep_store.h"
#include "pyro_release.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ring buffer ──────────────────────────────────────────────────── */

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    /* v2-9: log every in-flight sample; events tagged later by buf_tag_event */
    if ((flight_state_t)st >= ASCENT) {
        hal_log_sample(time_ms, pressure, altitude, st, ctx->under_thrust, EVT_NONE);
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
    s->under_thrust = 0;
    s->event = EVT_NONE;
    s->event_data = 0;
    ctx->buf_head = (ctx->buf_head + 1) % FLIGHT_BUF_SIZE;
    ctx->buf_count++;
    /* csv_track_sample() removed: incremental logger retired, hal_log owns flight record [v2-9] */
}

static void buf_tag_event(flight_context_t *ctx, uint8_t event) { /* [DAT-03] */
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].event = event;
    /* v2-9: also emit a timestamped event line to the HAL log */
    if (hal_log_active()) {
        const flight_sample_t *s = &ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE];
        if ((flight_state_t)s->state >= ASCENT)
            hal_log_sample(s->time_ms, s->pressure_pa, s->altitude_cm, s->state, s->under_thrust, event);
    }
}

#define MAX_ALTITUDE_CM 800000

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

/* [PYR-MODE-01..04, PYR-ALT-01] See IMPLEMENTATION.md "Altitude Limitations"
 * for behavior above 8000m. */
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected)
        return false; /* [PYR-SAFE-04] */
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    int32_t clamped = ((int32_t)value > max_units) ? max_units : (int32_t)value;
    int32_t fallen = cm_to_units(ctx->max_altitude - ctx->last_altitude, ctx->config.units);
    int32_t agl = cm_to_units(ctx->last_altitude, ctx->config.units);
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

/* [PYR-SAFE-01..03] */
/* [PYR-DEPLOY-02] The one place a channel is energised.
 *
 * Both igniters draw through one common FET and one fuse, so only one may be
 * live at a time -- on MK1B that path is a 1.5 A self-resetting PTC and the
 * combined draw can trip it and fire neither. A refusal here is not a failure:
 * the caller runs again next tick and the other channel will have finished.
 * That is also why every fire site goes through this function rather than
 * calling hal_pyro_fire() directly. */
static bool fire_channel(flight_context_t *ctx, int ch, uint32_t now) {
    if (hal_pyro_is_firing())
        return false;
    hal_pyro_fire(ch);
    if (ch == 1) {
        ctx->pyro1_fired = true;
        ctx->pyro1_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO1_FIRE);
    } else {
        ctx->pyro2_fired = true;
        ctx->pyro2_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO2_FIRE);
    }
    telemetry_pyro_fire(ch, ctx->last_altitude, now - ctx->launch_time);
    return true;
}

static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
    if (!ctx->pyro1_fired && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value))
        (void)fire_channel(ctx, 1, now);
    if (!ctx->pyro2_fired && ctx->pyro2_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value))
        (void)fire_channel(ctx, 2, now);
}

/* [PYR-FAULT-02/03] Check FLAG pin after fire for overcurrent */
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

/* [PYR-VERIFY-01] Post-fire continuity check: if pyro didn't open, it failed */

/* Each channel's verify window opens 500-600 ms after THAT channel fired, so
 * the two windows generally do not coincide. */
static bool verify_window_open(bool fired, bool already_failed, uint32_t fire_time, uint32_t now) {
    return fired && !already_failed && fire_time > 0 && now - fire_time > 500 && now - fire_time < 600;
}

static void check_post_fire_verify(flight_context_t *ctx, uint32_t now) {
    bool w1 = verify_window_open(ctx->pyro1_fired, ctx->pyro1_verify_fail, ctx->pyro1_fire_time, now);
    bool w2 = verify_window_open(ctx->pyro2_fired, ctx->pyro2_verify_fail, ctx->pyro2_fire_time, now);
    if (!w1 && !w2)
        return;

    /* One stimulus event serves whichever windows are open. */
    hal_pyro_sample();
    hal_continuity_t c;

    if (w1) {
        hal_pyro_get(1, &c);
        if (c.good && !c.open) {
            ctx->pyro1_verify_fail = true;
            buf_tag_event(ctx, EVT_PYRO1_NOPEN);
        }
    }
    if (w2) {
        hal_pyro_get(2, &c);
        if (c.good && !c.open) {
            ctx->pyro2_verify_fail = true;
            buf_tag_event(ctx, EVT_PYRO2_NOPEN);
        }
    }
}

/* check_refire() retired -- the bounded ladder below does this [DD-023] */

/* Config parser moved to config.c — X-macro generated [CFG-TABLE-01] */

/* ── Event detectors ──────────────────────────────────────────────── */

/* [FLT-BOOT-04, FLT-BOOT-09] */
static state_event_t detect_boot_settle(flight_context_t *ctx, uint32_t now) {
    return (now - ctx->boot_timer >= 2500) ? SEVT_TIMER : SEVT_NONE;
}

/* The first power-up test is the sensor.
 *
 * Testing the pyros first and beeping "all good" leaves an operator on the pad
 * with a board that cannot measure altitude -- it will never detect launch,
 * never arm and never deploy. The continuity verdict is worth nothing until
 * this one has passed, so it comes first and a failure is terminal. */
static bool assess_recovery(flight_context_t *ctx, uint32_t now, state_event_t *evt);

static state_event_t detect_boot_sensor(flight_context_t *ctx, uint32_t now) {
    extern void hal_telemetry_send(const char *sentence);
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
    /* The sensor is good, so the barometer may now be believed about whether
     * this board is airborne. The verdict needs two samples for a speed, so
     * this state lingers until it has them -- returning SEVT_DONE on the first
     * tick is what made an earlier version decide on one sample, which is to
     * say on no speed at all, which is to say never. */
    state_event_t rec = SEVT_NONE;
    if (!assess_recovery(ctx, now, &rec)) {
        return SEVT_NONE;
    }
    if (rec != SEVT_NONE) {
        return rec;
    }
    return SEVT_DONE;
}

/* Terminal. The board keeps serving HTTP and telemetry so the failure can be
 * diagnosed, but the flight machine goes no further and the buzzer repeats the
 * code until power is removed -- an operator at the pad has no console. */
static state_event_t detect_fault(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    return SEVT_NONE;
}

/* ── Brownout recovery [FLT-BOOT-11] ──────────────────────────────
 *
 * Runs once the sensor has been proved, because every question here is asked
 * of the barometer and an unproved barometer can answer anything.
 *
 * The altitude is measured against the marker's ground pressure, not against
 * a fresh calibration -- calibrating is exactly what must not happen while
 * airborne, since it would define the current altitude as zero and take the
 * rocket's remaining height with it. */
/* However long the sensor takes, the boot path may not hang here: a board that
 * cannot answer within this gets the cold-boot verdict and carries on. */
#define RECOVERY_DEADLINE_MS 4000u

static bool assess_recovery(flight_context_t *ctx, uint32_t now, state_event_t *evt) {
    *evt = SEVT_NONE;
    pad_marker_t m;
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    bool ok = (n == (int)sizeof(m)) && pad_marker_valid(&m);
    if (!ok) {
        ctx->recovery = (uint8_t)RECOVER_COLD;
        return true; /* nothing to recover: decided, and decided now */
    }
    if (now - ctx->boot_timer >= RECOVERY_DEADLINE_MS) {
        ctx->recovery = (uint8_t)RECOVER_COLD;
        return true;
    }
    ctx->marker_ground_pa = m.ground_pressure_pa;

    altitude_sample_t sample;
    if (!pp_read(&sample)) {
        return false; /* no sample yet; asked again next tick */
    }

    /* Two references, deliberately.
     *
     * The RATE comes from the queued sample's own altitude, measured against
     * whatever reference pp happens to be using before calibration -- that
     * reference is constant, and a constant cancels in a difference. The
     * LEVEL has to come from the marker's ground pressure, because that is
     * the only ground this board still knows about.
     *
     * Mixing them is what an earlier version did: it took the level from the
     * newest filtered pressure and the timestamp from an older queued sample,
     * so the two disagreed about which instant they described and the speed
     * came out as zero. */
    int32_t alt_pp = sample.altitude_cm;
    int32_t alt_agl = pp_pressure_to_altitude_cm(pp_last_filtered_pa(), m.ground_pressure_pa);

    bool have_pair = ctx->recovery_samples > 0 && sample.timestamp_ms > ctx->last_sample;
    int32_t speed = 0;
    if (have_pair) {
        speed = (alt_pp - ctx->last_altitude) * 1000 / (int32_t)(sample.timestamp_ms - ctx->last_sample);
    }
    ctx->last_altitude = alt_pp;
    ctx->last_sample = sample.timestamp_ms;
    if (ctx->recovery_samples < UINT8_MAX) {
        ctx->recovery_samples++;
    }
    /* One sample is not a speed. The deadline above is what stops this state
     * waiting forever for a second one. */
    if (!have_pair) {
        return false;
    }

    recovery_t r = brownout_assess((reset_cause_t)ctx->reset_cause, true, alt_agl, speed);
    ctx->recovery = (uint8_t)r;
    if (r != RECOVER_ASCENT && r != RECOVER_DESCENT) {
        return true;
    }

    /* The flight the log was recording is gone with the RAM that held it, so
     * this is a new log opened mid-air. T+0 is the moment of recovery, which
     * is the only launch time this board can still honestly claim. */
    ctx->diag |= DIAG_BROWNOUT;
    ctx->ground_pressure = m.ground_pressure_pa;
    pp_ground_track(false);
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->launch_time = now;
    ctx->max_altitude = alt_agl;
    ctx->last_altitude = alt_agl;
    ctx->vertical_speed_cms = speed;
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(0, ctx->filtered_pressure, alt_agl, ASCENT, 0, EVT_LAUNCH);
    *evt = (r == RECOVER_ASCENT) ? SEVT_RECOVER_ASCENT : SEVT_RECOVER_DESCENT;
    return true;
}

/* [SYS-STATUS-02] */
static state_event_t detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    (void)now;
    hal_continuity_t c1, c2;
    hal_pyro_sample();
    hal_pyro_get(1, &c1);
    hal_pyro_get(2, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->boot_timer = now;
    return SEVT_DONE;
}

/* [FLT-BOOT-08] Calibration is handled by pressure_processing layer.
 * pp_start_cal() was called in action_cal_init(); we just poll pp_cal_done().
 * Timeout after 10s if sensor fails to provide samples. */
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (pp_cal_done())
        return SEVT_CAL_DONE;

    /* A sensor that answered at init but produces no samples.
     *
     * This used to force PAD_IDLE, which left pp stuck in PP_CALIBRATING so
     * pp_read() never yielded a sample: the board sat on the pad looking
     * healthy, beeped "all good", and could not have detected a launch. */
    if (now - ctx->boot_timer >= 10000) {
        extern void hal_telemetry_send(const char *sentence);
        hal_telemetry_send("!CAL TIMEOUT - sensor produced no samples\r\n");
        ctx->diag |= DIAG_SENSOR_FAIL;
        return SEVT_FAULT;
    }

    return SEVT_NONE;
}

/* What the pad check found, as DIAG_* bits. Several can be true at once.
 *
 * Separate from the beep because they are different questions: this is what
 * is wrong, and beep_reason_for_diag() below is what to do about it. */
static void collect_pad_faults(flight_context_t *ctx, const hal_continuity_t *c1, const hal_continuity_t *c2) {
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    if ((ctx->config.pyro1_mode != PYRO_MODE_DELAY && ctx->config.pyro1_value > max_units) ||
        (ctx->config.pyro2_mode != PYRO_MODE_DELAY && ctx->config.pyro2_value > max_units)) {
        ctx->diag |= DIAG_CFG_RANGE;
    }
    /* A released channel is a Lua output, not a firing path. Its mocked
     * continuity reads open by design, and reporting "check the pyro" for a
     * pad the operator deliberately gave away is a false alarm they cannot
     * clear. */
    if (!c1->good && !pyro_release_is_released(1)) {
        ctx->diag |= c1->open ? DIAG_P1_OPEN : DIAG_P1_SHORT;
    }
    if (!c2->good && !pyro_release_is_released(2)) {
        ctx->diag |= c2->open ? DIAG_P2_OPEN : DIAG_P2_SHORT;
    }
}

/* Which outcome to say.
 *
 * Four, because four is the number of things an operator can do standing at a
 * rocket: fly it, check igniter 1, check igniter 2, or safe it and walk away.
 * Anything that cannot be fixed at the pad outranks anything that can -- a
 * board with both a dead sensor and an open igniter sends the operator away,
 * because adjusting the igniter would not help.
 *
 * Channel 1 is named before channel 2 when both are bad. One trip to the
 * rocket covers both, and the screen names both. */
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

static void update_continuity_and_buzzer(flight_context_t *ctx, uint32_t now) { /* [PYR-CONT-01, PYR-ALT-02] */
    if (now - ctx->last_cont_check <= 1000)
        return;
    hal_continuity_t c1, c2;
    hal_pyro_sample();
    hal_pyro_get(1, &c1);
    hal_pyro_get(2, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->pyro1_adc = c1.raw_adc;
    ctx->pyro2_adc = c2.raw_adc;
    ctx->last_cont_check = now;

    if (ctx->buzzer_started)
        return;

    /* Hold the startup beep until the board is genuinely up.
     *
     * Core1's startup -- compiling the script and running init() -- is
     * unbounded, and core0 writes no flash for its duration. So a beeping,
     * blinking board should mean "a script is running", not "a script is
     * still compiling": the beep is the only indication an operator has at
     * the pad without a console.
     *
     * Always true when Lua is not in play, so boards without it are
     * unaffected. */
    if (!lua_app_ready_or_absent())
        return;

    ctx->buzzer_started = true;
    collect_pad_faults(ctx, &c1, &c2);

    /* Say it on the active personality's cadence, which by default keeps
     * saying it until launch. A board that speaks once and falls silent is
     * indistinguishable from one whose battery died a second later. */
    ctx->last_reason = (uint8_t)beep_reason_for_diag(ctx->diag);
    beep_say((beep_reason_t)ctx->last_reason);
}

/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-RATE-01, DD-016]
 * Strengthened launch confirmation requires ALL of:
 *   1. Filtered altitude > 10m (1000cm)
 *   2. Vertical speed > 5 m/s (500 cm/s)
 * This prevents false launch from barometric drift or thermal expansion. */
/* The sensor produces a sample about every 20 ms; the loop runs at 10. */
#define PAD_SAMPLE_MS 20
#define LAUNCH_ALT_CM 3048 /* 100 ft above the frozen ground reference */
#define LAUNCH_SPEED_CMS 500

/* [FLT-BOOT-11] The pad marker, written once and only here.
 *
 * Ten seconds of PAD_IDLE, because the point is to have written it long
 * before the moment it protects against. Launch shock -- a battery connector
 * bouncing -- is the likeliest cause of the brownout this exists to survive,
 * and a flash write in progress is the worst possible moment to lose power.
 * So nothing writes flash at launch, and this is what makes that affordable:
 * the ground reference is already safe on disk before the motor lights.
 *
 * A failure costs the recovery path and nothing else, so it is not retried
 * and not reported as an error -- the flight is unaffected either way. */
static void write_pad_marker(flight_context_t *ctx, uint32_t now) {
    if (ctx->marker_written || now - ctx->boot_timer < PAD_MARKER_DWELL_MS) {
        return;
    }
    ctx->marker_written = true; /* once, whatever the write does */
    pad_marker_t m;
    pad_marker_fill(&m, pp_ground_pressure());
    (void)hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m));
}

static state_event_t detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    /* [GND-TEST-01..04, DD-011] Poll serial for ground test commands.
     * Processed before the sample-rate gate so commands drain promptly. */
    char cmd_buf[64];
    if (hal_serial_readline(cmd_buf, sizeof(cmd_buf)))
        ground_test_handle_command(&ctx->gt, cmd_buf, ctx, now);
    ground_test_update(&ctx->gt, now);

    if (now - ctx->last_sample < 10)
        return SEVT_NONE;

    update_continuity_and_buzzer(ctx, now);
    write_pad_marker(ctx, now);

    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE; /* no altitude sample available yet */
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;

    /* Track speed on the pad for launch confirmation [DD-016] */
    uint32_t dt = (ctx->last_sample > 0) ? (ts - ctx->last_sample) : 10;
    if (dt > 0)
        ctx->pad_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    ctx->filtered_pressure = pp_last_filtered_pa(); /* for telemetry/debug */

    /* [GND-CAL-01] The ground reference is a 5-second rolling mean of the
     * filtered pressure, kept by the pressure layer and fed from every
     * sample. It used to be a 60-second IIR computed here; it moved because
     * the reference belongs with the pressure it is derived from, and because
     * a boxcar forgets -- the value frozen at launch is the mean of the last
     * five seconds, with nothing older leaking in. */
    ctx->ground_pressure = pp_ground_pressure();

    buf_add(ctx, 0, ctx->filtered_pressure, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = ts;

    bool alt_ok = altitude > LAUNCH_ALT_CM;
    bool speed_ok = ctx->pad_speed_cms > LAUNCH_SPEED_CMS;
    return (alt_ok && speed_ok) ? SEVT_LAUNCH : SEVT_NONE;
}

/* [FLT-ASC-01..06, FLT-APO-01..04, FLT-RATE-02, DD-017]
 * DD-017: Arming gate — pyros arm only after max filtered speed exceeds
 *         threshold. The IIR filter (τ=500ms) attenuates measured speed
 *         by ~50% for short flights, so 10 m/s filtered ≈ 20 m/s true.
 *         Prevents false arming from barometric drift (~0 m/s filtered). */
#define ARM_SPEED_CMS 1000 /* 10 m/s filtered ≈ 20 m/s true [DD-017] */

/* [FLT-MACH-01] Apogee is not declared while the rocket is fast.
 *
 * Only an upward rush latches the gate. Testing the magnitude instead would
 * re-latch on the way down -- where speed climbs past the threshold again --
 * and lock apogee detection out for the rest of the flight, which is the
 * deadlock this gate would otherwise introduce. Descending fast is not a
 * reason to doubt that apogee happened; it is proof that it did. */
#define MACH_GATE_CMS 3048  /* 100 ft/s */
#define MACH_SETTLE_MS 1000 /* slow for this long before the sensor is believed */

/* The latch has to be fed from every ascent sample, not from the gate test.
 * The gate is only consulted once the pyros are armed, and arming already
 * requires the rocket to have slowed below 10 m/s -- so a latch that lived in
 * here could never see a speed above the threshold, and the gate would be
 * permanently open on exactly the flights it exists for. */
static void mach_gate_feed(flight_context_t *ctx) {
    if (ctx->vertical_speed_cms > MACH_GATE_CMS) {
        ctx->mach_exceeded = true;
        ctx->subsonic_since = 0;
    }
}

static bool mach_gate_clear(flight_context_t *ctx, uint32_t now) {
    if (!ctx->mach_exceeded)
        return true; /* never went fast, so nothing to wait out */
    if (ctx->vertical_speed_cms > MACH_GATE_CMS)
        return false;
    if (ctx->subsonic_since == 0)
        ctx->subsonic_since = now;
    return now - ctx->subsonic_since >= MACH_SETTLE_MS;
}

/* [DD-017] Arming requires confirmed motor burn: peak speed > threshold,
 * coast phase entered (speed decreasing but still positive). */
static bool arming_gate_met(const flight_context_t *ctx) {
    return !ctx->pyros_armed && ctx->max_speed_cms >= ARM_SPEED_CMS && ctx->vertical_speed_cms < 1000 &&
           ctx->vertical_speed_cms >= 0;
}

static state_event_t detect_ascent(flight_context_t *ctx, uint32_t now) {
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
    ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;

    /* Track peak speed for arming gate [DD-017] */
    if (ctx->vertical_speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = ctx->vertical_speed_cms;
    mach_gate_feed(ctx);

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, ASCENT);
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].under_thrust =
        ctx->under_thrust ? 1 : 0;

    if (altitude > ctx->max_altitude)
        ctx->max_altitude = altitude;
    ctx->last_altitude = altitude;
    /* The sample clock, not the loop clock. dt above is measured between
     * sample timestamps, so storing `now` here made every speed wrong by
     * however late the loop was running. */
    ctx->last_sample = ts;

    if (arming_gate_met(ctx))
        return SEVT_ARMED;

    /* [FLT-APO-01] Apogee is the sensor saying the rocket has stopped going
     * up, and nothing else. The backup timer that used to force it here was
     * removed [DD-022]: a wrong timer value fires during ascent, which is
     * worse than the sensor failure it was meant to cover. */
    if (ctx->pyros_armed && !ctx->apogee_detected && mach_gate_clear(ctx, now) && ctx->vertical_speed_cms <= 0)
        return SEVT_APOGEE;
    return SEVT_NONE;
}

/* ── Descent ──────────────────────────────────────────────────────────
 *
 * The phase is read from the rocket, not from the firing log. The old machine
 * left FALLING only on pyro1_fired and DROGUE_DESCENT only on pyro2_fired,
 * which meant a channel with no continuity, a channel set to NONE, or the two
 * firing out of order each parked the machine in a descent state for the rest
 * of the flight -- and since LANDED was reachable only from CHUTE_DESCENT,
 * hal_log_stop() was never called on exactly the flights whose log matters.
 *
 * What a working canopy looks like is a descent rate that has stopped
 * changing. What a failed one looks like is a rate that has not. So the phase
 * advances on a rate holding steady inside a band, and never on a command. */

#define DESC_DROGUE_CMS 3500 /* at or under 35 m/s, something is slowing us */
#define DESC_MAIN_CMS 1000   /* at or under 10 m/s, the main is out */

/* The dwell is what keeps free fall from being mistaken for a canopy. A
 * rocket in free fall gains ~11.8 m/s over this window, which breaks the
 * tolerance at every rate a canopy could explain. The tolerance is a fraction
 * of the rate, with a floor, because a drogue at 25 m/s breathes several m/s
 * while a main at 5 m/s does not. */
#define DESC_DWELL_MS 1200
#define DESC_TOL_MIN_CMS 250
#define DESC_TOL_FRAC 4
#define DESC_FAIL_MS 1000 /* a rate the phase cannot explain, held this long */

typedef enum { BAND_FAST = 0, BAND_DROGUE, BAND_MAIN } desc_band_t;

static int32_t descent_rate(const flight_context_t *ctx) {
    return ctx->vertical_speed_cms < 0 ? -ctx->vertical_speed_cms : ctx->vertical_speed_cms;
}

static desc_band_t descent_band(int32_t rate) {
    if (rate <= DESC_MAIN_CMS)
        return BAND_MAIN;
    if (rate <= DESC_DROGUE_CMS)
        return BAND_DROGUE;
    return BAND_FAST;
}

/* True once the rate has stayed in one band, and near one value, for the
 * dwell. Climbing does not count: just after apogee the rate passes through
 * the main band on its way to ballistic, and only the stability test keeps
 * that from reading as a deployed main. */
static bool descent_settled(flight_context_t *ctx, uint32_t now, desc_band_t *out) {
    int32_t v = ctx->vertical_speed_cms;
    if (v >= 0) {
        ctx->desc_band_since = 0;
        return false;
    }
    int32_t rate = descent_rate(ctx);
    desc_band_t b = descent_band(rate);
    int32_t tol = rate / DESC_TOL_FRAC;
    if (tol < DESC_TOL_MIN_CMS)
        tol = DESC_TOL_MIN_CMS;
    int32_t drift = v - ctx->desc_ref_cms;
    if (drift < 0)
        drift = -drift;

    if (b != (desc_band_t)ctx->desc_band || drift > tol || ctx->desc_band_since == 0) {
        ctx->desc_band = (uint8_t)b;
        ctx->desc_ref_cms = v;
        ctx->desc_band_since = now;
        return false;
    }
    *out = b;
    return now - ctx->desc_band_since >= DESC_DWELL_MS;
}

/* A canopy that has failed shows as a rate its phase cannot explain, held
 * long enough not to be a gust. Stability is deliberately not required: a
 * shredded drogue is accelerating, which is the whole point. */
static bool band_exceeded(flight_context_t *ctx, uint32_t now, int32_t ceiling) {
    if (descent_rate(ctx) <= ceiling) {
        ctx->desc_fail_since = 0;
        return false;
    }
    if (ctx->desc_fail_since == 0) {
        ctx->desc_fail_since = now;
        return false;
    }
    return now - ctx->desc_fail_since >= DESC_FAIL_MS;
}

/* [FLT-EMRG-01] What to do when a canopy does not answer.
 *
 * The drogue gets a grace period to bite, then one more attempt, then the
 * main goes out early whatever its configured trigger said. A main opened
 * high costs drift; a main that arrives too fast to open costs the rocket.
 * The budget is what makes this terminate -- the retired check_refire()
 * rewrote fire_time on every attempt and could retry for the whole descent. */
#define EMRG_DROGUE_GRACE_MS 2000
#define EMRG_MAX_REFIRE 1
/* A last resort, not a deployment policy. It only shortens the grace the
 * drogue is given; it never skips the retry and never fires over the top of a
 * trigger the flight has not reached yet. Set high deliberately: an ordinary
 * failed-drogue descent must reach the retry on the grace, not on this. */
#define EMRG_MAIN_PANIC_CMS 9000 /* 90 m/s */

/* canopy_working is deliberately not "settled": a rocket at terminal velocity
 * in free fall has a perfectly steady descent rate, and that steadiness is the
 * shredded-drogue case, not a success. Only settling inside a band a canopy
 * could explain counts. */
static void emergency_ladder(flight_context_t *ctx, uint32_t now, bool canopy_working) {
    /* The ladder answers a canopy that was commanded and did not work.
     *
     * It deliberately has no bare descent-rate trigger. A rocket in free fall
     * toward a trigger it has not reached yet is not failing, however fast it
     * is going -- free fall IS fast, and a rate trigger here would fire the
     * main over the top of any config whose drogue is set below apogee. */
    if (canopy_working || !ctx->pyro1_fired || ctx->pyro2_fired || !ctx->pyro2_continuity_good)
        return;

    /* Descending so fast that waiting out the rest of the grace cannot help. */
    bool panic = ctx->vertical_speed_cms < 0 && descent_rate(ctx) >= EMRG_MAIN_PANIC_CMS;
    if (!panic && now - ctx->pyro1_fire_time < EMRG_DROGUE_GRACE_MS)
        return; /* the canopy is still being given its chance to bite */

    /* Retry only where a retry can work: pyro1_verify_fail means the channel
     * never opened, so the charge did not light. A channel that opened fired
     * its charge and the canopy failed mechanically -- a second attempt on an
     * empty channel just spends altitude the main still needs. That flag was
     * previously collected and used for nothing but suppressing its own
     * re-check. */
    if (ctx->pyro1_verify_fail && ctx->pyro1_refires < EMRG_MAX_REFIRE) {
        if (fire_channel(ctx, 1, now))
            ctx->pyro1_refires++;
        return;
    }

    /* Out of drogue options and still not slowing: the main goes out early. */
    if (fire_channel(ctx, 2, now))
        ctx->main_forced = true;
}

/* [DD-015] Landing, checked in every descent state rather than only under the
 * main. A flight whose drogue never opened still lands, and the log has to be
 * closed on that flight too. */
#define LANDING_SPEED_CMS 500 /* 5 m/s — slow enough to be "landed" */

static bool landing_detected(flight_context_t *ctx, uint32_t now, int32_t altitude) {
    bool altitude_stable = abs(altitude - ctx->last_altitude) < 100;
    bool speed_low = abs(ctx->vertical_speed_cms) < 200;
    bool near_ground = altitude < 3000;

    if (altitude_stable && speed_low && near_ground) {
        if (ctx->landing_stable_since == 0)
            ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000)
            return true;
    } else {
        ctx->landing_stable_since = 0;
    }

    /* Force landing if descent has run long and the rocket is slow: handles
     * landing above the pad elevation, where AGL never returns near zero. */
    uint32_t timeout_s = ctx->config.landing_timeout;
    return timeout_s > 0 && ctx->descent_start_time > 0 && (now - ctx->descent_start_time) >= timeout_s * 1000 &&
           abs(ctx->vertical_speed_cms) < LANDING_SPEED_CMS;
}

/* Every descent state reads the same sample and derives the same speed; only
 * the verdict differs. dt is taken between sample timestamps -- mixing the
 * sample clock with the loop clock, as this did, made the speed wrong by
 * whatever the loop was running late by. last_altitude is left for the caller
 * because landing detection needs the previous value. */
static bool descent_sample(flight_context_t *ctx, uint32_t now, flight_state_t st, int32_t *alt_out) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return false;
    uint32_t dt = sample.timestamp_ms - ctx->last_sample;
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (sample.altitude_cm - ctx->last_altitude) * 1000 / (int32_t)dt;
    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, st);
    ctx->last_sample = sample.timestamp_ms;
    *alt_out = sample.altitude_cm;
    return true;
}

/* Free fall: no canopy is working yet. Both channels may still fire here --
 * a low flight puts drogue and main out on the same event. */
static state_event_t detect_falling(flight_context_t *ctx, uint32_t now) {
    int32_t altitude;
    if (!descent_sample(ctx, now, FALLING, &altitude))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, now, &band);

    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    state_event_t evt = SEVT_NONE;
    if (landing_detected(ctx, now, altitude))
        evt = SEVT_LANDING;
    else if (settled && band == BAND_MAIN)
        evt = SEVT_CHUTE;
    else if (settled && band == BAND_DROGUE)
        evt = SEVT_DROGUE;

    ctx->last_altitude = altitude;
    return evt;
}

/* Under drogue. The main may still be pending, and the drogue may still fail:
 * a rate above the drogue band, held, sends the machine back to free fall
 * where the ladder can escalate. */
static state_event_t detect_drogue_descent(flight_context_t *ctx, uint32_t now) {
    int32_t altitude;
    if (!descent_sample(ctx, now, DROGUE_DESCENT, &altitude))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, now, &band);

    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    state_event_t evt = SEVT_NONE;
    if (landing_detected(ctx, now, altitude))
        evt = SEVT_LANDING;
    else if (settled && band == BAND_MAIN)
        evt = SEVT_CHUTE;
    else if (band_exceeded(ctx, now, DESC_DROGUE_CMS))
        evt = SEVT_FREEFALL;

    ctx->last_altitude = altitude;
    return evt;
}

/* Under the main.
 *
 * try_fire_pyros() runs here too, and must. The phase is a diagnosis, not a
 * licence to cancel the flight plan: a rocket already descending slowly still
 * gets the deployment its config asked for. Leaving it out meant a flight
 * whose descent merely looked main-like -- a big drogue, a light airframe --
 * silently skipped a configured main, which is the firmware overriding the
 * operator on the strength of an inference. */
static state_event_t detect_chute_descent(flight_context_t *ctx, uint32_t now) {
    int32_t altitude;
    if (!descent_sample(ctx, now, CHUTE_DESCENT, &altitude))
        return SEVT_NONE;

    desc_band_t band = BAND_FAST;
    bool settled = descent_settled(ctx, now, &band);
    try_fire_pyros(ctx, now);
    check_pyro_fault(ctx);
    check_post_fire_verify(ctx, now);
    emergency_ladder(ctx, now, settled && band != BAND_FAST);

    state_event_t evt = landing_detected(ctx, now, altitude) ? SEVT_LANDING : SEVT_NONE;
    ctx->last_altitude = altitude;
    return evt;
}

/* [FLT-LAND-06, FLT-RATE-04] */
static state_event_t detect_landed(flight_context_t *ctx, uint32_t now) {
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    /* Landed: just log occasional samples for telemetry */
    ctx->filtered_pressure = pp_last_filtered_pa();
    /* Only add to ring buffer every 1 second to prevent overwriting events */
    if (now - ctx->last_sample >= 1000)
        buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, LANDED);
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_sample = sample.timestamp_ms;
    return SEVT_NONE;
}

/* ── Transition actions ───────────────────────────────────────────── */

/* Repeat forever: a repeat count of 0 is infinite and nothing clears it. There
 * is no recovery from a failed power-up test, so the board must not fall silent
 * and look like it passed. */
static void action_fault(flight_context_t *ctx, uint32_t now) {
    (void)now;
    /* Until power is removed: there is no recovery from a failed power-up
     * test, so the board must not fall silent and look like it passed. */
    ctx->last_reason = (uint8_t)BR_SYSTEM_FAILURE;
    beep_spec_t sp = beep_for(BR_SYSTEM_FAILURE);
    const beep_personality_t *p = beep_codes_active(beep_store_current());
    buzzer_play_spec(&sp, p->gap_ms, 0);
}

static void action_cal_init(flight_context_t *ctx, uint32_t now) {
    (void)ctx;
    (void)now;
    pp_start_cal(); /* pressure_processing layer handles calibration */
}

static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    /* Restarted here so the pad marker's dwell measures time sat on the pad,
     * not time since boot -- calibration's own duration should not count
     * toward it. */
    ctx->boot_timer = now;
    ctx->ground_pressure = pp_ground_pressure();
    ctx->filtered_pressure = ctx->ground_pressure;
    /* last_altitude is already 0 from flight_init() — correct for ground.
     * No need to set last_sample or filter state — pressure_processing
     * owns the filter and ring buffer now. Altitude samples will arrive
     * in chronological order via pp_read(). */
}

/* [FLT-LAUNCH-03..05] Backdate launch time to first sample above 50cm */
static void action_launch(flight_context_t *ctx, uint32_t now) {
    buzzer_stop();
    ctx->launch_time = now;
    for (int i = ctx->buf_count - 1; i >= 0; i--) {
        uint16_t idx = (ctx->buf_head - 1 - i + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE;
        if (ctx->flight_buffer[idx].altitude_cm <= 50) {
            /* PAD_IDLE samples arrive at the sensor's ~20 ms, not the 10 ms
             * loop period this assumed, so launch_time was backdated about
             * half the true elapsed time. */
            ctx->launch_time = now - (uint32_t)(ctx->buf_count - 1 - i) * PAD_SAMPLE_MS;
            break;
        }
    }
    /* [GND-CAL-02] Snap ground pressure to the current filtered pressure so
     * T+0 altitude is exactly 0 cm regardless of any residual tracker error.
     * ctx->ground_pressure (written to the log header) now records the true
     * atmospheric pressure at the moment of launch. */
    /* Freeze the reference; do not snap it to here.
     *
     * Snapping made T+0 altitude zero by definition, which threw away the
     * 100 ft the rocket had already climbed to trip the detector. Freezing
     * keeps it, so apogee and every AGL threshold are that much truer. */
    pp_ground_track(false);
    ctx->ground_pressure = pp_ground_pressure();

    /* The rocket is ~100 ft up, not at zero. Starting the first ASCENT speed
     * calculation from zero produced one enormous sample. */
    ctx->last_altitude = pp_pressure_to_altitude_cm(ctx->filtered_pressure, ctx->ground_pressure);

    /* v2-9: start log BEFORE tagging — LAUNCH sample is PAD_IDLE state
     * (below the >= ASCENT guard in buf_tag_event), so emit it directly.
     * time_ms = 0 = T+0 relative to launch. */
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(0, ctx->filtered_pressure, 0, ASCENT, 0, EVT_LAUNCH);
    buf_tag_event(ctx, EVT_LAUNCH); /* tags ring buffer for flight_save_csv() */
}

/* Rejoining a flight already on its way down. Apogee is behind us by
 * definition -- the rocket is descending -- so the pyros are armed and the
 * descent machine takes it from here, emergency ladder and all. */
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

/* [FLT-APO-03, DD-014] Force CSV flush at apogee */
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = now;
    ctx->descent_start_time = now; /* [DD-015] start landing timeout */
    buf_tag_event(ctx, EVT_APOGEE);
    telemetry_apogee(ctx->max_altitude, now - ctx->launch_time);
    /* pyro firing handled by detect_falling() on first tick in FALLING state */
    /* [DD-014] hal_log async task flushes to flash every 200ms — no
     * explicit apogee flush needed. hal_log_sample() already wrote the
     * APOGEE event line immediately above. */
}

/* [FLT-LAND-05, BUZ-03, DAT-06] */
static void action_landing(flight_context_t *ctx, uint32_t now) {
    buf_tag_event(ctx, EVT_LANDING);
    telemetry_landing(ctx->max_altitude, now - ctx->launch_time);
    buzzer_set_altitude(cm_to_units(ctx->max_altitude, ctx->config.units));
    ctx->landed_beep_started = true;
    ctx->csv_saved = true;
    /* v2-9: finalize flight log — flush remaining buffer to flash */
    hal_log_stop();
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
};

static const transition_t transitions[] = {
    {BOOT_SETTLE, SEVT_TIMER, BOOT_SENSOR, NULL},
    {BOOT_SENSOR, SEVT_DONE, BOOT_CONTINUITY, NULL},
    /* Straight into the flight machine, skipping calibration -- calibrating
     * is what must not happen while airborne. Armed on the descent path only:
     * a rocket already coming down has passed apogee whatever the lost RAM
     * used to think, while one still climbing goes through the normal arming
     * gate and apogee detection like any other flight. */
    {BOOT_SENSOR, SEVT_RECOVER_ASCENT, ASCENT, NULL},
    {BOOT_SENSOR, SEVT_RECOVER_DESCENT, FALLING, action_recovered_descent},
    {BOOT_SENSOR, SEVT_FAULT, FAULT, action_fault},
    {BOOT_CALIBRATE, SEVT_FAULT, FAULT, action_fault},
    {BOOT_CONTINUITY, SEVT_DONE, BOOT_CALIBRATE, action_cal_init},
    {BOOT_CALIBRATE, SEVT_CAL_DONE, PAD_IDLE, action_ground_cal},
    {PAD_IDLE, SEVT_LAUNCH, ASCENT, action_launch},
    {ASCENT, SEVT_ARMED, ASCENT, action_armed},
    {ASCENT, SEVT_APOGEE, FALLING, action_apogee},
    {FALLING, SEVT_DROGUE, DROGUE_DESCENT, NULL},
    /* A low flight puts both canopies out on one event and never shows a
     * drogue-rate phase, so free fall must be able to reach the main. */
    {FALLING, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    {DROGUE_DESCENT, SEVT_CHUTE, CHUTE_DESCENT, NULL},
    /* A drogue that shreds sends the machine back to free fall, where the
     * ladder can escalate. Without this the phase would be a one-way ratchet
     * that claimed a canopy was working long after it had gone. */
    {DROGUE_DESCENT, SEVT_FREEFALL, FALLING, NULL},
    /* Landing from every descent state. When only CHUTE_DESCENT could reach
     * LANDED, a flight that never deployed anything never called
     * hal_log_stop() and lost the record of why. */
    {FALLING, SEVT_LANDING, LANDED, action_landing},
    {DROGUE_DESCENT, SEVT_LANDING, LANDED, action_landing},
    {CHUTE_DESCENT, SEVT_LANDING, LANDED, action_landing},
};

#define NUM_TRANSITIONS (sizeof(transitions) / sizeof(transitions[0]))

flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state >= STATE_COUNT)
        return PAD_IDLE;

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

static const char *event_name(uint8_t evt) {
    switch (evt) {
    case EVT_LAUNCH:
        return "LAUNCH";
    case EVT_ARMED:
        return "ARMED";
    case EVT_APOGEE:
        return "APOGEE";
    case EVT_PYRO1_FIRE:
        return "PYRO1";
    case EVT_PYRO2_FIRE:
        return "PYRO2";
    case EVT_LANDING:
        return "LANDING";
    default:
        return "";
    }
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
    case PYRO_MODE_DELAY:
        return "delay";
    case PYRO_MODE_AGL:
        return "agl";
    case PYRO_MODE_FALLEN:
        return "fallen";
    case PYRO_MODE_SPEED:
        return "speed";
    default:
        return "none";
    }
}

/* [DAT-06, DAT-07] Batch CSV export (fallback, used by simulator) */
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
                     ctx->config.id, ctx->config.name, mode_name(ctx->config.pyro1_mode), ctx->config.pyro1_value,
                     mode_name(ctx->config.pyro2_mode), ctx->config.pyro2_value,
                     ctx->config.units == 2   ? "ft"
                     : ctx->config.units == 1 ? "m"
                                              : "cm",
                     (long)ctx->ground_pressure, (long)ctx->max_altitude);
    hal_fs_write(f, line, n);

    uint16_t idx = ctx->buf_tail;
    for (int i = 0; i < ctx->buf_count; i++) {
        flight_sample_t *s = &ctx->flight_buffer[idx];
        n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)s->time_ms, (long)s->pressure_pa,
                     (long)s->altitude_cm, s->state, s->under_thrust, event_name(s->event));
        hal_fs_write(f, line, n);
        idx = (idx + 1) % FLIGHT_BUF_SIZE;
    }
    hal_fs_close(f);
    return 0;
}

/* ── Init and output ──────────────────────────────────────────────── */

/* Global flight context pointer for runtime config reload and HTTP server access */
static flight_context_t *g_flight_ctx = NULL;

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    config_set_defaults(&ctx->config);

    /* Runs once before the main loop, so there is no separate BOOT_INIT
     * state. */
    hal_config_load(&ctx->config);
    telemetry_init(&ctx->config);
    buzzer_init();
    /* Before anything can ask for a code. On the host and the simulator there
     * is no file, so this publishes the shipped table. */
    beep_store_load(NULL, 0);
    pp_init();
    /* Why this boot happened, read before anything can reset the registers.
     * A brownout reads the same as someone connecting the battery, which is
     * what the pad marker is for. */
    ctx->reset_cause = (uint8_t)hal_reset_cause();
    /* Captured, not discarded. 0 means no sensor answered, and BOOT_SENSOR
     * turns that into a terminal fault rather than a board that beeps "all
     * good" and then never detects a launch. */
    ctx->sensor_type = (uint8_t)hal_pressure_init();
    ctx->fs_ok = hal_fs_healthy();
    hal_pyro_init();
    ctx->boot_timer = hal_time_ms();

    ctx->current_state = BOOT_SETTLE;
    ground_test_init(&ctx->gt);

    /* Store global reference for config reload and HTTP server access */
    g_flight_ctx = ctx;
}

/* ── Config reload (runtime config update) ────────────────────────── */

int flight_config_reload(flight_context_t *ctx) {
    /* Safety check: only allow reload in PAD_IDLE state */
    if (ctx->current_state != PAD_IDLE) {
        return -1; /* Rejected: not in safe state */
    }

    /* Load config from persistent storage */
    config_t new_config;
    config_set_defaults(&new_config);
    if (hal_config_load(&new_config) < 0) {
        return -2; /* Rejected: load failed */
    }

    /* Validate critical fields to prevent invalid configurations */
    if (new_config.pyro1_mode > PYRO_MODE_DELAY || new_config.pyro2_mode > PYRO_MODE_DELAY) {
        return -3; /* Rejected: invalid pyro mode */
    }
    if (new_config.units > 2) {
        return -3; /* Rejected: invalid units */
    }

    /* Apply new configuration */
    ctx->config = new_config;

    /* Reinitialize telemetry with new config (updates headers, format, etc.) */
    telemetry_init(&ctx->config);

    return 0; /* Success */
}

flight_context_t *flight_get_context(void) {
    return g_flight_ctx;
}

flight_state_t flight_get_state(void) {
    return g_flight_ctx ? g_flight_ctx->current_state : BOOT_SETTLE;
}

/* Map flight_state_t to the 0-5 telemetry state_id (spec v1.2) */
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

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    /* The buzzer is autonomous: hal_tasks_tick() drives it. */

    if (ctx->current_state >= PAD_IDLE) {
        /* ASCENT..CHUTE_DESCENT are contiguous — all high-rate states */
        uint32_t interval = (ctx->current_state >= ASCENT && ctx->current_state <= CHUTE_DESCENT) ? 100 : 1000;
        if (now - ctx->last_telemetry >= interval) {
            uint32_t flight_time = (ctx->current_state != PAD_IDLE) ? (now - ctx->launch_time) : 0;
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
                .time_ms = flight_time,
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
    }
}
