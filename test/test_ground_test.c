/*
 * The ground test through the flight software, booted as the hardware boots
 * (board_harness.c) [GND-TEST-05..11]: a board powered up with its ground
 * test pin asserted, the procedure run from the release, and what it must
 * never do.
 */
#include "unity.h"
#include "mocks.h"
#include "board_harness.h"
#include "../src/brownout.h"
#include "../src/flight_states.h"
#include "../src/ground_test_seq.h"
#include "../src/hal.h"
#include "../src/pad_claim.h"
#include "../src/ground_test.h"
#include "../src/serial_line.h"
#include <math.h>
#include <string.h>

extern reset_cause_t mock_reset_cause;

#define PAD_PA 101325.0f

static float isa_pa(float h_m) {
    return PAD_PA * powf(1.0f - 2.25577e-5f * h_m, 5.25588f);
}

static uint32_t t;
static uint32_t fire_ms[8];
static uint8_t fire_ch[8];
static int fires;
static bool saw[STATE_COUNT];

static void run_for(uint32_t ms) {
    uint32_t end = t + ms;
    while (t < end) {
        tick(++t);
        saw[ctx.current_state] = true;
        while (fires < mock_pyro.fire_count && fires < 8) {
            fire_ms[fires] = t;
            fire_ch[fires++] = mock_pyro.last_fire_channel;
        }
    }
}

/* Powered up with the pin as given, and the channels the configuration
 * enables: a mode, or none. */
static void power_up(bool pin, bool p1, bool p2) {
    boot_like_hardware(1);
    mock_ground_test_pin = pin;
    memset(saw, 0, sizeof(saw));
    fires = 0;
    t = 0;
    tick(t);
    ctx.config.pyro1_mode = p1 ? PYRO_MODE_DELAY : PYRO_MODE_NONE;
    ctx.config.pyro2_mode = p2 ? PYRO_MODE_AGL : PYRO_MODE_NONE;
}

/* In ground test mode, held long enough to arm; then released. Returns the
 * release's time. */
static uint32_t enter_and_release(bool p1, bool p2) {
    power_up(true, p1, p2);
    run_for(6000u);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    mock_ground_test_pin = false;
    return t;
}

void setUp(void) {}
void tearDown(void) {}

/* [GND-TEST-05, GND-TEST-06] Asserted at power-up: ground test mode, after
 * the sensor and continuity checks, announced, and never the pad. */
void test_GND_TEST_05_powered_up_asserted_enters_ground_test(void) {
    power_up(true, true, true);
    run_for(20000u);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    TEST_ASSERT_TRUE(saw[BOOT_CONTINUITY]);
    TEST_ASSERT_FALSE(saw[BOOT_CALIBRATE]);
    TEST_ASSERT_FALSE(saw[PAD_IDLE]);
    TEST_ASSERT_TRUE(harness_gt_sound_n >= 1);
    TEST_ASSERT_EQUAL(GT_SOUND_ALERT, harness_gt_sounds[0]);
    TEST_ASSERT_EQUAL(0, harness_spec_plays); /* no pad announcement */
    TEST_ASSERT_EQUAL(0, fires);
}

/* Guard: not asserted, the board boots to the pad as it always has. */
void test_GND_TEST_05_not_asserted_boots_to_the_pad(void) {
    power_up(false, true, true);
    run_for(20000u);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_EQUAL(0, harness_gt_sound_n);
}

/* [GND-TEST-05] Asserted only for the first moments of power-up, not held
 * through the settle: the pad, not ground test mode. */
void test_GND_TEST_05_asserted_briefly_boots_to_the_pad(void) {
    power_up(true, true, true);
    run_for(1000u);
    mock_ground_test_pin = false;
    run_for(20000u);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_EQUAL(0, fires);
}

/* [GND-TEST-07] The release: a countdown, channel 1 at zero, the tone and a
 * second countdown, channel 2 at its zero, the all-clear. */
void test_GND_TEST_07_the_procedure(void) {
    uint32_t released = enter_and_release(true, true);
    run_for(30000u);
    TEST_ASSERT_EQUAL(2, fires);
    TEST_ASSERT_EQUAL(1, fire_ch[0]);
    TEST_ASSERT_EQUAL(2, fire_ch[1]);
    TEST_ASSERT_UINT32_WITHIN(30u, released + GT_DEBOUNCE_MS + GT_COUNTDOWN_MS, fire_ms[0]);
    TEST_ASSERT_UINT32_WITHIN(30u, fire_ms[0] + GT_TONE_MS + GT_COUNTDOWN_MS, fire_ms[1]);
    const gt_sound_t want[] = {GT_SOUND_ALERT, GT_SOUND_COUNTDOWN, GT_SOUND_TONE, GT_SOUND_COUNTDOWN,
                               GT_SOUND_ALL_CLEAR};
    TEST_ASSERT_EQUAL(5, harness_gt_sound_n);
    TEST_ASSERT_EQUAL_INT_ARRAY(want, harness_gt_sounds, 5);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
}

/* [GND-TEST-08] A channel with no mode is not enabled: skipped, with the
 * tone and countdown before channel 2. */
void test_GND_TEST_08_only_the_enabled_channels_fire(void) {
    enter_and_release(false, true);
    run_for(30000u);
    TEST_ASSERT_EQUAL(1, fires);
    TEST_ASSERT_EQUAL(2, fire_ch[0]);

    enter_and_release(true, false);
    run_for(30000u);
    TEST_ASSERT_EQUAL(1, fires);
    TEST_ASSERT_EQUAL(1, fire_ch[0]);

    enter_and_release(false, false);
    run_for(30000u);
    TEST_ASSERT_EQUAL(0, fires);
    TEST_ASSERT_EQUAL(GT_SOUND_ALL_CLEAR, harness_gt_sounds[harness_gt_sound_n - 1]);
}

/* [GND-TEST-08] A channel released to Lua is not enabled either: its pads
 * are a script's. */
void test_GND_TEST_08_a_released_channel_is_not_enabled(void) {
    boot_like_hardware(1);
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(PAD(9), PAD_LUA)); /* channel 1's element, in the host numbering */
    mock_ground_test_pin = true;
    memset(saw, 0, sizeof(saw));
    fires = 0;
    t = 0;
    tick(t);
    ctx.config.pyro1_mode = PYRO_MODE_DELAY;
    ctx.config.pyro2_mode = PYRO_MODE_AGL;
    run_for(6000u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL(1, fires);
    TEST_ASSERT_EQUAL(2, fire_ch[0]);
}

/* [GND-TEST-10] Put back during the countdown: nothing fires. */
void test_GND_TEST_10_reasserting_aborts(void) {
    enter_and_release(true, true);
    run_for(2000u);
    mock_ground_test_pin = true;
    run_for(30000u);
    TEST_ASSERT_EQUAL(0, fires);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
}

/* [GND-TEST-11] Ground test mode never flies: a pressure that would be a
 * launch changes nothing, and no flight log is opened. */
void test_GND_TEST_11_never_flies(void) {
    power_up(true, true, true);
    run_for(6000u);
    for (int i = 0; i < 20000; i++) {
        mock_pressure.pressure_pa = isa_pa((float)i * 0.05f);
        run_for(1u);
    }
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    TEST_ASSERT_FALSE(saw[ASCENT]);
    TEST_ASSERT_FALSE(hal_log_active());
    TEST_ASSERT_EQUAL(0, fires);
}

/* [GND-TEST-05] A board coming back from a power event in flight carries on
 * flying, whatever the pin says: the recovery outranks ground test mode. Here
 * it comes back descending at 10 m/s from 300 m. (Still and high reads as
 * weather or a moved board, which takes the cold path by design.) */
void test_GND_TEST_05_a_recovery_outranks_the_pin(void) {
    boot_like_hardware(1);
    write_marker((int32_t)PAD_PA);
    mock_reset_cause = RESET_POWER_EVENT;
    mock_ground_test_pin = true;
    memset(saw, 0, sizeof(saw));
    fires = 0;
    t = 0;
    float h = 300.0f;
    mock_pressure.pressure_pa = isa_pa(h);
    tick(t);
    for (int i = 0; i < 6000; i++) {
        h -= 0.010f;
        mock_pressure.pressure_pa = isa_pa(h);
        run_for(1u);
    }
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_TRUE(saw[ASCENT] || saw[FALLING]);
}

/* [GND-TEST-06] USB attached in ground test mode does not take the buzzer:
 * a countdown must not be cut off by the attach chirp. */
void test_GND_TEST_06_usb_does_not_take_the_buzzer(void) {
    enter_and_release(true, true);
    run_for(1000u);
    flight_set_usb_attached(&ctx, true, t);
    run_for(20000u);
    TEST_ASSERT_EQUAL(0, harness_usb_ok_plays);
    TEST_ASSERT_EQUAL(2, fires);
}

/* ── The serial commands' line [GND-TEST-01..04] ─────────────────── */

static int gt_replies(void) {
    int n = 0;
    for (const char *p = mock_uart_buf; (p = strstr(p, "$GT,")) != NULL; p++)
        n++;
    return n;
}

/* MK1A's RX hears its own TX. Each echoed reply, answered as a command, was
 * answered with unknown_cmd, whose echo was answered again: a loop with no
 * end on the wire. Nothing this board sends is a command. */
void test_GND_TEST_01_an_echo_of_our_own_line_is_not_a_command(void) {
    power_up(false, true, true);
    run_for(20000u);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    ground_test_handle_command(&ctx.gt, "$GT,ERR,unknown_cmd*2B", &ctx, t);
    ground_test_handle_command(&ctx.gt, "$PYRO,1,0,0,0,0,101325,0,0*00", &ctx, t);
    ground_test_handle_command(&ctx.gt, "!PRES init OK: MS5607", &ctx, t);
    ground_test_handle_command(&ctx.gt, "", &ctx, t);
    TEST_ASSERT_EQUAL_MESSAGE(0, gt_replies(), mock_uart_buf);

    ground_test_handle_command(&ctx.gt, "NONSENSE", &ctx, t);
    TEST_ASSERT_EQUAL(1, gt_replies());
}

/* Bytes in, lines out, as hal_serial_readline() assembles them. */
static int feed(serial_line_t *r, const char *bytes, char *out, int max_len, char lines[][SERIAL_LINE_MAX]) {
    int n = 0;
    for (const char *c = bytes; *c; c++) {
        if (serial_line_feed(r, *c, out, max_len))
            strcpy(lines[n++], out);
    }
    return n;
}

/* 63 bytes with no line end filled the old buffer, and the reader stopped
 * taking bytes for good: no command reached the board again. A line too long
 * is dropped whole, and the next one reads from its start. */
void test_GND_TEST_01_an_over_long_line_is_dropped_and_the_next_read(void) {
    serial_line_t r = {0};
    char out[48], lines[4][SERIAL_LINE_MAX];
    char noise[200];
    memset(noise, 'x', sizeof(noise) - 1);
    noise[sizeof(noise) - 1] = '\0';
    TEST_ASSERT_EQUAL(0, feed(&r, noise, out, (int)sizeof(out), lines));
    TEST_ASSERT_EQUAL(0, feed(&r, "ARM 1", out, (int)sizeof(out), lines));
    TEST_ASSERT_EQUAL_MESSAGE(0, feed(&r, "\r\n", out, (int)sizeof(out), lines), "the tail of the long line came out");
    TEST_ASSERT_EQUAL(1, feed(&r, "STATUS\r\n", out, (int)sizeof(out), lines));
    TEST_ASSERT_EQUAL_STRING("STATUS", lines[0]);
}

void test_GND_TEST_01_a_line_is_cut_to_the_callers_buffer(void) {
    serial_line_t r = {0};
    char out[4], lines[2][SERIAL_LINE_MAX];
    TEST_ASSERT_EQUAL(1, feed(&r, "BEEP\n", out, (int)sizeof(out), lines));
    TEST_ASSERT_EQUAL_STRING("BEE", lines[0]);
    TEST_ASSERT_EQUAL_MESSAGE(0, feed(&r, "BEEP\n", out, 0, lines), "a zero-length buffer takes nothing");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_GND_TEST_05_powered_up_asserted_enters_ground_test);
    RUN_TEST(test_GND_TEST_05_not_asserted_boots_to_the_pad);
    RUN_TEST(test_GND_TEST_05_asserted_briefly_boots_to_the_pad);
    RUN_TEST(test_GND_TEST_07_the_procedure);
    RUN_TEST(test_GND_TEST_08_only_the_enabled_channels_fire);
    RUN_TEST(test_GND_TEST_08_a_released_channel_is_not_enabled);
    RUN_TEST(test_GND_TEST_10_reasserting_aborts);
    RUN_TEST(test_GND_TEST_11_never_flies);
    RUN_TEST(test_GND_TEST_05_a_recovery_outranks_the_pin);
    RUN_TEST(test_GND_TEST_06_usb_does_not_take_the_buzzer);
    RUN_TEST(test_GND_TEST_01_an_echo_of_our_own_line_is_not_a_command);
    RUN_TEST(test_GND_TEST_01_an_over_long_line_is_dropped_and_the_next_read);
    RUN_TEST(test_GND_TEST_01_a_line_is_cut_to_the_callers_buffer);
    return UNITY_END();
}
