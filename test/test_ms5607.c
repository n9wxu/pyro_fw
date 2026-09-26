/*
 * The MS5607 one-shot's interrupt state machine [DD-051], on a fake bus and
 * clock (ms5607_bus.h). The loop starts a conversion and takes the result;
 * between the two, only the handler runs, and the stamp is its own.
 */
#include "unity.h"
#include "ms5607_driver.h"
#include "ms5607_bus.h"
#include <stdio.h>

uint64_t fake_bus_now;
uint32_t fake_bus_adc;
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
    run_to(fake_bus_now + 20000u);
    ms5607_conversion_t c;
    (void)ms5607_async_take(&c);
    fake_bus_ncmds = 0;
    fake_bus_reads = 0;
    fake_bus_adc = 6465444u;
    fake_bus_now = 1000000u;
}

void tearDown(void) {}

/* Started at t, the command ends at t + 200 us; the reading describes the
 * middle of the conversion that began there. */
void test_SNS_PRES_08_stamp_is_the_conversions(void) {
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start(false));
    interrupts();
    TEST_ASSERT_EQUAL(1, fake_bus_ncmds);
    TEST_ASSERT_EQUAL_HEX8(CONV_D1, fake_bus_cmds[0]);
    run_to(1020000u);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_FALSE(c.temperature);
    TEST_ASSERT_EQUAL_UINT32(6465444u, c.raw);
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.at_us);
}

/* An erase that holds the read off 60 ms moves the read, not the stamp. */
void test_SNS_PRES_08_held_read_keeps_its_stamp(void) {
    ms5607_async_start(false);
    interrupts();
    held_until(1060000u);
    TEST_ASSERT_EQUAL(1, fake_bus_reads);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.at_us);
}

/* A loop that comes to take it 50 ms late changes nothing either. */
void test_SNS_PRES_08_late_loop_keeps_the_stamp(void) {
    ms5607_async_start(false);
    run_to(1060000u);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL_UINT64(1000000u + FAKE_BUS_COMMAND_US + MS5607_HALF_CONV_US, c.at_us);
}

/* The read comes no sooner than the datasheet's 9.04 ms after the command:
 * earlier, the sensor answers 0. */
void test_SNS_PRES_05_read_after_worst_case(void) {
    ms5607_async_start(false);
    interrupts();
    uint64_t command_end = fake_bus_now;
    TEST_ASSERT_TRUE(fake_bus_armed);
    TEST_ASSERT_TRUE(fake_bus_alarm_at >= command_end + 9040u);
    run_to(fake_bus_alarm_at - 1u);
    TEST_ASSERT_EQUAL(0, fake_bus_reads);
    ms5607_conversion_t c;
    TEST_ASSERT_FALSE(ms5607_async_take(&c));
}

/* One conversion at a time: a start while one is in flight sends nothing. */
void test_ms5607_busy_until_read(void) {
    ms5607_async_start(false);
    interrupts();
    TEST_ASSERT_EQUAL(MS5607_BUSY, ms5607_async_start(false));
    interrupts();
    TEST_ASSERT_EQUAL(1, fake_bus_ncmds);
    run_to(1020000u);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start(false));
    interrupts();
    TEST_ASSERT_EQUAL(2, fake_bus_ncmds);
}

void test_ms5607_temperature(void) {
    ms5607_async_start(true);
    run_to(1020000u);
    TEST_ASSERT_EQUAL_HEX8(CONV_D2, fake_bus_cmds[0]);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_TRUE(c.temperature);
}

/* A command the sensor does not answer ends the conversion as failed, arms
 * nothing, and leaves the next start free. */
void test_ms5607_command_nack(void) {
    fake_bus_nack = true;
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start(false));
    interrupts();
    TEST_ASSERT_FALSE(fake_bus_armed);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(c.ok);
    fake_bus_nack = false;
    TEST_ASSERT_EQUAL(MS5607_STARTED, ms5607_async_start(false));
    run_to(1020000u);
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_TRUE(c.ok);
}

void test_ms5607_read_nack(void) {
    ms5607_async_start(false);
    interrupts();
    fake_bus_nack = true;
    run_to(1020000u);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(c.ok);
}

void test_ms5607_taken_once(void) {
    ms5607_async_start(false);
    run_to(1020000u);
    ms5607_conversion_t c;
    TEST_ASSERT_TRUE(ms5607_async_take(&c));
    TEST_ASSERT_FALSE(ms5607_async_take(&c));
}

/* Started at the top of a loop, a conversion is ready with half a millisecond
 * to spare before the next top, which is never sooner than a period on. The
 * spare is for the interrupt latency and the work ahead of the pressure task
 * at the top of the loop; a conversion not ready is a loop without a sample. */
void test_ms5607_ready_before_the_next_loop(void) {
    ms5607_async_start(false);
    run_to(1000000u + MS5607_CONV_MS * 1000u);
    TEST_ASSERT_EQUAL(1, fake_bus_reads);
    uint64_t ready = fake_bus_alarm_at + FAKE_BUS_READ_US;
    char msg[80];
    snprintf(msg, sizeof(msg), "ready %llu us into a %u us loop", (unsigned long long)(ready - 1000000u),
             MS5607_CONV_MS * 1000u);
    TEST_ASSERT_TRUE_MESSAGE(ready + 500u <= 1000000u + MS5607_CONV_MS * 1000u, msg);
}

/* Primes the temperature line the way a loop does, so what follows is
 * pressure. */
static void prime_temperature(ms5607_temps_t *t) {
    ms5607_conversion_t c;
    ms5607_start_t started;
    *t = (ms5607_temps_t){0};
    fake_bus_adc = 8077636u;
    ms5607_async_cycle(t, &c, &started);
    run_to(fake_bus_now + 10000u);
    fake_bus_adc = 6465444u;
    ms5607_async_cycle(t, &c, &started);
    TEST_ASSERT_EQUAL(1, t->n);
}

/* The next conversion is started before the one taken is handed back, so
 * whatever the caller does with it cannot delay the next command. */
void test_ms5607_cycle_starts_before_returning(void) {
    ms5607_temps_t t;
    prime_temperature(&t);
    interrupts();
    run_to(fake_bus_now + 10000u);
    ms5607_conversion_t c;
    ms5607_start_t started;
    TEST_ASSERT_TRUE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_FALSE(c.temperature);
    TEST_ASSERT_EQUAL(MS5607_STARTED, started);
    TEST_ASSERT_TRUE(fake_bus_forced);
}

/* Measured on an MK1B: a pressure's compensation, filter and fit take up to
 * 2.7 ms of the loop. Started after that work, the next conversion missed the
 * next loop half the time: 47 % of loops waited, and the rate was 50 Hz. */
#define PRESSURE_WORK_US 2700u

void test_ms5607_work_costs_no_samples(void) {
    ms5607_temps_t t;
    prime_temperature(&t);
    uint64_t top = fake_bus_now + 10000u;
    run_to(top);
    int busy = 0, pressures = 0, temperatures = 0;
    for (int loop = 0; loop < 200; loop++) {
        ms5607_conversion_t c;
        ms5607_start_t started;
        bool took = ms5607_async_cycle(&t, &c, &started);
        interrupts();
        if (started == MS5607_BUSY)
            busy++;
        if (took && c.temperature)
            temperatures++;
        if (took && !c.temperature) {
            pressures++;
            fake_bus_now += PRESSURE_WORK_US;
        }
        top += MS5607_CONV_MS * 1000u;
        run_to(top);
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "%d of 200 loops waited; %d pressures, %d temperatures", busy, pressures,
             temperatures);
    TEST_ASSERT_EQUAL_MESSAGE(0, busy, msg);
    TEST_ASSERT_INT_WITHIN_MESSAGE(2, 180, pressures, msg);
    TEST_ASSERT_INT_WITHIN_MESSAGE(2, 20, temperatures, msg);
}

/* A temperature goes onto the line inside the cycle, so the choice of the
 * next conversion sees it. */
void test_ms5607_cycle_notes_the_temperature(void) {
    ms5607_temps_t t = {0};
    ms5607_conversion_t c;
    ms5607_start_t started;
    TEST_ASSERT_FALSE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_EQUAL(MS5607_STARTED, started);
    run_to(1020000u);
    TEST_ASSERT_EQUAL_HEX8(CONV_D2, fake_bus_cmds[0]);
    TEST_ASSERT_TRUE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_TRUE(c.temperature);
    TEST_ASSERT_EQUAL(1, t.n);
    interrupts();
    TEST_ASSERT_EQUAL_HEX8(CONV_D1, fake_bus_cmds[1]);
}

/* A read the sensor did not answer starts nothing: the caller backs off
 * rather than retrying every loop against a missing sensor. */
void test_ms5607_cycle_holds_off_after_a_failed_read(void) {
    ms5607_temps_t t = {0};
    ms5607_conversion_t c;
    ms5607_start_t started;
    ms5607_async_cycle(&t, &c, &started);
    interrupts();
    fake_bus_nack = true;
    run_to(1020000u);
    TEST_ASSERT_TRUE(ms5607_async_cycle(&t, &c, &started));
    TEST_ASSERT_FALSE(c.ok);
    TEST_ASSERT_EQUAL(MS5607_HELD, started);
    TEST_ASSERT_FALSE(fake_bus_forced);
}

void test_ms5607_begin_addresses_the_sensor(void) {
    TEST_ASSERT_EQUAL_HEX8(0x77, fake_bus_address);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_PRES_08_stamp_is_the_conversions);
    RUN_TEST(test_SNS_PRES_08_held_read_keeps_its_stamp);
    RUN_TEST(test_SNS_PRES_08_late_loop_keeps_the_stamp);
    RUN_TEST(test_SNS_PRES_05_read_after_worst_case);
    RUN_TEST(test_ms5607_busy_until_read);
    RUN_TEST(test_ms5607_temperature);
    RUN_TEST(test_ms5607_command_nack);
    RUN_TEST(test_ms5607_read_nack);
    RUN_TEST(test_ms5607_taken_once);
    RUN_TEST(test_ms5607_ready_before_the_next_loop);
    RUN_TEST(test_ms5607_cycle_starts_before_returning);
    RUN_TEST(test_ms5607_work_costs_no_samples);
    RUN_TEST(test_ms5607_cycle_notes_the_temperature);
    RUN_TEST(test_ms5607_cycle_holds_off_after_a_failed_read);
    RUN_TEST(test_ms5607_begin_addresses_the_sensor);
    return UNITY_END();
}
