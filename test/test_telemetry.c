/*
 * The downlink of a whole flight, read as a ground station reads it
 * [TEL-01..11, DD-088]. The sentence formats are in
 * docs/ground-station-interface-spec.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_harness.h"
#include "flight_run.h"
#include "../src/telemetry.h"

void setUp(void) {}
void tearDown(void) {}

/* ── What a ground station makes of the port's output ─────────────── */

typedef struct {
    long seq, state, thrust, alt_cm, speed_cms, max_alt_cm, press_pa, time_ms, flags;
    int fields;
} state_msg_t;

typedef struct {
    char kind[12]; /* PYRO_APO, PYRO_FIRE, PYRO_LAND */
    long channel, alt_cm, time_ms;
    long carried_at_ms; /* the flight time of the state sentence that followed it */
} event_msg_t;

#define MSGS_MAX 400
static state_msg_t states[MSGS_MAX];
static event_msg_t events[64];
static int n_states, n_events, n_bad_checksums, n_other_sentences, n_events_not_followed;

static bool checksum_ok(const char *line, size_t len) {
    const char *star = memchr(line, '*', len);
    if (!star || line + len - star < 3)
        return false;
    uint8_t sum = 0;
    for (const char *p = line + 1; p < star; p++)
        sum ^= (uint8_t)*p;
    return (uint8_t)strtoul(star + 1, NULL, 16) == sum;
}

static void read_downlink(void) {
    n_states = n_events = n_bad_checksums = n_other_sentences = n_events_not_followed = 0;
    int pending = 0; /* events since the last state sentence */
    for (const char *line = mock_uart_buf; *line;) {
        const char *end = strstr(line, "\r\n");
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (line[0] == '$') {
            if (!checksum_ok(line, len))
                n_bad_checksums++;
            char body[220] = {0};
            memcpy(body, line + 1, len - 1 < sizeof(body) - 1 ? len - 1 : sizeof(body) - 1);
            *strchr(body, '*') = '\0';
            if (strncmp(body, "PYRO,", 5) == 0 && n_states < MSGS_MAX) {
                state_msg_t *m = &states[n_states++];
                m->fields = sscanf(body, "PYRO,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%lx", &m->seq, &m->state, &m->thrust,
                                   &m->alt_cm, &m->speed_cms, &m->max_alt_cm, &m->press_pa, &m->time_ms, &m->flags);
                for (; pending > 0; pending--)
                    events[n_events - pending].carried_at_ms = m->time_ms;
            } else if (strncmp(body, "PYRO_", 5) == 0 && n_events < 64) {
                event_msg_t *e = &events[n_events++];
                memset(e, 0, sizeof(*e));
                memcpy(e->kind, body, strcspn(body, ","));
                if (strcmp(e->kind, "PYRO_FIRE") == 0)
                    sscanf(body, "PYRO_FIRE,%ld,%ld,%ld", &e->channel, &e->alt_cm, &e->time_ms);
                else
                    sscanf(strchr(body, ','), ",%ld,%ld", &e->alt_cm, &e->time_ms);
                pending++;
            } else {
                n_other_sentences++;
            }
        }
        if (!end)
            break;
        line = end + 2;
    }
    n_events_not_followed = pending;
}

static int count_events(const char *kind) {
    int n = 0;
    for (int i = 0; i < n_events; i++)
        n += strcmp(events[i].kind, kind) == 0;
    return n;
}

/* ── One flight, to the ground ────────────────────────────────────── */

static flown_t flown;

static void stay_landed(uint32_t ms) {
    for (uint32_t t = mock_time_ms + 1, end = t + ms; t < end; t++)
        tick(t);
}

static void fly_to_the_ground(const char *config, uint32_t seed) {
    flight_conditions_t c = {.config = config, .rate_ms = {15.0f, 5.0f}, .to_landed = true};
    flown = fly(&ROCKETS[SUBSONIC].r, &COLD, &c, seed, 400.0f);
    TEST_ASSERT_EQUAL(LANDED, flown.final_state);
    stay_landed(1500);
    read_downlink();
}

void test_TEL_01_every_sentence_is_pyro_nmea_with_a_good_checksum(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 1);
    TEST_ASSERT_TRUE(n_states > 30);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n_bad_checksums, "[TEL-02]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n_other_sentences, "nothing but $PYRO and its events");
    for (int i = 0; i < n_states; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(9, states[i].fields, "sequence, state, altitude, speed, peak, pressure, time, flags [TEL-06]");
}

void test_TEL_09_the_sequence_counts_every_sentence(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 2);
    for (int i = 1; i < n_states; i++)
        TEST_ASSERT_EQUAL_INT32(states[i - 1].seq + 1, states[i].seq);
}

/* [TEL-03] One message a second: on the pad, through the flight and after
 * the landing. In flight the flight time says so itself. */
void test_TEL_03_one_message_a_second_in_every_state(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 3);
    bool seen[6] = {false};
    for (int i = 0; i < n_states; i++) {
        TEST_ASSERT_TRUE(states[i].state >= 0 && states[i].state <= 5);
        seen[states[i].state] = true;
        bool flying = states[i].state >= 1 && states[i].state <= 4;
        if (i > 0 && flying && states[i - 1].state >= 1)
            TEST_ASSERT_INT32_WITHIN(60, 1000, states[i].time_ms - states[i - 1].time_ms);
    }
    TEST_ASSERT_TRUE_MESSAGE(seen[0] && seen[1] && seen[5], "the pad, the ascent and the landing [TEL-07]");
    TEST_ASSERT_TRUE_MESSAGE(seen[3] || seen[4], "a descent band [TEL-07]");

    boot_like_hardware(4);
    uint32_t t = 0;
    run_to_pad(&t);
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    for (uint32_t end = t + 20000u; t < end; t++)
        tick(t);
    read_downlink();
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 20, n_states, "on the pad");
}

void test_TEL_03_one_message_a_second_after_landing(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 5);
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    stay_landed(20000);
    read_downlink();
    TEST_ASSERT_INT_WITHIN(1, 20, n_states);
    for (int i = 0; i < n_states; i++) {
        TEST_ASSERT_EQUAL_INT32(5, states[i].state);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(states[0].time_ms, states[i].time_ms, "the flight time stops at the landing");
    }
}

void test_TEL_08_the_flags_follow_the_flight(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 6);
    const state_msg_t *pad = &states[0], *last = &states[n_states - 1];
    TEST_ASSERT_EQUAL_HEX8(TELEM_FLAG_P1_READY | TELEM_FLAG_P2_READY, pad->flags);
    const long flown_flags = TELEM_FLAG_P1_FIRED | TELEM_FLAG_P2_FIRED | TELEM_FLAG_ARMED | TELEM_FLAG_APOGEE;
    TEST_ASSERT_EQUAL_HEX8(flown_flags, last->flags & flown_flags);
    for (int i = 1; i < n_states; i++) {
        long set = states[i].flags & flown_flags, before = states[i - 1].flags & flown_flags;
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(before, set & before, "what has happened stays happened");
        if (set & (TELEM_FLAG_P1_FIRED | TELEM_FLAG_P2_FIRED))
            TEST_ASSERT_TRUE_MESSAGE(set & TELEM_FLAG_APOGEE, "no fire without apogee");
    }
}

void test_TEL_10_thrust_is_reported_only_in_the_burn(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 7);
    int thrusting = 0;
    float burn_s = ROCKETS[SUBSONIC].r.burn_s;
    for (int i = 0; i < n_states; i++) {
        if (!states[i].thrust)
            continue;
        thrusting++;
        TEST_ASSERT_EQUAL_INT32(1, states[i].state);
        TEST_ASSERT_TRUE((float)states[i].time_ms / 1000.0f < burn_s + 1.0f);
    }
    TEST_ASSERT_TRUE(thrusting >= 1);
}

void test_TEL_06_the_sentence_carries_the_flight(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 8);
    const state_msg_t *last = &states[n_states - 1];
    TEST_ASSERT_FLOAT_WITHIN(0.03f * flown.apogee_h, flown.apogee_h, (float)last->max_alt_cm / 100.0f);
    TEST_ASSERT_INT32_WITHIN(300, 0, last->alt_cm);
    TEST_ASSERT_INT32_WITHIN(60, (int32_t)mp_pad_pa(&COLD), last->press_pa);
    long fastest_up = 0, fastest_down = 0;
    for (int i = 0; i < n_states; i++) {
        fastest_up = states[i].speed_cms > fastest_up ? states[i].speed_cms : fastest_up;
        fastest_down = states[i].speed_cms < fastest_down ? states[i].speed_cms : fastest_down;
        TEST_ASSERT_TRUE(states[i].max_alt_cm >= states[i].alt_cm - 200);
    }
    TEST_ASSERT_TRUE_MESSAGE(fastest_up > 5000 && fastest_down < -400, "the speed is signed, up positive");
}

/* ── Events ride the next message [TEL-11] ────────────────────────── */

void test_TEL_11_each_event_is_carried_once_by_the_next_message(void) {
    fly_to_the_ground("pyro2_mode=agl\npyro2_value=150\n", 9);
    TEST_ASSERT_EQUAL_INT(1, count_events("PYRO_APO"));
    TEST_ASSERT_EQUAL_INT(1, count_events("PYRO_LAND"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(flown.pulses, count_events("PYRO_FIRE"), "every pulse");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n_events_not_followed, "each is followed by its state sentence");
    for (int i = 0; i < n_events; i++) {
        if (strcmp(events[i].kind, "PYRO_LAND") == 0)
            continue; /* the flight time has stopped by then */
        long late = events[i].carried_at_ms - events[i].time_ms;
        TEST_ASSERT_TRUE_MESSAGE(late >= 0 && late <= 1000, "at most a second late");
    }
    /* Flight time counts from the start of the rise [FLT-LAUNCH-03]. */
    TEST_ASSERT_FLOAT_WITHIN(0.15f, flown.declared_t, (float)events[0].time_ms / 1000.0f);
}

/* A failed recovery: every re-fire of both channels is reported, none lost. */
void test_TEL_11_no_event_is_lost_in_an_emergency(void) {
    flight_conditions_t c = {.config = "pyro2_mode=agl\npyro2_value=150\nemergency_fire_speed=35\n"
                                       "pyro1_refire_speed=25\npyro2_refire_speed=25\n",
                             .lights_on_pulse = {-1, -1},
                             .to_landed = true};
    flown = fly(&ROCKETS[HOP].r, &COLD, &c, 10, 400.0f);
    stay_landed(1500);
    read_downlink();
    TEST_ASSERT_TRUE(flown.pulses >= 2);
    TEST_ASSERT_EQUAL_INT(flown.pulses, count_events("PYRO_FIRE"));
    TEST_ASSERT_EQUAL_UINT32(0, ctx.telemetry.dropped);
}

void test_TEL_11_the_queue_holds_more_than_a_second_of_events(void) {
    telemetry_queue_t q = {0};
    for (uint8_t i = 0; i < TELEM_QUEUE_SIZE; i++)
        telemetry_queue(&q, (telemetry_event_t){TELEM_EVENT_FIRE, (uint8_t)(1 + i % 2), 1000 * i, 500u * i});
    TEST_ASSERT_EQUAL_UINT32(0, q.dropped);
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    telemetry_send(&q, &(telemetry_snapshot_t){.seq = 7});
    read_downlink();
    TEST_ASSERT_EQUAL_INT(TELEM_QUEUE_SIZE, n_events);
    for (int i = 0; i < n_events; i++)
        TEST_ASSERT_EQUAL_INT32_MESSAGE(500 * i, events[i].time_ms, "oldest first");
    TEST_ASSERT_EQUAL_INT(1, n_states);
    TEST_ASSERT_EQUAL_INT(0, n_bad_checksums);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_TEL_01_every_sentence_is_pyro_nmea_with_a_good_checksum);
    RUN_TEST(test_TEL_09_the_sequence_counts_every_sentence);
    RUN_TEST(test_TEL_03_one_message_a_second_in_every_state);
    RUN_TEST(test_TEL_03_one_message_a_second_after_landing);
    RUN_TEST(test_TEL_08_the_flags_follow_the_flight);
    RUN_TEST(test_TEL_10_thrust_is_reported_only_in_the_burn);
    RUN_TEST(test_TEL_06_the_sentence_carries_the_flight);
    RUN_TEST(test_TEL_11_each_event_is_carried_once_by_the_next_message);
    RUN_TEST(test_TEL_11_no_event_is_lost_in_an_emergency);
    RUN_TEST(test_TEL_11_the_queue_holds_more_than_a_second_of_events);
    return UNITY_END();
}
