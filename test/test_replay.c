/*
 * Reading a flight log for replay (sim/replay.h) [DAT-08]: the logs a
 * replay meets off the board, not only the ones the firmware writes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../sim/replay.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define ROWS 200
#define ROW_MS 20 /* LOOP_PERIOD_MS */
#define PAD_PA 101325

/* A log of a rocket sitting still at ASCENT, which a replay must read to the
 * end, with or without the state column. */
static int write_log(char *out, size_t size, const char *eol, bool with_state) {
    int n = snprintf(out, size,
                     "# Pyro Flight Data%s# Pyro1: delay 0%s# Pyro2: agl 200%s# Units: ft%s"
                     "# Ground Pa: %d%s# Log rate: every sample%s",
                     eol, eol, eol, eol, PAD_PA, eol, eol);
    n += snprintf(out + n, size - (size_t)n, "%s%s",
                  with_state ? "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event"
                             : "time_ms,pressure_pa,altitude_cm,thrust,raw_pa,temp_c,event",
                  eol);
    for (int i = 0; i < ROWS; i++) {
        if (with_state)
            n += snprintf(out + n, size - (size_t)n, "%d,%d,0,4,0,%d,20.0,%s", i * ROW_MS, PAD_PA, PAD_PA, eol);
        else
            n += snprintf(out + n, size - (size_t)n, "%d,%d,0,0,%d,20.0,%s", i * ROW_MS, PAD_PA, PAD_PA, eol);
    }
    return n;
}

static char lf_log[32768], crlf_log[32768];

void test_DAT_08_a_crlf_log_replays_as_its_lf_twin(void) {
    write_log(lf_log, sizeof(lf_log), "\n", true);
    write_log(crlf_log, sizeof(crlf_log), "\r\n", true);
    replay_events_t lf, crlf;
    TEST_ASSERT_TRUE(replay_run(lf_log, &lf));
    TEST_ASSERT_TRUE_MESSAGE(replay_run(crlf_log, &crlf), "a log saved on Windows replays");
    TEST_ASSERT_EQUAL_INT(lf.rows, crlf.rows);
    TEST_ASSERT_EQUAL_UINT32(lf.diverged_ms, crlf.diverged_ms);
}

void test_DAT_08_a_log_without_a_state_column_still_replays(void) {
    write_log(lf_log, sizeof(lf_log), "\n", false);
    replay_events_t decided;
    TEST_ASSERT_TRUE(replay_run(lf_log, &decided));
    TEST_ASSERT_EQUAL_INT(ROWS, decided.rows);
    TEST_ASSERT_EQUAL_UINT32(0, decided.diverged_ms);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_DAT_08_a_crlf_log_replays_as_its_lf_twin);
    RUN_TEST(test_DAT_08_a_log_without_a_state_column_still_replays);
    return UNITY_END();
}
