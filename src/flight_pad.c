/*
 * On the pad: the pyro health check and its announcement, the ground
 * reference, and the launch [PYR-CONT-01..03, BUZ-01, BUZ-02, GND-CAL-06,
 * FLT-LAUNCH-02..07, USB-01..04].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "atmosphere.h"
#include "beep_store.h"
#include "buzzer.h"
#include "pressure_processing.h"

#define HEALTH_CHECK_MS 1000u
#define GROUND_RESEED_MS 5000u
#define STILL_CMS 100

static bool health_check_due(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_health_check < HEALTH_CHECK_MS)
        return false;
    ctx->last_health_check = now;
    return true;
}

/* Said again whenever the verdict changes, and otherwise left to repeat on
 * the personality's cadence. Not said on USB [USB-02]. */
static void announce(flight_context_t *ctx) {
    if (flight_grounded_on_usb(ctx))
        return;
    beep_reason_t verdict = pad_check_announcement(ctx->diag);
    if (ctx->announced && verdict == (beep_reason_t)ctx->announcement)
        return;
    ctx->announced = true;
    ctx->announcement = (uint8_t)verdict;
    beep_say(verdict);
}

static void check_pad(flight_context_t *ctx, uint32_t now) {
    if (!health_check_due(ctx, now))
        return;
    flight_read_pyro_health(ctx);
    announce(ctx);
}

/* [GND-CAL-06] */
static void follow_a_moved_board(flight_context_t *ctx, const pp_sample_t *s, uint32_t now) {
    bool still = s->speed_cms < STILL_CMS && s->speed_cms > -STILL_CMS;
    if (pp_ground_rejecting_ms(s->timestamp_ms) < GROUND_RESEED_MS || !still)
        return;
    pp_ground_reseed();
    hal_telemetry_send("!GND reseed\r\n");
    ctx->boot_timer = now;
    ctx->record_written = false;
}

state_event_t flight_detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    check_pad(ctx, now);

    pp_sample_t s;
    if (!pp_read(&s)) {
        flight_note_no_sample(ctx, now);
        return SEVT_NONE;
    }
    follow_a_moved_board(ctx, &s, now);
    ctx->ground_pressure = pp_ground_pressure();
    flight_take_sample(ctx, &s, PAD_IDLE);
    flight_note_sensor(ctx, &s, now);

    bool launch = launch_detected(&s, (float)ctx->ground_pressure);
    return launch && !flight_grounded_on_usb(ctx) ? SEVT_LAUNCH : SEVT_NONE;
}

/* [FLT-LAUNCH-03..05, GND-CAL-04, GND-CAL-05] */
void flight_action_launch(flight_context_t *ctx, uint32_t now) {
    (void)now;
    buzzer_stop();
    ctx->launch_time = pp_time_of_rise_ms();
    (void)pp_ground_freeze_before(ctx->launch_time);
    ctx->ground_pressure = pp_ground_pressure();
    ctx->altitude_cm = (int32_t)(atmos_height_above_m((float)ctx->pressure_pa, (float)ctx->ground_pressure) * 100.0f);
    ctx->under_thrust = true;

    uint32_t flight_ms = ctx->last_sample - ctx->launch_time;
    hal_log_start(&ctx->config, ctx->ground_pressure);
    hal_log_sample(flight_ms, ctx->pressure_pa, ctx->altitude_cm, ASCENT, 0, EVT_LAUNCH);
}

/* ── USB [USB-01..08] ─────────────────────────────────────────────── */

static void grounding_changed(flight_context_t *ctx, bool was_grounded, uint32_t now) {
    bool grounded = flight_grounded_on_usb(ctx);
    if (grounded == was_grounded || ctx->current_state == GROUND_TEST)
        return;
    if (grounded) {
        buzzer_play_usb_ok();
        ctx->announced = false;
        return;
    }
    switch (ctx->current_state) {
    case PAD_IDLE:
        ctx->boot_timer = now; /* the bench was not the pad: the record's dwell starts again */
        ctx->record_written = false;
        break;
    case LANDED:
        buzzer_play_altitude(flight_to_units(ctx->max_altitude_cm, ctx->config.units));
        break;
    case FAULT:
        flight_say_fault();
        break;
    default:
        break;
    }
}

void flight_set_usb_attached(flight_context_t *ctx, bool attached, uint32_t now) {
    if (attached == ctx->usb_attached || flight_state_is_airborne(ctx->current_state))
        return;
    bool was_grounded = flight_grounded_on_usb(ctx);
    ctx->usb_attached = attached;
    grounding_changed(ctx, was_grounded, now);
}

void flight_set_test_mode(flight_context_t *ctx, bool on, uint32_t now) {
    if (on == ctx->test_mode || flight_state_is_airborne(ctx->current_state))
        return;
    bool was_grounded = flight_grounded_on_usb(ctx);
    ctx->test_mode = on;
    grounding_changed(ctx, was_grounded, now);
}
