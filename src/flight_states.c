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
#include "pressure_fit.h"
#include "mach_lockout.h"
#include "telemetry_formatter.h"
#include "ground_test.h"
#include "buzzer.h"
#include "beep_store.h"
#include "pyro_release.h"
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

/* [CFG-SUBSYS-01] log_rate_hz thins the samples written; events are always
 * written. At or above the sensor's rate every sample goes in. */
#define SENSOR_RATE_HZ 50

static bool log_sample_due(flight_context_t *ctx, uint32_t time_ms) {
    uint8_t hz = ctx->config.log_rate_hz;
    if (hz == 0 || hz >= SENSOR_RATE_HZ)
        return true;
    if (time_ms - ctx->last_logged_ms < 1000u / hz)
        return false;
    ctx->last_logged_ms = time_ms;
    return true;
}

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (state_is_logged((flight_state_t)st) && log_sample_due(ctx, time_ms)) {
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

#define MAX_ALTITUDE_CM 800000

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

/* [PYR-MODE-01..05, PYR-ALT-01] See IMPLEMENTATION.md "Altitude Limitations"
 * for behavior above 8000m. AGL and FALLEN read the fit's height, which does
 * not lag; DELAY counts from where the fit put the apogee. */
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

/* [PYR-DEPLOY-02] Both igniters draw through one common FET and one fuse, so
 * only one may be live at a time -- on MK1B that path is a 1.5 A
 * self-resetting PTC and the combined draw can trip it and fire neither.
 * BUSY is not a failure: the caller asks again next tick, and the other
 * channel's pulse will have ended. */
pyro_fire_result_t flight_pyro_energise(uint8_t channel) {
    if (hal_pyro_is_firing())
        return PYRO_BUSY;
    hal_pyro_fire(channel);
    return hal_pyro_is_firing() ? PYRO_ENERGISED : PYRO_REFUSED;
}

/* [PYR-SAFE-01..03, SYS-DEPLOY-01] Every in-flight fire goes through here.
 *
 * A channel is recorded as fired only when the board energised it. A refusal
 * -- MK1C before its firing sequence exists, or a channel released to Lua --
 * is recorded as one, and the channel is not asked again. */
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
    if (ch == 1)
        ctx->pyro1_fired = true;
    else
        ctx->pyro2_fired = true;
    buf_tag_event(ctx, ch == 1 ? EVT_PYRO1_FIRE : EVT_PYRO2_FIRE);
    telemetry_pyro_fire(ch, ctx->last_altitude, now - ctx->launch_time);
    return true;
}

static bool channel_waiting(bool fired, bool refused, bool continuity) {
    return !fired && !refused && continuity;
}

/* [DD-048] The pressure triggers act on a clean fit. An unclean one is a bad
 * reading or two the median let through, a charge pressurising the bay -- for
 * a moment the rocket reads hundreds of metres lower -- or a canopy swinging:
 * waited out for at most this long, then believed, so a swing that spoils
 * every fit cannot hold back the main. The wait restarts at each charge. */
#define UNCLEAN_WAIT_MS 2000u

/* A charge pressurises the bay, which reads the rocket lower than it is; a
 * canopy opening reads it higher than a ballistic fall would. So after a
 * charge an unclean fit that reads no lower than the last clean fit carried on
 * ballistically is believed: a charge cannot make the rocket fall faster, and
 * a main set just below a fast drogue must not wait two seconds for the
 * opening's shock to leave the window. */
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
    /* A failed sensor is waited out for as long as it takes: it must never
     * cause a deployment [SNS-PRES-10, SNS-PRES-11]. */
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

/* A channel whose trigger was met stays due while the other's pulse holds
 * the common path [PYR-DEPLOY-02]: by the time it ends, that channel's charge
 * has spoiled the fit, and the trigger must not be asked again. */
static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
    if (channel_waiting(ctx->pyro1_fired, ctx->pyro1_refused, ctx->pyro1_continuity_good) &&
        (ctx->pyro1_due || trigger_met(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value, now)))
        ctx->pyro1_due = !fire_channel(ctx, 1, now) && !ctx->pyro1_refused;
    if (channel_waiting(ctx->pyro2_fired, ctx->pyro2_refused, ctx->pyro2_continuity_good) &&
        (ctx->pyro2_due || trigger_met(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value, now)))
        ctx->pyro2_due = !fire_channel(ctx, 2, now) && !ctx->pyro2_refused;
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
static void read_continuity(flight_context_t *ctx);

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
    /* The sensor is good, so the barometer may now be believed about whether
     * this board is airborne. The verdict needs 600 ms of history for a speed,
     * so this state lingers until it has it. */
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

/* ── Brownout recovery [FLT-BROWN-02] ─────────────────────────────
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

/* The verdict is read from the pressure layer's history, which runs from
 * power-on. The level is the median of the newest 250 ms; the speed is that
 * against the median of a window ending 350 ms earlier. Medians, because the
 * history is only the median of three: two bad readings in a row pass it, and
 * on the pad one of them would read as a flight in progress. Two-reading
 * speed noise, 1.7 m/s RMS, sits too close to the 5 m/s threshold. */
#define RECOVERY_LEVEL_MS 250u
#define RECOVERY_SPAN_MS 600u
#define RECOVERY_MIN_READINGS 8

static void mach_flag(flight_context_t *ctx, int32_t p, uint32_t ts);

/* [FLT-MACH-06] A recovered ascent has lost its speed history and cannot
 * know whether the rocket is supersonic, so it starts locked, flagged at the
 * pressure it rejoined at. */
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
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
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

    /* The level is measured against the marker's ground: that is the only
     * ground this board still knows about. */
    int32_t alt_agl = pp_pressure_to_altitude_cm(level, m.ground_pressure_pa);
    int32_t alt_before = pp_pressure_to_altitude_cm(before, m.ground_pressure_pa);
    int32_t speed = (alt_agl - alt_before) * 1000 / (int32_t)(RECOVERY_SPAN_MS - RECOVERY_LEVEL_MS);

    recovery_t r = brownout_assess((reset_cause_t)ctx->reset_cause, true, alt_agl, speed);
    ctx->recovery = (uint8_t)r;
    if (r == RECOVER_COLD)
        return recovery_cold(ctx, ctx->reset_cause == RESET_POWER_EVENT ? COLD_AT_GROUND : COLD_NOT_POWER);
    if (r != RECOVER_ASCENT && r != RECOVER_DESCENT)
        return true;

    /* The flight the log was recording is gone with the RAM that held it, so
     * this is a new log opened mid-air. T+0 is the moment of recovery, which
     * is the only launch time this board can still honestly claim. The
     * pressure layer starts here, against the marker's ground, never
     * calibrated: calibrating would call this height zero. */
    pp_resume_flight(m.ground_pressure_pa, level);
    pp_set_sigma((float)m.sigma_mpa / 1000.0f);
    /* BOOT_CONTINUITY is skipped on this path, and a channel whose continuity
     * was never read is never fired [PYR-SAFE-01]. */
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

/* [SYS-STATUS-02] */
static state_event_t detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    read_continuity(ctx);
    ctx->boot_timer = now;
    return SEVT_DONE;
}

/* [FLT-BOOT-08] Calibration is handled by pressure_processing layer.
 * pp_start_cal() was called in action_cal_init(); we just poll pp_cal_done().
 * Timeout after 10s if sensor fails to provide samples. */
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (pp_cal_done())
        return SEVT_CAL_DONE;

    /* A sensor that answered at init but produces no samples. PAD_IDLE is
     * not an option: pp would stay in PP_CALIBRATING, pp_read() would never
     * yield a sample, and a board that beeps "all good" could not detect a
     * launch. */
    if (now - ctx->boot_timer >= 10000) {
        extern void hal_telemetry_send(const char *sentence);
        hal_telemetry_send("!CAL TIMEOUT - sensor produced no samples\r\n");
        ctx->diag |= DIAG_SENSOR_FAIL;
        return SEVT_FAULT;
    }

    return SEVT_NONE;
}

/* What the pad check finds, as DIAG_* bits. Several can be true at once.
 *
 * Separate from the beep because they are different questions: this is what
 * is wrong, and beep_reason_for_diag() below is what to do about it. */
static bool mode_is_altitude(uint8_t mode) {
    return mode == PYRO_MODE_AGL || mode == PYRO_MODE_FALLEN || mode == PYRO_MODE_SPEED;
}

/* A channel with an igniter the flight software means to fire. A released
 * channel is a Lua output and a disabled one has nothing connected by design:
 * their open continuity is the expected reading, and "check the pyro" would be
 * a false alarm the operator cannot clear. */
static bool channel_expects_igniter(const flight_context_t *ctx, uint8_t ch) {
    uint8_t mode = ch == 1 ? ctx->config.pyro1_mode : ctx->config.pyro2_mode;
    return mode != PYRO_MODE_NONE && !pyro_release_is_released(ch);
}

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

    /* [FLT-BOOT-15] Re-derived on every check, so the diagnosis describes the
     * same instant as the continuity it came from. */
    ctx->diag = (uint16_t)((ctx->diag & ~DIAG_PAD_ANY) | pad_faults(ctx, &c1, &c2));

    /* [USB-02] Diagnosed, for the screen, but not said. */
    if (grounded_on_usb(ctx))
        return;

    /* Hold the first announcement until the board is genuinely up.
     *
     * Core1's startup -- compiling the script and running init() -- is
     * unbounded, and core0 writes no flash for its duration. So a beeping,
     * blinking board should mean "a script is running", not "a script is
     * still compiling": the beep is the only indication an operator has at
     * the pad without a console.
     *
     * Always true when Lua is not in play, so boards without it are
     * unaffected. */
    if (!ctx->buzzer_started && !lua_app_ready_or_absent())
        return;

    /* Said again whenever the answer changes: an igniter lead that lets go
     * during a long wait on the pad must stop the board saying OK to fly, and
     * one that is fixed must stop it saying otherwise. Said on the active
     * personality's cadence, which by default repeats until launch -- a board
     * that speaks once and falls silent is indistinguishable from one whose
     * battery died a second later. */
    beep_reason_t r = beep_reason_for_diag(ctx->diag);
    if (ctx->buzzer_started && r == (beep_reason_t)ctx->last_reason)
        return;
    ctx->buzzer_started = true;
    ctx->last_reason = (uint8_t)r;
    beep_say(r);
}

/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-LAUNCH-06, DD-016] Launch is declared
 * when the filtered altitude is more than 100 ft above the frozen-at-launch
 * ground reference AND the rocket is climbing faster than 5 m/s. The height
 * is what rejects weather drift; the speed is what rejects a slow rise. */
#define LAUNCH_ALT_CM 3048 /* 100 ft */
#define LAUNCH_SPEED_CMS 500
#define LAUNCH_RISE_CM 50 /* [FLT-LAUNCH-03] T+0 is the first sample above this */

/* [GND-CAL-06] Every sample rejected for this long, with the board still, is
 * a new ground: re-seed. Long enough that no gust lasts it; short enough that
 * the reference is not wrong for long after the rocket is set down. */
#define GND_RESEED_MS 5000u
#define GND_STILL_CMS 100 /* 1 m/s */

/* [FLT-LAUNCH-07, FLT-APO-01] A trigger must hold, sample after sample, for
 * this long. A median of three stops one bad reading; two in a row reach the
 * detectors, and each trigger used to fire on one sample. Durations, never
 * sample counts: at another sample rate a count is another hold. */
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

/* [FLT-BROWN-01] The pad marker, written once, after ten seconds of PAD_IDLE.
 *
 * Ten seconds, because the point is to have written it long before the moment
 * it protects against. Launch shock -- a battery connector bouncing -- is the
 * likeliest cause of the brownout this exists to survive, and a flash write in
 * progress is the worst possible moment to lose power. So nothing writes flash
 * at launch, and this is what makes that affordable: the ground reference is
 * already safe on disk before the motor lights.
 *
 * Written from flight_flash_service(), not from the PAD_IDLE detector: on the
 * hardware the detector runs with the flash window shut, and a write there is
 * refused every time.
 *
 * A failure costs the recovery path and nothing else, so it is not retried
 * and not reported as an error -- the flight is unaffected either way. */
void flight_flash_service(flight_context_t *ctx, uint32_t now) {
    /* [FLT-BROWN-04] The flight is over, so the marker is spent: a power-up
     * after this, at the recovery site or back on the pad, must not recover
     * against it. hal.h has no delete, so an invalid marker is written. */
    if (ctx->current_state == LANDED && !ctx->marker_spent) {
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

/* [DD-048] Every detector's speed: the fit's, through the altitude formula's
 * slope at the fitted pressure. Short of a fit -- the first second after
 * power-on -- the two-point speed. */
static int32_t sample_speed(const flight_context_t *ctx, const altitude_sample_t *s) {
    return s->fit_valid ? s->speed_cms : two_point_speed(ctx, s);
}

/* [FLT-LAUNCH-07] Except the launch's. A burst of bad readings the median
 * lets through spoils every fit that holds it, for a second -- far longer
 * than the launch hold -- while the two-point speed spikes only for as long
 * as the burst. A real launch shows either. */
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

/* [SNS-PRES-10] A stuck sensor, said once each time it sticks; after the
 * sample's row, so the event carries its time. The DIAG bit stays: it is the
 * record that the flight had one. */
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

/* [SNS-PRES-11] No sample for this long in flight is a lost sensor. Nothing
 * is decided meanwhile, since every decision waits for a sample, and the
 * first fits after it are suspect until a whole window of new samples. */
#define SENSOR_LOST_MS 500u

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
    /* [GND-TEST-01..04, DD-011] Poll serial for ground test commands.
     * Processed before the sample-rate gate so commands drain promptly. */
    char cmd_buf[64];
    if (hal_serial_readline(cmd_buf, sizeof(cmd_buf)))
        ground_test_handle_command(&ctx->gt, cmd_buf, ctx, now);
    ground_test_update(&ctx->gt, now);

    update_continuity_and_buzzer(ctx, now);

    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE; /* no altitude sample available yet */
    int32_t altitude = sample.altitude_cm;
    uint32_t ts = sample.timestamp_ms;

    /* Track speed on the pad for launch confirmation [DD-016] */
    ctx->pad_speed_cms = launch_speed(ctx, &sample);
    take_fit(ctx, &sample);

    if (sample.rise_cm <= LAUNCH_RISE_CM) {
        ctx->pad_rising = false;
    } else if (!ctx->pad_rising) {
        ctx->pad_rising = true;
        ctx->pad_rise_ms = ts;
    }
    pad_mach_flag(ctx, &sample, ts);

    ctx->filtered_pressure = pp_last_filtered_pa(); /* for telemetry/debug */

    if (pp_ground_rejecting_ms(ts) >= GND_RESEED_MS && abs(ctx->pad_speed_cms) < GND_STILL_CMS) {
        extern void hal_telemetry_send(const char *sentence);
        pp_ground_reseed();
        hal_telemetry_send("!GND reseed\r\n");
        /* The marker records the ground this board now stands on. */
        ctx->boot_timer = now;
        ctx->marker_written = false;
    }

    /* [GND-CAL-01] The ground reference is the pressure layer's 5-second
     * rolling mean of the filtered pressure. A boxcar forgets: the value
     * frozen at launch is the last five seconds, with nothing older in it. */
    ctx->ground_pressure = pp_ground_pressure();

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

/* [FLT-ASC-01..06, FLT-APO-01..04, FLT-RATE-02, DD-017]
 * DD-017: Arming gate — pyros arm once the rocket has been faster than this
 *         and has slowed below it again, climbing: a burn, then a coast.
 *         The fit's speed is the true speed, and no drift reaches it. Still
 *         climbing at 10 m/s at the launch detector's 100 ft, a rocket
 *         reaches 35 m: every flight that can trip the detector arms. */
#define ARM_SPEED_CMS 1000 /* 10 m/s [DD-017] */

/* [FLT-APO-01, T5-A] Apogee needs the fitted pressure this far above the
 * lowest a clean fit showed: 0.6-0.9 m below the peak from sea level to 9 km,
 * about 0.4 s of fall. The fit's pressure noise is about half a pascal; at
 * 9 km the drop is 3 Pa, so noise cannot fake it. */
#define APOGEE_DROP 1.0001f

/* ── The Mach lockout [FLT-MACH-02..07, DD-049] ──────────────────────
 *
 * Past Mach 0.85 the static ports sit in disturbed flow, and until the
 * rocket is subsonic again their pressure is not the air's: it can make a
 * climbing rocket look slow, stopped or falling. So the flag goes up while
 * the data is still clean, and the data is not believed again until it has
 * shown, for a whole second, what only a subsonic coast shows: smooth,
 * climbing, slow, and slowing by gravity or more. The thresholds are pressure
 * ratios (mach_lockout.h); docs/mach_lockout.md derives each one. */
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

/* Before any release the flag takes any fit, and the rate over the newest
 * 40 ms besides: setting it is the safe direction, and through a 66 g boost's
 * first second a one-second fit still holds the pad and reads the climb
 * 120 m/s slow. After a release, only a clean fit: the coast has been seen,
 * and a bad reading must not lock out the apogee just ahead. */
static bool mach_flag_due(const flight_context_t *ctx, const altitude_sample_t *s, int32_t p) {
    if (s->fit_suspect)
        return false; /* a sensor coming back jumps: that is no climb [SNS-PRES-10] */
    if (ctx->mach_released)
        return s->fit_clean && mach_too_fast(p, s->fit_pdot);
    return mach_too_fast(p, s->fit_pdot) || mach_too_fast(p, s->short_pdot);
}

/* The flag is the climb's, from T+0: the launch detector wants 100 ft and
 * 100 ms, and a 66 g boost is past Mach 0.85 by then. It outlives a rise that
 * falls back, because a port faking a descent is one: supersonic, the rocket
 * can read below the pad. A flag that no launch follows for this long was a
 * bad reading. */
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

/* [DD-017] Arming requires confirmed motor burn: peak speed > threshold,
 * then slower than it, about 30 m up [FLT-MACH-06], on a sensor that has not
 * failed. Descending counts: a failed sensor near apogee must not close the
 * window for good, and arming late is safe -- apogee has its own tests. */
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

/* The sample's speed, thrust and row, and the arming height. */
static void ascent_take(flight_context_t *ctx, const altitude_sample_t *s) {
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    ctx->vertical_speed_cms = sample_speed(ctx, s);
    take_fit(ctx, s);
    ctx->under_thrust = s->fit_valid ? s->accel_cms2 > 0 : ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;
    /* Track peak speed for arming gate [DD-017] */
    if (ctx->vertical_speed_cms > ctx->max_speed_cms)
        ctx->max_speed_cms = ctx->vertical_speed_cms;
    int32_t p = s->fit_valid ? mach_round_clamp(s->fit_pa, MACH_RATE_CLAMP) : ctx->filtered_pressure;
    if (mach_above_arm_height(p, ctx->ground_pressure))
        ctx->arm_height = true;

    buf_add(ctx, s->timestamp_ms - ctx->launch_time, ctx->filtered_pressure, s->altitude_cm, ASCENT);
    ctx->flight_buffer[(ctx->buf_head - 1 + FLIGHT_BUF_SIZE) % FLIGHT_BUF_SIZE].under_thrust =
        ctx->under_thrust ? 1 : 0;
    ctx->last_altitude = s->altitude_cm;
    ctx->last_height = s->height_cm;
    /* The sample clock, not the loop clock: a speed short of a fit is taken
     * between sample timestamps, and `now` would put the loop's lateness into
     * it. */
    ctx->last_sample = s->timestamp_ms;
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

/* [FLT-APO-01, FLT-MACH-05] Apogee is the sensor saying the rocket has
 * stopped going up, and nothing else. No timer may force it [DD-022]: a wrong
 * value fires during ascent, which is worse than the sensor failure it would
 * cover. Said by clean fits whose pressure is rising, and has risen past the
 * drop, while no lock stands. */
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

/* ── Descent ──────────────────────────────────────────────────────────
 *
 * The phase is read from the rocket, not from the firing log [DD-023]. Do not
 * key a phase on pyroN_fired: a channel with no continuity, a channel set to
 * NONE, or the two firing out of order would each park the machine in a
 * descent state for the rest of the flight, and the log would never close.
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

static int32_t desc_tolerance(int32_t rate) {
    int32_t tol = rate / DESC_TOL_FRAC;
    return tol < DESC_TOL_MIN_CMS ? DESC_TOL_MIN_CMS : tol;
}

/* True once the rate has stayed in one band, and near one value, for the
 * dwell. Climbing does not count: just after apogee the rate passes through
 * the main band on its way to ballistic, and only the stability test keeps
 * that from reading as a deployed main. */
/* ts is sample time: the dwell is how long the samples have held steady. */
static bool descent_settled(flight_context_t *ctx, uint32_t ts, desc_band_t *out) {
    int32_t v = ctx->vertical_speed_cms;
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

/* A canopy that has failed shows as a rate its phase cannot explain, held
 * long enough not to be a gust. Stability is deliberately not required: a
 * shredded drogue is accelerating, which is the whole point. */
static bool band_exceeded(flight_context_t *ctx, uint32_t ts, int32_t ceiling) {
    if (descent_rate(ctx) <= ceiling) {
        ctx->desc_fail_since = 0;
        return false;
    }
    if (ctx->desc_fail_since == 0) {
        ctx->desc_fail_since = ts + 1u;
        return false;
    }
    return ts + 1u - ctx->desc_fail_since >= DESC_FAIL_MS;
}

/* [FLT-EMRG-01, PYR-REFIRE-01] What to do when a canopy does not answer.
 *
 * Two rungs. The retry answers a charge that never lit, and the evidence for
 * it is the channel's own post-fire continuity. The early main answers a
 * drogue that is not slowing the rocket, and the evidence for that is the
 * rocket: faster than any drogue explains, and not being slowed, for
 * DESC_FAIL_MS. Neither acts on the mere absence of a settled descent -- a
 * canopy opened at apogee starts from zero and is still accelerating toward its
 * terminal rate for seconds, so "not yet settled" is true of a working drogue
 * and would put the main out high on every flight. */
#define EMRG_DROGUE_GRACE_MS 2000 /* the drogue's chance to bite, per command */
#define EMRG_MAX_REFIRE 1
/* Shortens the retry's grace only. Set high deliberately: an ordinary
 * failed-drogue descent must reach the retry on the grace, not on this. */
#define EMRG_MAIN_PANIC_CMS 9000 /* 90 m/s */

/* The evidence the main is brought forward on. Measured only once the drogue
 * has had its grace: before that a rocket falling toward a trigger below
 * apogee is fast by design [FLT-EMRG-02], and one whose drogue has just opened
 * is still being slowed. A rate that falls by more than the tolerance is a
 * canopy biting, and starts the window again. */
/* The grace runs on the loop clock, from the fire; the hold on sample time,
 * because it is the samples that must keep showing the rate. */
static bool drogue_failing(flight_context_t *ctx, uint32_t now, uint32_t drogue_cmd_ms) {
    uint32_t ts = ctx->last_sample;
    int32_t rate = ctx->vertical_speed_cms < 0 ? -ctx->vertical_speed_cms : 0;
    /* A failed sensor's speed is no evidence [SNS-PRES-10, SNS-PRES-11]. */
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

/* The retry rung. Its evidence is the channel's own post-fire continuity; a
 * channel that opened fired its charge, and the canopy failed mechanically:
 * re-firing an empty channel spends altitude the main still needs
 * [PYR-REFIRE-02]. */
static void retry_drogue(flight_context_t *ctx, uint32_t now, bool canopy_working) {
    bool panic = ctx->vertical_speed_cms < 0 && descent_rate(ctx) >= EMRG_MAIN_PANIC_CMS && !ctx->fit_suspect;
    if (canopy_working || (!panic && now - ctx->pyro1_fire_time < EMRG_DROGUE_GRACE_MS))
        return;
    /* A refused retry is spent too: asked again it would only be refused, and
     * logged, every tick. */
    if (fire_channel(ctx, 1, now) || ctx->pyro1_refused)
        ctx->pyro1_refires++;
}

/* canopy_working is settling inside a band a canopy could explain; settling
 * in the fast band is a rocket at terminal velocity with nothing out. */
static void emergency_ladder(flight_context_t *ctx, uint32_t now, bool canopy_working) {
    bool drogue_commanded = ctx->pyro1_fired || ctx->pyro1_refused;
    if (!drogue_commanded || ctx->pyro2_fired || ctx->pyro2_refused || !ctx->pyro2_continuity_good)
        return;

    if (ctx->pyro1_verify_fail && ctx->pyro1_refires < EMRG_MAX_REFIRE) {
        retry_drogue(ctx, now, canopy_working);
        return;
    }

    if (!drogue_failing(ctx, now, ctx->pyro1_fire_time))
        return;
    if (fire_channel(ctx, 2, now)) {
        ctx->main_forced = true;
        log_event(ctx, EVT_MAIN_FORCED);
    }
}

/* [DD-015] Landing, checked in every descent state rather than only under the
 * main. A flight whose drogue never opened still lands, and the log has to be
 * closed on that flight too. */
#define LANDING_STILL_CMS 200 /* 2 m/s: the stillness test's own speed */

static bool landing_detected(flight_context_t *ctx, uint32_t now, int32_t prev_altitude) {
    int32_t altitude = ctx->last_altitude;
    bool altitude_stable = abs(altitude - prev_altitude) < 100;
    bool speed_low = abs(ctx->vertical_speed_cms) < 200;
    bool near_ground = altitude < 3000;

    if (altitude_stable && speed_low && near_ground) {
        uint32_t ts = ctx->last_sample;
        if (ctx->landing_stable_since == 0)
            ctx->landing_stable_since = ts + 1u;
        if (ts + 1u - ctx->landing_stable_since >= 1000)
            return true;
    } else {
        ctx->landing_stable_since = 0;
    }

    /* [FLT-LAND-07] Force landing if descent has run long and the rocket is
     * still: landing above the pad's elevation, where AGL never comes near
     * zero. Still, not merely slow -- a main descends at 3-6 m/s (N7) -- and
     * on a sensor that has not failed, since a stuck one reads as still. */
    uint32_t timeout_s = ctx->config.landing_timeout;
    bool timed_out = timeout_s > 0 && ctx->descent_start_time > 0 &&
                     (now - ctx->descent_start_time) >= timeout_s * 1000;
    bool still = abs(ctx->vertical_speed_cms) < LANDING_STILL_CMS && !ctx->fit_suspect;
    return held(timed_out && still, &ctx->still_since, ctx->last_sample, 1000u);
}

/* Every descent state reads the same sample and derives the same speed; only
 * the verdict differs. dt is taken between sample timestamps, never against
 * the loop clock. The sample becomes last_altitude before any trigger is
 * tested, so a trigger sees this sample and not the one before it; the
 * previous altitude is handed back for landing detection. */
static bool descent_sample(flight_context_t *ctx, uint32_t now, flight_state_t st, int32_t *prev_out) {
    watch_sensor(ctx, now); /* the row is logged at the sample's time [DAT-02] */
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return false;
    ctx->sensor_lost = false;
    ctx->filtered_pressure = pp_last_filtered_pa();
    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    ctx->vertical_speed_cms = sample_speed(ctx, &sample);
    take_fit(ctx, &sample);
    buf_add(ctx, sample.timestamp_ms - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, st);
    note_stuck(ctx, &sample);
    ctx->last_sample = sample.timestamp_ms;
    *prev_out = ctx->last_altitude;
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_height = sample.height_cm;
    return true;
}

/* Free fall: no canopy is working yet. Both channels may still fire here --
 * a low flight puts drogue and main out on the same event. */
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

/* Under drogue. The main may still be pending, and the drogue may still fail:
 * a rate above the drogue band, held, sends the machine back to free fall
 * where the ladder can escalate. */
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

/* Under the main.
 *
 * try_fire_pyros() runs here too, and must. The phase is a diagnosis, not a
 * licence to cancel the flight plan: a rocket already descending slowly -- a
 * big drogue, a light airframe -- still gets the deployment its config asked
 * for. Skipping it would be the firmware overriding the operator on the
 * strength of an inference. */
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

/* [FLT-LAND-06, FLT-RATE-04] */
static state_event_t detect_landed(flight_context_t *ctx, uint32_t now) {
    (void)now;
    altitude_sample_t sample;
    if (!pp_read(&sample))
        return SEVT_NONE;
    /* Landed: just log occasional samples for telemetry */
    ctx->filtered_pressure = pp_last_filtered_pa();
    /* Once a second, so the ring keeps the flight's events. Timed apart from
     * last_sample, which every sample moves. */
    if (ctx->landed_row_ms == 0 || sample.timestamp_ms - ctx->landed_row_ms >= 1000u) {
        buf_add(ctx, sample.timestamp_ms - ctx->launch_time, ctx->filtered_pressure, sample.altitude_cm, LANDED);
        ctx->landed_row_ms = sample.timestamp_ms;
    }
    ctx->last_altitude = sample.altitude_cm;
    ctx->last_height = sample.height_cm;
    ctx->last_sample = sample.timestamp_ms;
    return SEVT_NONE;
}

/* ── Transition actions ───────────────────────────────────────────── */

/* Repeat forever: a repeat count of 0 is infinite and nothing clears it. There
 * is no recovery from a failed power-up test, so the board must not fall silent
 * and look like it passed. */
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
    pp_start_cal(); /* pressure_processing layer handles calibration */
}

static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    /* Restarted here so the pad marker's dwell measures time sat on the pad,
     * not time since boot -- calibration's own duration should not count
     * toward it. */
    ctx->boot_timer = now;
    ctx->ground_pressure = pp_ground_pressure();
    ctx->filtered_pressure = ctx->ground_pressure;
}

/* [FLT-LAUNCH-03..05, GND-CAL-04/05] */
static void action_launch(flight_context_t *ctx, uint32_t now) {
    buzzer_stop();
    /* T+0 is the first sample above 50 cm. The detector cannot be sure of a
     * launch until 100 ft, a second or more later, and every time in the log
     * would be late by that climb. */
    ctx->launch_time = ctx->pad_rising ? ctx->pad_rise_ms : now;

    /* Freeze the reference; do not snap it to the pressure here. Snapping
     * would define the 100 ft already climbed as zero, and apogee and every
     * AGL threshold would be that much lower. Frozen from before T+0, since
     * the climb up to detection has already leaked into the mean. */
    (void)pp_ground_freeze_before(ctx->launch_time);
    ctx->ground_pressure = pp_ground_pressure();

    /* Also the base of the first ASCENT speed: from zero it would be one
     * enormous sample. */
    ctx->last_altitude = pp_pressure_to_altitude_cm(ctx->filtered_pressure, ctx->ground_pressure);
    ctx->last_height = pp_pressure_to_height_cm(ctx->filtered_pressure, ctx->ground_pressure);
    ctx->last_logged_ms = ctx->last_sample - ctx->launch_time;

    /* The ring's newest sample is a PAD_IDLE one, which the log does not
     * take, so the LAUNCH row is written here: the moment of detection and
     * the height reached by then. */
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(ctx->last_sample - ctx->launch_time, ctx->filtered_pressure, ctx->last_altitude, ASCENT, 0,
                   EVT_LAUNCH);
    buf_tag_event(ctx, EVT_LAUNCH);
    /* Flagged on the climb before the launch was sure [FLT-MACH-02]. */
    if (ctx->mach_lock) {
        extern void hal_telemetry_send(const char *sentence);
        hal_log_sample(ctx->last_sample - ctx->launch_time, ctx->filtered_pressure, ctx->last_altitude, ASCENT, 0,
                       EVT_MACH_LOCK);
        hal_telemetry_send("!MACH LOCK\r\n");
    }
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

/* [FLT-APO-02/03] */
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = ctx->apogee_fit_ms ? ctx->apogee_fit_ms : now; /* [PYR-MODE-05] */
    /* [FLT-MACH-07] A lock let go this close to apogee may have hidden the
     * top of the climb from the peak. */
    ctx->peak_lower_bound = ctx->mach_released && ctx->apogee_time - ctx->mach_release_ms < MACH_LOWER_BOUND_MS;
    ctx->descent_start_time = now; /* [DD-015] start landing timeout */
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

/* [DAT-06, DAT-07] The ring buffer's last 64 samples, for the simulator. */
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
    hal_pressure_init();
    int sensor = hal_pressure_sensor();
    ctx->sensor_type = sensor < 0 ? SENSOR_PENDING : (uint8_t)sensor;
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

/* [TEL-04, TEL-05] The ground-station contract has six states and no FAULT,
 * so a board that failed its power-up test and a board still booting send no
 * $PYRO sentence at all: state 0 would tell a tracker it is ready to fly. */
static bool state_sends_telemetry(flight_state_t st) {
    return st == PAD_IDLE || st == LANDED || state_is_airborne(st);
}

static void grounding_changed(flight_context_t *ctx, bool was, uint32_t now) {
    bool is = grounded_on_usb(ctx);
    if (is == was)
        return;
    if (is) {
        buzzer_play_usb_ok(); /* in place of whatever was being said */
        ctx->buzzer_started = false;
        return;
    }
    switch (ctx->current_state) {
    case PAD_IDLE:
        /* The marker is the pad's ground, and the bench was not the pad. The
         * pad check announces on its next pass. */
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

bool flight_in_progress(void) {
    return g_flight_ctx && state_is_airborne(g_flight_ctx->current_state);
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

/* A faulted board has no $PYRO sentence to send, but a console on the UART
 * should still be told why it is silent. */
#define FAULT_REPORT_MS 5000

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
    /* The buzzer is autonomous: hal_tasks_tick() drives it. */

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
