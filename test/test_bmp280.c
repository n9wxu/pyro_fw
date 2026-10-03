/*
 * The BMP280 at the loop's rate [DD-067]: one forced conversion a loop,
 * commanded by the loop and taken at the next, on a fake part whose
 * conversions take the time docs/datasheets/BST-BMP280-DS001-26 Table 13
 * (page 18) gives them. Built against test/fake_sdk with MK1A's pins.
 *
 * Verifies [SYS-ALT-02, SNS-PRES-06].
 */
#include "../src/loop_period.h"
#include "unity.h"
#include "board_pins.h"
#include "bmp280_driver.h"
#include "fake_i2c.h"
#include <stdio.h>
#include <string.h>

void hal_telemetry_send(const char *sentence) {
    (void)sentence;
}

volatile uint32_t flash_op_seq;

#define SDA BOARD_PIN_I2C_SDA
#define SCL BOARD_PIN_I2C_SCL
#define BUS (BOARD_I2C_INST->index)

/* ── A fake BMP280 ──────────────────────────────────────────────────
 * Forced mode runs one conversion and sleeps; normal mode repeats one every
 * conversion plus the standby. Each conversion publishes codes of its own,
 * so a reading names the conversion it came from. */

#define ADC_P0 415148u
#define ADC_T0 519888u

static struct {
    uint8_t regs[256];
    uint8_t ptr;
    bool running;
    uint64_t end_us;
    uint32_t meas_us, standby_us;
    uint32_t conversions, commands;
    bool nack;
} bmp;

static fake_i2c_dev_t bmp_dev;

static uint64_t now_us(void) {
    return (uint64_t)fake_now_ms * 1000u;
}

static void publish(uint32_t n) {
    uint32_t p = ADC_P0 + n, t = ADC_T0 + n;
    bmp.regs[0xF7] = (uint8_t)(p >> 12);
    bmp.regs[0xF8] = (uint8_t)(p >> 4);
    bmp.regs[0xF9] = (uint8_t)(p << 4);
    bmp.regs[0xFA] = (uint8_t)(t >> 12);
    bmp.regs[0xFB] = (uint8_t)(t >> 4);
    bmp.regs[0xFC] = (uint8_t)(t << 4);
}

static void bmp_run(void) {
    while (bmp.running && now_us() >= bmp.end_us) {
        publish(++bmp.conversions);
        if ((bmp.regs[0xF4] & 3u) == 3u) {
            bmp.end_us += bmp.standby_us + bmp.meas_us;
        } else {
            bmp.running = false;
            bmp.regs[0xF4] &= (uint8_t)~3u;
        }
    }
    bmp.regs[0xF3] = bmp.running ? 0x08 : 0x00;
}

static int bmp_write(fake_i2c_dev_t *d, const uint8_t *src, size_t len) {
    (void)d;
    if (bmp.nack)
        return PICO_ERROR_GENERIC;
    bmp_run();
    bmp.ptr = src[0];
    if (len >= 2) {
        bmp.regs[bmp.ptr] = src[1];
        if (bmp.ptr == 0xF4 && (src[1] & 3u) != 0) {
            bmp.commands++;
            bmp.running = true;
            bmp.end_us = now_us() + bmp.meas_us;
        }
    }
    return (int)len;
}

static int bmp_read(fake_i2c_dev_t *d, uint8_t *dst, size_t len) {
    (void)d;
    if (bmp.nack)
        return PICO_ERROR_GENERIC;
    bmp_run();
    for (size_t i = 0; i < len; i++)
        dst[i] = bmp.regs[(uint8_t)(bmp.ptr + i)];
    return (int)len;
}

/* A part's calibration, little-endian from 0x88. */
static const int16_t CALIB[12] = {27504, 26435, -1000, (int16_t)36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000};

void setUp(void) {
    fake_now_ms = 1000;
    memset(&bmp, 0, sizeof(bmp));
    bmp.regs[0xD0] = 0x58;
    for (int i = 0; i < 12; i++) {
        bmp.regs[0x88 + 2 * i] = (uint8_t)((uint16_t)CALIB[i] & 0xFF);
        bmp.regs[0x89 + 2 * i] = (uint8_t)((uint16_t)CALIB[i] >> 8);
    }
    publish(0);
    bmp.meas_us = 11500u; /* typical, x4 pressure and x1 temperature */
    bmp.standby_us = 500u;
    fake_i2c_reset();
    i2c_init(BOARD_I2C_INST, BOARD_BMP280_I2C_HZ);
    gpio_set_function(SDA, GPIO_FUNC_I2C);
    gpio_set_function(SCL, GPIO_FUNC_I2C);
    bmp_dev = (fake_i2c_dev_t){.bus = BUS, .addr = 0x77, .sda = SDA, .scl = SCL, .write = bmp_write, .read = bmp_read};
    fake_i2c_attach(&bmp_dev);
    TEST_ASSERT_TRUE(bmp280_detect());
}

void tearDown(void) {}

static uint32_t code_p(const bmp280_reading_t *r) {
    return r->adc_p;
}

/* Detection leaves the part asleep with the loop's settings: the loop, not
 * the part's own timer, starts every conversion. */
void test_bmp280_detect_leaves_it_asleep(void) {
    TEST_ASSERT_EQUAL_HEX8(0x2C, bmp.regs[0xF4]); /* x1 temperature, x4 pressure, sleep */
    TEST_ASSERT_EQUAL_HEX8(0x00, bmp.regs[0xF5]); /* no filter */
    TEST_ASSERT_FALSE(bmp.running);
    TEST_ASSERT_EQUAL(0, bmp.commands);
}

/* Each loop takes the conversion the last commanded and commands the next:
 * a reading every loop, each from a conversion of its own. */
void test_bmp280_one_conversion_a_loop(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    int took = 0, repeats = 0, busy = 0;
    uint32_t last = 0;
    for (int loop = 0; loop < 500; loop++) {
        if (bmp280_cycle(&r, &started)) {
            repeats += took > 0 && code_p(&r) == last;
            last = code_p(&r);
            took++;
        }
        busy += started == BMP280_BUSY;
        fake_now_ms += LOOP_PERIOD_MS;
    }
    TEST_ASSERT_EQUAL(499, took);
    TEST_ASSERT_EQUAL(0, repeats);
    TEST_ASSERT_EQUAL(0, busy);
    TEST_ASSERT_EQUAL_UINT32(500, bmp.commands);
}

/* [SNS-PRES-08] Stamped at the middle of the pressure's own measurement,
 * from the command that started it: a loop that takes it late moves nothing. */
void test_bmp280_stamp_is_the_conversions(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_STARTED, started);
    uint64_t commanded = now_us();
    fake_now_ms += 3u * LOOP_PERIOD_MS + 7u;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL_UINT64(commanded + BMP280_P_MID_US, r.reading.time_us);
    TEST_ASSERT_EQUAL_UINT32(ADC_P0 + 1u, r.adc_p);
    TEST_ASSERT_EQUAL_UINT32(ADC_T0 + 1u, r.adc_t);
    TEST_ASSERT_TRUE(r.reading.pressure_pa > 30000.0f && r.reading.pressure_pa < 110000.0f);
}

/* At the datasheet's maximum, 13.3 ms, every conversion is done by the next
 * loop, and the part's own timer never runs. */
void test_bmp280_fresh_at_the_worst_case(void) {
    bmp.meas_us = BMP280_MEAS_MAX_US;
    bmp280_reading_t r;
    bmp280_start_t started;
    int took = 0, busy = 0;
    for (int loop = 0; loop < 100; loop++) {
        took += bmp280_cycle(&r, &started);
        busy += started == BMP280_BUSY;
        fake_now_ms += LOOP_PERIOD_MS;
    }
    TEST_ASSERT_EQUAL(99, took);
    TEST_ASSERT_EQUAL(0, busy);
    TEST_ASSERT_EQUAL_UINT32(bmp.commands, bmp.conversions + (bmp.running ? 1u : 0u));
}

/* Still measuring: nothing is taken and nothing commanded, so a conversion
 * is never cut short; the next loop takes it. */
void test_bmp280_busy_takes_and_commands_nothing(void) {
    bmp.meas_us = LOOP_PERIOD_US + 5000u;
    bmp280_reading_t r;
    bmp280_start_t started;
    bmp280_cycle(&r, &started);
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_BUSY, started);
    TEST_ASSERT_EQUAL_UINT32(1, bmp.commands);
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL_UINT32(ADC_P0 + 1u, r.adc_p);
    TEST_ASSERT_EQUAL(BMP280_STARTED, started);
}

/* A read the part does not answer takes nothing and commands nothing; the
 * conversion is still there when it answers again. */
void test_bmp280_bus_error_keeps_the_conversion(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    bmp280_cycle(&r, &started);
    fake_now_ms += LOOP_PERIOD_MS;
    bmp.nack = true;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_BUS, started);
    bmp.nack = false;
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL_UINT32(ADC_P0 + 1u, r.adc_p);
    TEST_ASSERT_EQUAL_UINT32(2, bmp.commands);
}

/* A command the part does not take leaves nothing to read: the next loop
 * commands again rather than reading the old codes as new. */
void test_bmp280_refused_command_reads_nothing(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    bmp.nack = true;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_BUS, started);
    bmp.nack = false;
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_STARTED, started);
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL_UINT32(ADC_P0 + 1u, r.adc_p);
}

/* [DD-068] A flash erase or program during the conversion marks it; the
 * next, undisturbed, is not. */
void test_bmp280_flash_during_the_conversion_marks_it(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    bmp280_cycle(&r, &started);
    flash_op_seq += 2u;
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_TRUE(r.flashed);
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
    TEST_ASSERT_FALSE(r.flashed);
}

/* A part holding SCL low never finishes a transfer. The SDK's blocking
 * calls wait on it forever, which in flight is a lockup until the watchdog
 * resets the board cold; each transfer here is bounded. */
void test_bmp280_a_held_bus_costs_a_bounded_wait(void) {
    bmp280_reading_t r;
    bmp280_start_t started;
    bmp280_cycle(&r, &started);
    fake_now_ms += LOOP_PERIOD_MS;
    bmp_dev.held = true;
    fake_slept = NULL;
    TEST_ASSERT_FALSE(bmp280_cycle(&r, &started));
    TEST_ASSERT_EQUAL(BMP280_BUS, started);
    TEST_ASSERT_NULL_MESSAGE(fake_slept, "an unbounded wait");
    bmp_dev.held = false;
    fake_now_ms += LOOP_PERIOD_MS;
    TEST_ASSERT_TRUE(bmp280_cycle(&r, &started));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bmp280_detect_leaves_it_asleep);
    RUN_TEST(test_bmp280_one_conversion_a_loop);
    RUN_TEST(test_bmp280_stamp_is_the_conversions);
    RUN_TEST(test_bmp280_fresh_at_the_worst_case);
    RUN_TEST(test_bmp280_busy_takes_and_commands_nothing);
    RUN_TEST(test_bmp280_bus_error_keeps_the_conversion);
    RUN_TEST(test_bmp280_refused_command_reads_nothing);
    RUN_TEST(test_bmp280_flash_during_the_conversion_marks_it);
    RUN_TEST(test_bmp280_a_held_bus_costs_a_bounded_wait);
    return UNITY_END();
}
