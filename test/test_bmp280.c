/*
 * The BMP280's detection and arithmetic, on a fake part
 * (docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf). Its conversions are the
 * collector's (test_collector.c). Built against test/fake_sdk with MK1A's
 * pins.
 *
 * Verifies [SYS-ALT-02].
 */
#include "unity.h"
#include "board_pins.h"
#include "bmp280_driver.h"
#include "fake_i2c.h"
#include <stdio.h>
#include <string.h>

void hal_telemetry_send(const char *sentence) {
    (void)sentence;
}

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
static const int16_t CALIB[12] = {27504, 26435, -1000, (int16_t)36477, -10685, 3024,
                                  2855,  140,   -7,    15500,          -14600, 6000};

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

/* Detection leaves the part asleep: the collector, not the part's own timer,
 * starts every conversion. */
void test_bmp280_detect_leaves_it_asleep(void) {
    TEST_ASSERT_EQUAL_HEX8(0x2C, bmp.regs[0xF4]); /* x1 temperature, x4 pressure, sleep */
    TEST_ASSERT_EQUAL_HEX8(0x00, bmp.regs[0xF5]); /* no filter */
    TEST_ASSERT_FALSE(bmp.running);
    TEST_ASSERT_EQUAL(0, bmp.commands);
    TEST_ASSERT_EQUAL_HEX8(0x77, bmp280_address());
}

/* Section 3.12's worked example (page 23): these codes, with this
 * calibration, are 100653.27 Pa and 25.08 C. */
void test_bmp280_the_datasheets_example(void) {
    bmp280_reading_t r;
    TEST_ASSERT_TRUE(bmp280_compensate(&bmp.regs[0xF7], &r));
    TEST_ASSERT_EQUAL_UINT32(ADC_P0, r.adc_p);
    TEST_ASSERT_EQUAL_UINT32(ADC_T0, r.adc_t);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 100653.27f, r.reading.pressure_pa);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 25.08f, r.reading.temperature_c);
}

/* Every code the ADC has gives a pressure, and a lower pressure than the
 * code before it: nothing in the arithmetic bends or wraps. */
void test_bmp280_every_code_compensates_in_order(void) {
    float previous = 1e9f;
    for (uint32_t adc_p = 0; adc_p < (1u << 20); adc_p += 256u) {
        const uint8_t data[BMP280_DATA_BYTES] = {(uint8_t)(adc_p >> 12), (uint8_t)(adc_p >> 4),
                                                 (uint8_t)(adc_p << 4),  (uint8_t)(ADC_T0 >> 12),
                                                 (uint8_t)(ADC_T0 >> 4), (uint8_t)(ADC_T0 << 4)};
        bmp280_reading_t r;
        TEST_ASSERT_TRUE(bmp280_compensate(data, &r));
        TEST_ASSERT_TRUE(r.reading.pressure_pa < previous);
        previous = r.reading.pressure_pa;
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bmp280_detect_leaves_it_asleep);
    RUN_TEST(test_bmp280_the_datasheets_example);
    RUN_TEST(test_bmp280_every_code_compensates_in_order);
    return UNITY_END();
}
