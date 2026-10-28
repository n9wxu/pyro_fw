/*
 * Real flights, flown through the flight software [TST-02, FLT-APO-01,
 * FLT-LAUNCH-07]: the barometer of fifteen TeleMetrum flights from AltOS
 * (test_data/altos/, under the GNU GPL version 2 or later). Each log's
 * readings before the boost play over and over on the pad for the board to
 * calibrate, then the log plays, and its last second plays on after it.
 * AltOS's own apogee, from its accelerometer and barometer together, is the
 * first drogue or main row. The log of serial 334's flight 3 is left out: its
 * barometer reads lowest 60 ms after the boost.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <stdio.h>
#include <string.h>
#include "board_harness.h"
#include "../src/atmosphere.h"

void setUp(void) {}
void tearDown(void) {}

#define POINTS_MAX 4000
#define FIELDS 14
#define START_MS 25000u /* boot, calibration and time on the pad */
#define AFTER_THE_LOG_MS 20000u
#define EARLY_MAX_S 0.1f
#define LATE_MAX_S 4.0f

typedef struct {
    float t_s, pa;
} point_t;

static point_t logged[POINTS_MAX];
static int points;
static float altos_apogee_s, end_s;

/* The columns of an AltOS CSV row this needs: time, state name, pressure. */
static bool parse_row(char *line, float *t, char *state, float *pa) {
    char *field[FIELDS];
    int n = 0;
    for (char *p = line; n < FIELDS; p++) {
        field[n++] = p;
        p = strchr(p, ',');
        if (!p)
            break;
        *p = '\0';
    }
    if (n < FIELDS || sscanf(field[4], "%f", t) != 1 || sscanf(field[11], "%f", pa) != 1)
        return false;
    sscanf(field[9], "%15s", state);
    return true;
}

static void load(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "test_data/altos/%s", name);
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    char line[1024];
    points = 0;
    altos_apogee_s = -1.0f;
    while (fgets(line, sizeof(line), f) && points < POINTS_MAX) {
        float t, pa;
        char state[16] = "";
        if (line[0] == '#' || !parse_row(line, &t, state, &pa))
            continue;
        if (altos_apogee_s < 0.0f && (strcmp(state, "drogue") == 0 || strcmp(state, "main") == 0))
            altos_apogee_s = t;
        logged[points++] = (point_t){t, pa};
    }
    fclose(f);
    TEST_ASSERT_TRUE(points > 100);
    end_s = logged[points - 1].t_s;
}

#define TAIL_S 1.0f
/* Past apogee AltOS logs less often. Across a gap the reading is the next
 * row's: a shock seen in the last row before it is over in tens of
 * milliseconds, not stretched across the gap. */
#define LOG_STEP_MAX_S 0.015f

/* A span of the log played over and over: a sensor that repeats one reading
 * is a stuck one [SNS-PRES-10]. */
static float looped(float from_s, float to_s, float t_s) {
    float span = to_s - from_s;
    float k = (float)(int)((t_s - from_s) / span);
    return from_s + (t_s - from_s) - k * span;
}

static float logged_pa(float t_s) {
    if (t_s < logged[0].t_s)
        t_s = looped(logged[0].t_s, 0.0f, t_s);
    if (t_s >= end_s)
        t_s = looped(end_s - TAIL_S, end_s, t_s);
    for (int i = 1; i < points; i++)
        if (t_s < logged[i].t_s) {
            float gap_s = logged[i].t_s - logged[i - 1].t_s;
            if (gap_s > LOG_STEP_MAX_S)
                return logged[i].pa;
            float along = (t_s - logged[i - 1].t_s) / gap_s;
            return logged[i - 1].pa + along * (logged[i].pa - logged[i - 1].pa);
        }
    return logged[points - 1].pa;
}

typedef struct {
    float launch_s, apogee_s;
} replayed_t;

static replayed_t replay(const char *name) {
    load(name);
    boot_like_hardware(1);
    mock_noise_rms_pa = 0.0f; /* the log carries the sensor's own */
    mock_pressure.pressure_pa = logged[0].pa;
    harness_config("[pyro]\nunits=m\n");
    replayed_t r = {-1.0f, -1.0f};
    uint32_t start = START_MS;
    uint32_t end = start + (uint32_t)((end_s - logged[0].t_s) * 1000.0f) + AFTER_THE_LOG_MS;
    for (uint32_t t = 0; t < end; t++) {
        float log_s = logged[0].t_s + ((float)t - (float)start) / 1000.0f;
        if (t == start)
            TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "on the pad when the log begins");
        mock_pressure.pressure_pa = logged_pa(log_s);
        tick(t);
        if (ctx.current_state >= ASCENT && ctx.current_state <= LANDED && r.launch_s < 0.0f)
            r.launch_s = log_s;
        if (ctx.apogee_declared && r.apogee_s < 0.0f)
            r.apogee_s = log_s;
    }
    printf("  %-38s launch %5.2f s, apogee %+5.2f s from AltOS's\n", name, (double)r.launch_s,
           (double)(r.apogee_s - altos_apogee_s));
    return r;
}

static const char *const FLIGHTS[] = {
    "2012-04-07-serial-882-flight-001.csv", "2012-04-07-serial-892-flight-002.csv",
    "2012-05-28-serial-319-flight-005.csv", "2012-06-29-serial-058-flight-005.csv",
    "2012-06-29-serial-336-flight-002.csv", "2012-06-30-serial-058-flight-006.csv",
    "2012-06-30-serial-336-flight-003.csv", "2012-06-30-serial-336-flight-004.csv",
    "2012-07-03-serial-058-flight-007.csv", "2012-09-03-serial-336-flight-005.csv",
    "2012-09-15-serial-892-flight-004.csv", "2012-09-29-serial-341-flight-005.csv",
    "2012-10-20-serial-068-flight-004.csv", "2012-10-20-serial-336-flight-006.csv",
    "2012-11-17-serial-209-flight-013.csv",
};
#define N_FLIGHTS (int)(sizeof(FLIGHTS) / sizeof(FLIGHTS[0]))

/* AltOS's apogee is from its accelerometer too, the nearest to the truth a
 * log has. Its charge, or the motor's, separates the rocket at or just after
 * the top; past that the log thins to ten rows a second, and a replay can
 * place an apogee there only to within seconds. */
void test_TST_02_every_altos_flight_is_launched_and_has_its_apogee(void) {
    char bad[512] = "";
    for (int i = 0; i < N_FLIGHTS; i++) {
        replayed_t r = replay(FLIGHTS[i]);
        float from_altos_s = r.apogee_s - altos_apogee_s;
        if (r.launch_s < 0.0f || r.apogee_s < 0.0f || from_altos_s < -EARLY_MAX_S || from_altos_s > LATE_MAX_S) {
            char item[96];
            snprintf(item, sizeof(item), " %.32s: launch %.2f, apogee %+.2f s;", FLIGHTS[i], (double)r.launch_s,
                     (double)from_altos_s);
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_TST_02_every_altos_flight_is_launched_and_has_its_apogee);
    return UNITY_END();
}
