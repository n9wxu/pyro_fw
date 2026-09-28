/*
 * The MS5607 one-shot's interrupt state machine [DD-051, DD-066], on a fake
 * bus and clock (ms5607_bus.h). The loop starts a pair -- a pressure, then a
 * temperature -- and takes both a loop later; between the two, only the
 * handler runs, and the stamps are its own.
 */
#include "unity.h"
#include "ms5607_driver.h"
#include "ms5607_bus.h"
#include "loop_period.h"
#include <stdio.h>

uint64_t fake_bus_now;
uint32_t fake_bus_adc;
uint32_t fake_bus_adc_t;
uint8_t fake_bus_last_cmd;
bool fake_bus_nack;
uint8_t fake_bus_cmds[64];
int fake_bus_ncmds;
int fake_bus_reads;
uint64_t fake_bus_alarm_at;
bool fake_bus_armed;
bool fake_bus_forced;
uint8_t fake_bus_address;
void (*fake_bus_handler)(void);

uint8_t ms5607_address(void) {
    return 0x77;
}

#define CONV_D1 0x48
#define CONV_D2 0x58

/* A forced interrupt is taken at once: the loop's start preempts itself. */
static void interrupts(void) {
    while (fake_bus_forced)
        fake_bus_handler();
}

/* Time passes to t, and each alarm fires when it comes due. */
static void run_to(uint64_t t) {
    interrupts();
    while (fake_bus_armed && fake_bus_alarm_at <= t) {
        if (fake_bus_now < fake_bus_alarm_at)
            fake_bus_now = fake_bus_alarm_at;
        fake_bus_armed = false;
        fake_bus_handler();
        interrupts();
    }
    if (fake_bus_now < t)
        fake_bus_now = t;
}

/* A flash erase holds interrupts off until t; what came due meanwhile fires
 * as it ends. */
static void held_until(uint64_t t) {
    if (fake_bus_now < t)
        fake_bus_now = t;
    run_to(t);
}

void setUp(void) {
    TEST_ASSERT_TRUE(ms5607_async_begin());
    fake_bus_nack = false;
    run_to(fake_bus_now + 40000u);
    ms5607_pair_t c;
    (void)ms5607_async_take(&c);
    fake_bus_ncmds = 0;
    fake_bus_reads = 0;
    fake_bus_adc = 6465444u;
    fake_bus_adc_t = 8077636u;
    fake_bus_now = 1000000u;
}

void tearDown(void) {}

/* When the pressure's conversion ends, which is when the temperature's can
 * start: the command, the conversion's worst case, the read. */
#define D1_READ_END (1000000u + FAKE_BUS_COMMAND_US + MS5607_CONV_DONE_US + FAKE_BUS_READ_US)
#define D2_AT (D1_READ_END + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US)

/* Started at t: the pressure's command ends at t plus a command, and its
 * reading describes the middle of that conversion. The temperature's is
 * commanded as the pressure is read, and describes the middle of its own. */
void test_SNS_PRES_08_stamps_are_the_conversions(void) {
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start());
    interrupts();
    TEST_ASSERT_EQUAL(1, fake_bus_ncmds);
    TEST_ASSERT_EQUAL_HEX8(CONV_D1, fake_bus_cmds[0]);
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_EQUAL(2, fake_bus_ncmds);
    TEST_ASSERT_EQUAL_HEX8(CONV_D2, fake_bus_cmds[1]);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL_UINT32(6465444u, c.d1);
    TEST_ASSERT_EQUAL_UINT32(8077636u, c.d2);
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.d1_at_us);
    TEST_ASSERT_EQUAL_UINT64(D2_AT, c.d2_at_us);
}

/* An erase that holds the pressure's read off 60 ms moves the read, and so
 * the temperature, not the pressure's stamp. */
void test_SNS_PRES_08_held_read_keeps_its_stamp(void) {
    ms5607_async_start();
    interrupts();
    held_until(1060000u);
    TEST_ASSERT_EQUAL(1, fake_bus_reads);
    run_to(1080000u);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.d1_at_us);
    TEST_ASSERT_TRUE(c.d2_at_us > 1060000u);
}

/* A loop that comes to take it 50 ms late changes nothing either. */
void test_SNS_PRES_08_late_loop_keeps_the_stamps(void) {
    ms5607_async_start();
    run_to(1070000u);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.d1_at_us);
    TEST_ASSERT_EQUAL_UINT64(D2_AT, c.d2_at_us);
}

/* Each read comes no sooner than the datasheet's 9.04 ms after its command:
 * earlier, the sensor answers 0. */
void test_SNS_PRES_05_each_read_after_worst_case(void) {
    ms5607_async_start();
    interrupts();
    uint64_t command_end = fake_bus_now;
    TEST_ASSERT_TRUE(fake_bus_armed);
    TEST_ASSERT_TRUE(fake_bus_alarm_at >= command_end + 9040u);
    run_to(fake_bus_alarm_at - 1u);
    TEST_ASSERT_EQUAL(0, fake_bus_reads);
    run_to(fake_bus_alarm_at);
    TEST_ASSERT_EQUAL(1, fake_bus_reads);
    uint64_t d2_command_end = fake_bus_now;
    TEST_ASSERT_TRUE(fake_bus_armed);
    TEST_ASSERT_TRUE(fake_bus_alarm_at >= d2_command_end + 9040u);
    run_to(fake_bus_alarm_at - 1u);
    TEST_ASSERT_EQUAL(1, fake_bus_reads);
    ms5607_pair_t c;
    TEST_ASSERT_FALSE(ms5607_async_take(&c));
}

/* One pair at a time: a start while either half is in flight sends nothing. */
void test_ms5607_busy_until_the_pair_is_read(void) {
    ms5607_async_start();
    interrupts();
    TEST_ASSERT_EQUAL(MS5607_BUSY, ms5607_async_start());
    run_to(1000000u + 10000u);
    TEST_ASSERT_EQUAL(MS5607_BUSY, ms5607_async_start());
    interrupts();
    TEST_ASSERT_EQUAL(2, fake_bus_ncmds);
    run_to(1000000u + LOOP_PERIOD_US);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start());
    interrupts();
    TEST_ASSERT_EQUAL(3, fake_bus_ncmds);
}

/* A command the sensor does not answer ends the pair as failed, arms
 * nothing, and leaves the next start free. */
void test_ms5607_command_nack(void) {
    fake_bus_nack = true;
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start());
    interrupts();
    TEST_ASSERT_FALSE(fake_bus_armed);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(c.ok);
    fake_bus_nack = false;
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start());
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_TRUE(c.ok);
}

/* The pressure's read unanswered: no temperature is commanded. */
void test_ms5607_pressure_read_nack(void) {
    ms5607_async_start();
    interrupts();
    fake_bus_nack = true;
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_EQUAL(1, fake_bus_ncmds);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(c.ok);
}

/* The temperature's read unanswered: the pair fails with it. */
void test_ms5607_temperature_read_nack(void) {
    ms5607_async_start();
    run_to(1000000u + 12000u);
    TEST_ASSERT_EQUAL(2, fake_bus_ncmds);
    fake_bus_nack = true;
    run_to(1000000u + LOOP_PERIOD_US);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(c.ok);
}

void test_ms5607_taken_once(void) {
    ms5607_async_start();
    run_to(1000000u + LOOP_PERIOD_US);
    ms5607_pair_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(ms5607_async_take(&c));
}

/* Started at the top of a loop, the pair is ready with half a millisecond to
 * spare before the next top, which is never sooner than a period on. The
 * spare is for the interrupt latency and the work ahead of the pressure task
 * at the top of the loop; a pair not ready is a loop without a sample. */
void test_ms5607_pair_ready_before_the_next_loop(void) {
    ms5607_async_start();
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_EQUAL(2, fake_bus_reads);
    uint64_t ready = fake_bus_alarm_at + FAKE_BUS_READ_US;
    char msg[80];
    snprintf(msg, sizeof(msg), "ready %llu us into a %u us loop", (unsigned long long)(ready - 1000000u),
             LOOP_PERIOD_US);
    TEST_ASSERT_TRUE_MESSAGE(ready + 500u <= 1000000u + LOOP_PERIOD_US, msg);
}

/* The next pair is started before the one taken is handed back, so whatever
 * the caller does with it cannot delay the next command; and the pair's
 * temperature is on the line by then. */
void test_ms5607_cycle_notes_the_temperature_and_starts_the_next(void) {
    ms5607_temps_t t = {0};
    ms5607_pair_t c;
    ms5607_start_t started;
    TEST_ASSERT_FALSE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_EQUAL(MS5607_STARTED, started);
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_TRUE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL(1, t.n);
    TEST_ASSERT_EQUAL_UINT32(8077636u, t.d2[0]);
    TEST_ASSERT_EQUAL(MS5607_STARTED, started);
    TEST_ASSERT_TRUE(fake_bus_forced);
}

/* Measured on an MK1B: a pressure's compensation, filter and fit take up to
 * 2.7 ms of the loop. The pair must not cost a sample for it. */
#define PRESSURE_WORK_US 2700u

void test_ms5607_a_pair_every_loop(void) {
    ms5607_temps_t t = {0};
    uint64_t top = fake_bus_now;
    int busy = 0, pairs = 0;
    for (int loop = 0; loop < 200; loop++) {
        ms5607_pair_t c;
        ms5607_start_t started;
        bool took = ms5607_async_cycle(&t, &c, &started);
        interrupts();
        if (started == MS5607_BUSY)
            busy++;
        if (took && c.ok) {
            pairs++;
            fake_bus_now += PRESSURE_WORK_US;
        }
        top += LOOP_PERIOD_US;
        run_to(top);
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "%d of 200 loops waited; %d pairs", busy, pairs);
    TEST_ASSERT_EQUAL_MESSAGE(0, busy, msg);
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 199, pairs, msg);
}

/* A read the sensor did not answer starts nothing: the caller backs off
 * rather than retrying every loop against a missing sensor. */
void test_ms5607_cycle_holds_off_after_a_failed_read(void) {
    ms5607_temps_t t = {0};
    ms5607_pair_t c;
    ms5607_start_t started;
    ms5607_async_cycle(&t, &c, &started);
    interrupts();
    fake_bus_nack = true;
    run_to(1000000u + LOOP_PERIOD_US);
    TEST_ASSERT_TRUE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_FALSE(c.ok);
    TEST_ASSERT_EQUAL(MS5607_HELD, started);
    TEST_ASSERT_FALSE(fake_bus_forced);
}

/* [SNS-PRES-12] A pressure is compensated with the temperature at its own
 * time: between two readings, the line's value there, not the newest's. */
void test_SNS_PRES_12_the_line_interpolates_back(void) {
    ms5607_temps_t t = {0};
    ms5607_temps_note(&t, 8000000u, 1000000u);
    ms5607_temps_note(&t, 8000200u, 1020000u);
    TEST_ASSERT_EQUAL_UINT32(8000110u, ms5607_temps_at(&t, 1011000u));
    TEST_ASSERT_EQUAL_UINT32(8000000u, ms5607_temps_at(&t, 900000u)); /* no further back than the oldest */
    TEST_ASSERT_EQUAL_UINT32(8000300u, ms5607_temps_at(&t, 1030000u));
}

void test_ms5607_begin_addresses_the_sensor(void) {
    TEST_ASSERT_EQUAL_HEX8(0x77, fake_bus_address);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_PRES_08_stamps_are_the_conversions);
    RUN_TEST(test_SNS_PRES_08_held_read_keeps_its_stamp);
    RUN_TEST(test_SNS_PRES_08_late_loop_keeps_the_stamps);
    RUN_TEST(test_SNS_PRES_05_each_read_after_worst_case);
    RUN_TEST(test_ms5607_busy_until_the_pair_is_read);
    RUN_TEST(test_ms5607_command_nack);
    RUN_TEST(test_ms5607_pressure_read_nack);
    RUN_TEST(test_ms5607_temperature_read_nack);
    RUN_TEST(test_ms5607_taken_once);
    RUN_TEST(test_ms5607_pair_ready_before_the_next_loop);
    RUN_TEST(test_ms5607_cycle_notes_the_temperature_and_starts_the_next);
    RUN_TEST(test_ms5607_a_pair_every_loop);
    RUN_TEST(test_ms5607_cycle_holds_off_after_a_failed_read);
    RUN_TEST(test_SNS_PRES_12_the_line_interpolates_back);
    RUN_TEST(test_ms5607_begin_addresses_the_sensor);
    return UNITY_END();
}
