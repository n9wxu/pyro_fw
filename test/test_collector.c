/*
 * The pressure collector's interrupt state machine [DD-093] on a fake bus,
 * clock and part (collector_bus.h), with each sensor's own description.
 *
 * The part is the MS5607's kind: a read before its conversion has finished,
 * or with no conversion before it, answers zero
 * (docs/datasheets/MS5607-02BA03_2017-06.pdf page 11). Each conversion
 * answers with a code of its own, so a cycle names the conversions it holds.
 *
 * Verifies [SNS-COL-01..05, SNS-PRES-08, SNS-PRES-14, FLT-RATE-01].
 */
#include "unity.h"
#include "collector_bus.h"
#include "bmp280_driver.h"
#include "ms5607_driver.h"
#include <stdio.h>

fake_bus_t fake_bus;

/* ── The part ─────────────────────────────────────────────────────── */

#define WRITES_KEPT 64

static struct {
    uint32_t converts_us; /* how long this part really takes */
    bool converting;      /* a conversion commanded and not yet read */
    uint64_t done_at;
    uint64_t commanded_at; /* the end of the newest conversion's command */
    uint32_t conversions, zero_answers, reads;
    uint64_t soonest_read_us; /* the shortest wait any read gave its conversion */
    uint8_t written[WRITES_KEPT][COLLECTOR_COMMAND_MAX];
    uint8_t written_len[WRITES_KEPT];
    int writes;
    uint8_t read_from;
    const collector_part_t *kind;
} part;

static bool is_conversion(const uint8_t *bytes, uint8_t len) {
    for (uint8_t i = 0; i < part.kind->steps; i++)
        if (len == part.kind->step[i].command_len && memcmp(bytes, part.kind->step[i].command, len) == 0)
            return true;
    return false;
}

void fake_part_written(const uint8_t *bytes, uint8_t len, uint64_t ended_at) {
    if (part.writes < WRITES_KEPT) {
        memcpy(part.written[part.writes], bytes, len);
        part.written_len[part.writes++] = len;
    }
    if (!is_conversion(bytes, len))
        return;
    part.converting = true;
    part.commanded_at = ended_at;
    part.done_at = ended_at + part.converts_us;
    part.conversions++;
}

void fake_part_read(uint8_t from_register, uint8_t *data, uint8_t len, uint64_t at) {
    part.reads++;
    part.read_from = from_register;
    memset(data, 0, len);
    if (!part.converting || at < part.done_at) {
        part.zero_answers++;
        return;
    }
    part.converting = false;
    if (at - part.commanded_at < part.soonest_read_us)
        part.soonest_read_us = at - part.commanded_at;
    data[0] = (uint8_t)(part.conversions >> 16);
    data[1] = (uint8_t)(part.conversions >> 8);
    data[2] = (uint8_t)part.conversions;
}

static uint32_t conversion_of(const collector_raw_t *raw, int step) {
    return ms5607_code(raw->data[step]);
}

/* ── The clock ────────────────────────────────────────────────────── */

static void interrupts(void) {
    while (fake_bus.forced && !fake_bus.held_off)
        fake_bus.handler();
}

/* Time passes to t, and each alarm fires when it comes due. */
static void run_to(uint64_t t) {
    interrupts();
    while (fake_bus.armed && fake_bus.alarm_at <= t) {
        if (fake_bus.now < fake_bus.alarm_at)
            fake_bus.now = fake_bus.alarm_at;
        fake_bus.armed = false;
        fake_bus.handler();
        interrupts();
    }
    if (fake_bus.now < t)
        fake_bus.now = t;
}

static void run_for(uint64_t us) {
    run_to(fake_bus.now + us);
}

/* A flash erase holds interrupts off until t; what came due meanwhile fires
 * as it ends. */
static void held_until(uint64_t t) {
    if (fake_bus.now < t)
        fake_bus.now = t;
    run_to(t);
}

static int take_all(collector_raw_t *last) {
    int n = 0;
    collector_raw_t raw;
    while (collector_take(&raw)) {
        n++;
        if (last)
            *last = raw;
    }
    return n;
}

static collector_stats_t stats(void) {
    collector_stats_t s;
    collector_stats(&s);
    return s;
}

static uint32_t failures(void) {
    collector_stats_t s = stats();
    uint32_t n = 0;
    for (int i = 0; i < COLLECTOR_CAUSES; i++)
        n += s.failed[i];
    return n;
}

/* ── Each test starts a quiet, running machine ────────────────────── */

static const collector_wiring_t MS5607_AT_400K = {1, 0x77, 7, 6, 400000u};
static const collector_wiring_t BMP280_AT_100K = {1, 0x76, 7, 6, 100000u};

static collector_stats_t before;

/* The machine is one for the life of the program, as on the board. Each
 * test gets it between cycles, with an empty queue. */
static void quiet(void) {
    fake_bus.fail = COLLECTOR_OK;
    fake_bus.sda_stuck_clocks = 0;
    run_for(100000u);
    (void)take_all(NULL);
    before = stats();
    part.writes = 0;
    part.zero_answers = 0;
    part.soonest_read_us = UINT64_MAX;
    fake_bus.clocks = fake_bus.stops = fake_bus.takes = fake_bus.gives = 0;
    fake_bus.shortest_half_bit = UINT64_MAX;
}

void setUp(void) {
    part.kind = &MS5607_PART;
    part.converts_us = 9040u; /* the datasheet's worst case, page 3 */
    TEST_ASSERT_TRUE(collector_begin(&MS5607_PART, &MS5607_AT_400K));
    quiet();
}

void tearDown(void) {}

/* Runs to the next cycle's first command, so a test can count from it. */
static uint64_t next_cycle_starts(void) {
    for (;;) {
        uint32_t was = part.conversions;
        run_to(fake_bus.alarm_at);
        if (part.conversions != was && part.written[(part.writes - 1) % WRITES_KEPT][0] == MS5607_CMD_CONVERT_D1)
            return part.commanded_at;
        if (part.writes >= WRITES_KEPT)
            part.writes = 0;
    }
}

/* ── Free-running [SNS-COL-01] ────────────────────────────────────── */

void test_SNS_COL_01_it_runs_with_nothing_starting_it(void) {
    run_for(1000000u);
    collector_stats_t s = stats();
    TEST_ASSERT_TRUE_MESSAGE(s.cycles - before.cycles >= 50u, "a second of cycles, and no call but the alarm's");
    TEST_ASSERT_EQUAL_UINT32(0, failures());
}

/* [FLT-RATE-01] Faster than the flight software's 50 a second, on each
 * sensor at the slowest bus a board runs it. */
void test_FLT_RATE_01_each_sensor_outruns_the_flight_software(void) {
    uint32_t ms5607 = collector_cycle_us(&MS5607_PART, &MS5607_AT_400K);
    uint32_t bmp280 = collector_cycle_us(&BMP280_PART, &BMP280_AT_100K);
    char msg[96];
    snprintf(msg, sizeof(msg), "MS5607 %lu us a cycle, BMP280 at 100 kHz %lu us", (unsigned long)ms5607,
             (unsigned long)bmp280);
    TEST_ASSERT_TRUE_MESSAGE(ms5607 < 20000u && bmp280 < 20000u, msg);
    run_for(1000000u);
    TEST_ASSERT_UINT32_WITHIN(1, 1000000u / ms5607, stats().cycles - before.cycles);
}

void test_SNS_COL_01_a_cycle_is_each_conversion_in_turn(void) {
    (void)next_cycle_starts();
    part.writes = 0;
    (void)take_all(NULL);
    run_for(collector_cycle_us(&MS5607_PART, &MS5607_AT_400K) + 200u);
    TEST_ASSERT_TRUE(part.writes >= 2);
    TEST_ASSERT_EQUAL_HEX8(MS5607_CMD_CONVERT_D2, part.written[0][0]);
    TEST_ASSERT_EQUAL_HEX8(MS5607_CMD_CONVERT_D1, part.written[1][0]);
    TEST_ASSERT_EQUAL_HEX8(MS5607_CMD_ADC_READ, part.read_from);
    collector_raw_t raw;
    TEST_ASSERT_TRUE(collector_take(&raw));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(conversion_of(&raw, MS5607_PRESSURE) + 1u, conversion_of(&raw, MS5607_TEMPERATURE),
                                     "the temperature is the conversion after its pressure");
}

/* ── No read is early, and none is repeated [SNS-COL-02] ──────────── */

void test_SNS_COL_02_every_read_waits_out_the_worst_case(void) {
    run_for(2000000u);
    char msg[96];
    snprintf(msg, sizeof(msg), "soonest read %lu us after its command; %lu zero answers",
             (unsigned long)part.soonest_read_us, (unsigned long)part.zero_answers);
    TEST_ASSERT_TRUE_MESSAGE(part.soonest_read_us >= 9040u, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, part.zero_answers, msg);
}

/* Interrupts held off for anything up to 400 ms, at any point in a cycle:
 * a read comes later, never sooner, and none finds a conversion unfinished
 * or already read. */
void test_SNS_COL_02_no_zero_however_the_handler_is_held_off(void) {
    uint32_t seed = 12345u;
    collector_raw_t raw;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1664525u + 1013904223u;
        run_for(seed % 23000u);
        seed = seed * 1664525u + 1013904223u;
        if (i % 3 == 0)
            held_until(fake_bus.now + (seed >> 8) % 400000u);
        while (collector_take(&raw)) {
            TEST_ASSERT_TRUE(conversion_of(&raw, MS5607_PRESSURE) != 0u);
            TEST_ASSERT_TRUE(conversion_of(&raw, MS5607_TEMPERATURE) != 0u);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, part.zero_answers);
    TEST_ASSERT_TRUE(part.soonest_read_us >= 9040u);
    TEST_ASSERT_EQUAL_UINT32(0, failures());
}

/* ── Stamps [SNS-PRES-08] ─────────────────────────────────────────── */

void test_SNS_PRES_08_each_stamp_is_the_middle_of_its_conversion(void) {
    (void)take_all(NULL);
    uint64_t pressure_commanded = next_cycle_starts();
    run_for(MS5607_CONV_DONE_US + 1000u);
    uint64_t temperature_commanded = part.commanded_at;
    run_for(MS5607_CONV_DONE_US + 1000u);
    collector_raw_t raw;
    while (collector_take(&raw))
        if (raw.at_us[MS5607_PRESSURE] > pressure_commanded)
            break;
    TEST_ASSERT_EQUAL_UINT64(pressure_commanded + MS5607_HALF_CONV_US, raw.at_us[MS5607_PRESSURE]);
    TEST_ASSERT_EQUAL_UINT64(temperature_commanded + MS5607_HALF_CONV_US, raw.at_us[MS5607_TEMPERATURE]);
}

/* A flash erase that holds the pressure's read off 60 ms moves the read, and
 * so the temperature after it, not the pressure's stamp. */
void test_SNS_PRES_08_a_held_read_keeps_its_stamp(void) {
    uint64_t commanded = next_cycle_starts();
    (void)take_all(NULL);
    held_until(commanded + 60000u);
    run_for(30000u);
    collector_raw_t raw;
    TEST_ASSERT_TRUE(collector_take(&raw));
    TEST_ASSERT_EQUAL_UINT64(commanded + MS5607_HALF_CONV_US, raw.at_us[MS5607_PRESSURE]);
    TEST_ASSERT_TRUE(raw.at_us[MS5607_TEMPERATURE] > commanded + 60000u);
}

/* ── The queue [SNS-COL-03] ───────────────────────────────────────── */

void test_SNS_COL_03_a_task_60_ms_late_loses_nothing(void) {
    uint32_t made = stats().cycles;
    run_for(60000u);
    made = stats().cycles - made;
    TEST_ASSERT_TRUE(made >= 3u);
    TEST_ASSERT_EQUAL_INT((int)made, take_all(NULL));
    TEST_ASSERT_EQUAL_UINT32(before.dropped, stats().dropped);
}

void test_SNS_COL_03_four_are_kept_and_the_oldest_goes_first(void) {
    uint32_t made = stats().cycles;
    run_for(1000000u);
    made = stats().cycles - made;
    collector_raw_t raw[COLLECTOR_QUEUE + 1];
    int kept = 0;
    while (kept <= COLLECTOR_QUEUE && collector_take(&raw[kept]))
        kept++;
    TEST_ASSERT_EQUAL_INT(COLLECTOR_QUEUE, kept);
    TEST_ASSERT_EQUAL_UINT32(made - COLLECTOR_QUEUE, stats().dropped - before.dropped);
    for (int i = 1; i < kept; i++)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(conversion_of(&raw[i - 1], MS5607_PRESSURE) + MS5607_PART.steps,
                                         conversion_of(&raw[i], MS5607_PRESSURE),
                                         "in order, oldest first, none skipped");
    TEST_ASSERT_TRUE_MESSAGE(part.conversions - conversion_of(&raw[kept - 1], MS5607_TEMPERATURE) <= 2u,
                             "the newest kept is the newest made");
}

void test_SNS_COL_03_a_cycle_is_taken_once(void) {
    (void)take_all(NULL);
    run_for(collector_cycle_us(&MS5607_PART, &MS5607_AT_400K) + 500u);
    collector_raw_t raw;
    TEST_ASSERT_TRUE(collector_take(&raw));
    TEST_ASSERT_FALSE(collector_take(&raw));
}

/* The handler is held off while the task takes a cycle. */
void test_SNS_COL_03_the_handler_does_not_run_inside_a_take(void) {
    collector_raw_t raw;
    (void)collector_take(&raw);
    TEST_ASSERT_EQUAL_INT(0, fake_bus.held_off);
}

/* ── A flash operation beside a conversion [SNS-PRES-14] ──────────── */

void test_SNS_PRES_14_a_flash_operation_marks_the_conversion_it_ran_beside(void) {
    uint64_t commanded = next_cycle_starts();
    (void)take_all(NULL);
    run_to(commanded + 3000u);
    fake_bus.flash_ops++;
    run_for(collector_cycle_us(&MS5607_PART, &MS5607_AT_400K));
    collector_raw_t raw;
    TEST_ASSERT_TRUE(collector_take(&raw));
    TEST_ASSERT_TRUE(raw.flashed[MS5607_PRESSURE]);
    TEST_ASSERT_FALSE(raw.flashed[MS5607_TEMPERATURE]);

    commanded = next_cycle_starts();
    (void)take_all(NULL);
    run_to(commanded + MS5607_CONV_DONE_US + 3000u);
    fake_bus.flash_ops++;
    run_for(collector_cycle_us(&MS5607_PART, &MS5607_AT_400K));
    TEST_ASSERT_TRUE(collector_take(&raw));
    TEST_ASSERT_FALSE(raw.flashed[MS5607_PRESSURE]);
    TEST_ASSERT_TRUE(raw.flashed[MS5607_TEMPERATURE]);
}

/* ── Failures, by cause [SNS-COL-04] ──────────────────────────────── */

static void one_failure_of(collector_cause_t cause) {
    quiet();
    uint32_t cycles = stats().cycles;
    fake_bus.fail = cause;
    fake_bus.fail_transfers = 1;
    run_for(100000u);
    collector_stats_t s = stats();
    char msg[64];
    snprintf(msg, sizeof(msg), "cause %d", (int)cause);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before.failed[cause] + 1u, s.failed[cause], msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before.recoveries, s.recoveries, "one failure is no reason to clear the bus");
    TEST_ASSERT_TRUE_MESSAGE(s.cycles - cycles >= 3u, "and the machine carries on");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, part.zero_answers, msg);
}

void test_SNS_COL_04_a_failed_transfer_is_counted_by_its_cause(void) {
    one_failure_of(COLLECTOR_ADDRESS_NACK);
    one_failure_of(COLLECTOR_DATA_NACK);
    one_failure_of(COLLECTOR_LINE_HELD);
    one_failure_of(COLLECTOR_TIMEOUT);
}

void test_SNS_COL_04_a_transfer_that_never_ends_is_aborted(void) {
    uint32_t aborts = fake_bus.aborts;
    fake_bus.fail = COLLECTOR_TIMEOUT;
    fake_bus.fail_transfers = 1;
    run_for(100000u);
    TEST_ASSERT_EQUAL_UINT32(aborts + 1u, fake_bus.aborts);
}

/* A cycle whose second conversion fails is not half a cycle on the queue:
 * the next one starts again from its pressure. */
void test_SNS_COL_04_a_failed_cycle_is_not_queued(void) {
    (void)take_all(NULL);
    uint64_t commanded = next_cycle_starts();
    run_to(commanded + MS5607_CONV_DONE_US + 1000u);
    (void)take_all(NULL);
    fake_bus.fail = COLLECTOR_DATA_NACK;
    fake_bus.fail_transfers = 1;
    run_for(60000u);
    collector_raw_t raw;
    while (collector_take(&raw))
        TEST_ASSERT_EQUAL_UINT32(conversion_of(&raw, MS5607_PRESSURE) + 1u, conversion_of(&raw, MS5607_TEMPERATURE));
}

/* ── Recovery [SNS-COL-05] ────────────────────────────────────────── */

/* The part holds SDA low until it has been clocked five times: the bus clear
 * of UM10204 3.1.16, then the part's own reset (MS5607 page 12), then its
 * PROM's reload, and the cycles come again. */
void test_SNS_COL_05_three_failures_clear_the_bus_and_reset_the_part(void) {
    fake_bus.sda_stuck_clocks = 5;
    run_for(200000u);
    collector_stats_t s = stats();
    TEST_ASSERT_EQUAL_UINT32(before.failed[COLLECTOR_LINE_HELD] + 3u, s.failed[COLLECTOR_LINE_HELD]);
    TEST_ASSERT_EQUAL_UINT32(before.recoveries + 1u, s.recoveries);
    TEST_ASSERT_EQUAL_INT(9, fake_bus.clocks);
    TEST_ASSERT_EQUAL_INT(1, fake_bus.stops);
    TEST_ASSERT_EQUAL_INT(1, fake_bus.gives);
    TEST_ASSERT_FALSE(fake_bus.lines_taken);
    TEST_ASSERT_TRUE_MESSAGE(fake_bus.shortest_half_bit >= 5u, "no faster than the bus's standard mode needs");

    int reset_at = -1;
    for (int i = 0; i < part.writes; i++)
        if (part.written[i][0] == MS5607_CMD_RESET)
            reset_at = i;
    TEST_ASSERT_TRUE_MESSAGE(reset_at >= 0, "the reset command follows the clear");
    TEST_ASSERT_TRUE_MESSAGE(take_all(NULL) >= 3, "and cycles come again");
    TEST_ASSERT_EQUAL_UINT32(0, part.zero_answers);
}

/* The first conversion after a reset waits out the part's reload. */
void test_SNS_COL_05_the_part_is_left_to_reload_after_its_reset(void) {
    fake_bus.fail = COLLECTOR_ADDRESS_NACK;
    fake_bus.fail_transfers = 3;
    uint64_t reset_ended = 0;
    for (int i = 0; i < 20000 && reset_ended == 0; i++) {
        int writes = part.writes;
        run_for(10u);
        if (part.writes > writes && part.written[part.writes - 1][0] == MS5607_CMD_RESET)
            reset_ended = fake_bus.done_at;
    }
    TEST_ASSERT_TRUE(reset_ended != 0);
    uint32_t conversions = part.conversions;
    run_to(reset_ended + MS5607_PART.reset_us - 10u);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(conversions, part.conversions, "nothing is commanded inside the reload");
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(conversions + 1u, part.conversions);
}

/* A part that never answers: the machine goes on trying, one recovery to
 * every three failures, and is ready the moment the part is. */
void test_SNS_COL_05_a_silent_part_is_tried_until_it_answers(void) {
    fake_bus.fail = COLLECTOR_ADDRESS_NACK;
    fake_bus.fail_transfers = -1;
    run_for(1000000u);
    collector_stats_t s = stats();
    uint32_t failed = s.failed[COLLECTOR_ADDRESS_NACK] - before.failed[COLLECTOR_ADDRESS_NACK];
    uint32_t recovered = s.recoveries - before.recoveries;
    char msg[96];
    snprintf(msg, sizeof(msg), "%lu failures and %lu recoveries in a second", (unsigned long)failed,
             (unsigned long)recovered);
    TEST_ASSERT_TRUE_MESSAGE(recovered >= 10u && recovered <= 500u, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, take_all(NULL), "and nothing is queued");

    fake_bus.fail = COLLECTOR_OK;
    uint32_t cycles = stats().cycles;
    run_for(100000u);
    TEST_ASSERT_TRUE(stats().cycles - cycles >= 3u);
}

/* ── The BMP280's description ─────────────────────────────────────── */

/* One forced conversion a cycle, read in one burst from 0xF7
 * (docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf pages 20 and 25), and no
 * sooner than Table 13's 13.3 ms. */
void test_SNS_COL_02_the_bmp280_is_read_after_its_worst_case(void) {
    TEST_ASSERT_EQUAL_UINT8(1, BMP280_PART.steps);
    TEST_ASSERT_EQUAL_HEX8(0xF4, BMP280_PART.step[0].command[0]);
    TEST_ASSERT_EQUAL_HEX8(0x2D, BMP280_PART.step[0].command[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF7, BMP280_PART.step[0].read_register);
    TEST_ASSERT_EQUAL_UINT8(6, BMP280_PART.step[0].read_len);
    TEST_ASSERT_TRUE(BMP280_PART.step[0].convert_us >= 13300u);
    TEST_ASSERT_EQUAL_HEX8(0xE0, BMP280_PART.reset[0]);
    TEST_ASSERT_EQUAL_HEX8(0xB6, BMP280_PART.reset[1]);
    TEST_ASSERT_TRUE(BMP280_PART.reset_us >= 2000u);
}

void test_SNS_COL_02_the_ms5607_is_read_after_its_worst_case(void) {
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(MS5607_PART.step[i].convert_us >= 9040u);
        TEST_ASSERT_EQUAL_UINT8(3, MS5607_PART.step[i].read_len);
    }
    TEST_ASSERT_TRUE_MESSAGE(MS5607_PART.reset_us >= 2800u, "the PROM's reload, pages 10 and 11");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_COL_01_it_runs_with_nothing_starting_it);
    RUN_TEST(test_FLT_RATE_01_each_sensor_outruns_the_flight_software);
    RUN_TEST(test_SNS_COL_01_a_cycle_is_each_conversion_in_turn);
    RUN_TEST(test_SNS_COL_02_every_read_waits_out_the_worst_case);
    RUN_TEST(test_SNS_COL_02_no_zero_however_the_handler_is_held_off);
    RUN_TEST(test_SNS_PRES_08_each_stamp_is_the_middle_of_its_conversion);
    RUN_TEST(test_SNS_PRES_08_a_held_read_keeps_its_stamp);
    RUN_TEST(test_SNS_COL_03_a_task_60_ms_late_loses_nothing);
    RUN_TEST(test_SNS_COL_03_four_are_kept_and_the_oldest_goes_first);
    RUN_TEST(test_SNS_COL_03_a_cycle_is_taken_once);
    RUN_TEST(test_SNS_COL_03_the_handler_does_not_run_inside_a_take);
    RUN_TEST(test_SNS_PRES_14_a_flash_operation_marks_the_conversion_it_ran_beside);
    RUN_TEST(test_SNS_COL_04_a_failed_transfer_is_counted_by_its_cause);
    RUN_TEST(test_SNS_COL_04_a_transfer_that_never_ends_is_aborted);
    RUN_TEST(test_SNS_COL_04_a_failed_cycle_is_not_queued);
    RUN_TEST(test_SNS_COL_05_three_failures_clear_the_bus_and_reset_the_part);
    RUN_TEST(test_SNS_COL_05_the_part_is_left_to_reload_after_its_reset);
    RUN_TEST(test_SNS_COL_05_a_silent_part_is_tried_until_it_answers);
    RUN_TEST(test_SNS_COL_02_the_bmp280_is_read_after_its_worst_case);
    RUN_TEST(test_SNS_COL_02_the_ms5607_is_read_after_its_worst_case);
    return UNITY_END();
}
