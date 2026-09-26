/*
 * A board's pressure sensor bring-up, boards/<name>/pressure_board.c, run on
 * the host against test/fake_sdk and fake sensors. Built once per board
 * (sensor_bringup_tests). The loop is the only clock (DD-053): bus recovery,
 * settles and sensor resets are steps a later loop takes, and the fake SDK's
 * sleeps fail the test.
 */
#include "unity.h"
#include "board_pins.h"
#include "fake_i2c.h"
#include "pressure_sensor.h"
#include <stdio.h>
#include <string.h>

void hal_telemetry_send(const char *sentence) {
    (void)sentence;
}

#if defined(BOARD_PIN_MS5607_SDA) /* MK1B: two SDA pads on one SCL */
#define MS_SDA BOARD_PIN_MS5607_SDA
#define BMP_SDA BOARD_PIN_BMP280_SDA
#define HAS_MS5607 1
#define HAS_BMP280 1
#elif defined(BOARD_HAS_BMP280) && BOARD_HAS_BMP280
#define BMP_SDA BOARD_PIN_I2C_SDA
#define HAS_BMP280 1
#else
#define MS_SDA BOARD_PIN_I2C_SDA
#define HAS_MS5607 1
#endif
#define SCL BOARD_PIN_I2C_SCL
#define BUS (BOARD_I2C_INST->index)

/* ── The bus recovery, watched from the pins ───────────────────────── */

static int scl_falls, stops[FAKE_PINS], transfers_before_recovery;

static void on_put(uint pin, bool value) {
    if (pin == SCL && !value && fake_level[SCL] && fake_func[SCL] != GPIO_FUNC_I2C)
        scl_falls++;
    /* A STOP: an SDA line released high while SCL is high. */
    if (pin != SCL && value && !fake_level[pin] && fake_level[SCL] && fake_output[pin])
        stops[pin]++;
}

static void note_transfer(uint8_t sda) {
    if (scl_falls < 9 || stops[sda] < 1)
        transfers_before_recovery++;
}

/* Transfers a sensor refused because they came during its reset. */
static uint64_t ms_reload_until, bmp_start_until;
static int ms_sel, ms_early, bmp_early;

static uint64_t now_us(void) {
    return (uint64_t)fake_now_ms * 1000u;
}

#ifdef HAS_MS5607
/* ── A fake MS5607: NACKs through the 2.8 ms reload after a reset ─── */

static const uint16_t MS_PROM[8] = {0x0042, 46372, 43981, 29059, 27842, 31553, 28165, 0x0005};

static int ms_write(fake_i2c_dev_t *d, const uint8_t *src, size_t len) {
    note_transfer(d->sda);
    if (now_us() < ms_reload_until) {
        ms_early++;
        return PICO_ERROR_GENERIC;
    }
    uint8_t cmd = src[0];
    if (cmd == 0x1E)
        ms_reload_until = now_us() + 2800u;
    else if (cmd >= 0xA0 && cmd <= 0xAE)
        ms_sel = (cmd - 0xA0) / 2;
    else if (cmd == 0x00)
        ms_sel = -1;
    return (int)len;
}

static int ms_read(fake_i2c_dev_t *d, uint8_t *dst, size_t len) {
    note_transfer(d->sda);
    if (now_us() < ms_reload_until) {
        ms_early++;
        return PICO_ERROR_GENERIC;
    }
    if (ms_sel >= 0 && len == 2) {
        dst[0] = (uint8_t)(MS_PROM[ms_sel] >> 8);
        dst[1] = (uint8_t)MS_PROM[ms_sel];
    } else {
        memset(dst, 0, len);
    }
    return (int)len;
}

#endif

#ifdef HAS_BMP280
/* ── A fake BMP280: NACKs through the 2 ms start-up after a reset ──── */

static uint8_t bmp_regs[256];
static uint8_t bmp_ptr;

static int bmp_write(fake_i2c_dev_t *d, const uint8_t *src, size_t len) {
    note_transfer(d->sda);
    if (now_us() < bmp_start_until) {
        bmp_early++;
        return PICO_ERROR_GENERIC;
    }
    bmp_ptr = src[0];
    if (len >= 2) {
        bmp_regs[bmp_ptr] = src[1];
        if (bmp_ptr == 0xE0 && src[1] == 0xB6)
            bmp_start_until = now_us() + 2000u;
    }
    return (int)len;
}

static int bmp_read(fake_i2c_dev_t *d, uint8_t *dst, size_t len) {
    note_transfer(d->sda);
    if (now_us() < bmp_start_until) {
        bmp_early++;
        return PICO_ERROR_GENERIC;
    }
    for (size_t i = 0; i < len; i++)
        dst[i] = bmp_regs[(uint8_t)(bmp_ptr + i)];
    return (int)len;
}

#endif

#ifdef HAS_MS5607
static fake_i2c_dev_t ms_dev;

static void attach_ms5607(void) {
    ms_dev = (fake_i2c_dev_t){.bus = BUS, .addr = 0x77, .sda = MS_SDA, .scl = SCL, .write = ms_write, .read = ms_read};
    fake_i2c_attach(&ms_dev);
}
#endif

#ifdef HAS_BMP280
static fake_i2c_dev_t bmp_dev;

static void attach_bmp280(void) {
    memset(bmp_regs, 0x11, sizeof(bmp_regs));
    bmp_regs[0xD0] = 0x58;
    bmp_dev = (fake_i2c_dev_t){.bus = BUS, .addr = 0x77, .sda = BMP_SDA, .scl = SCL, .write = bmp_write, .read = bmp_read};
    fake_i2c_attach(&bmp_dev);
}
#endif

static pressure_sensor_type_t bring_up(int *loops) {
    pressure_sensor_begin();
    TEST_ASSERT_NULL_MESSAGE(fake_slept, fake_slept);
    pressure_sensor_type_t t = PRESSURE_SENSOR_PENDING;
    int n = 0;
    while (t == PRESSURE_SENSOR_PENDING && n < 200) {
        fake_now_ms += 10u;
        n++;
        t = pressure_sensor_step(fake_now_ms);
        TEST_ASSERT_NULL_MESSAGE(fake_slept, fake_slept);
    }
    if (loops)
        *loops = n;
    return t;
}

void setUp(void) {
    memset(fake_level, 0, sizeof(fake_level));
    memset(fake_output, 0, sizeof(fake_output));
    memset(fake_func, 0, sizeof(fake_func));
    fake_on_put = on_put;
    fake_slept = NULL;
    fake_now_ms = 5000u;
    fake_i2c_reset();
    scl_falls = transfers_before_recovery = 0;
    memset(stops, 0, sizeof(stops));
    ms_reload_until = bmp_start_until = 0;
    ms_early = bmp_early = 0;
    ms_sel = 0;
}

void tearDown(void) {}

#ifdef HAS_MS5607
#define FITTED PRESSURE_SENSOR_MS5607
#define FITTED_NAME "MS5607"
#define FITTED_SDA MS_SDA
static void attach_fitted(void) {
    attach_ms5607();
}
#else
#define FITTED PRESSURE_SENSOR_BMP280
#define FITTED_NAME "BMP280"
#define FITTED_SDA BMP_SDA
static void attach_fitted(void) {
    attach_bmp280();
}
#endif

/* Found within half a second of loops, well inside BOOT_SETTLE's 2.5 s. */
void test_bringup_finds_its_sensor(void) {
    attach_fitted();
    int loops;
    TEST_ASSERT_EQUAL(FITTED, bring_up(&loops));
    char msg[48];
    snprintf(msg, sizeof(msg), "%d loops", loops);
    TEST_ASSERT_TRUE_MESSAGE(loops <= 50, msg);
    TEST_ASSERT_EQUAL_STRING(FITTED_NAME, pressure_sensor_name());
}

/* Nine clocks and a STOP on the sensor's own SDA free a sensor a CPU reset
 * left mid-transfer, before the peripheral's first transfer. */
void test_bringup_recovers_the_bus_first(void) {
    attach_fitted();
    bring_up(NULL);
    TEST_ASSERT_TRUE(scl_falls >= 9);
    TEST_ASSERT_TRUE(stops[FITTED_SDA] >= 1);
    TEST_ASSERT_EQUAL(0, transfers_before_recovery);
}

/* A sensor is not spoken to during its reset: the MS5607's 2.8 ms reload and
 * the BMP280's 2 ms start-up are deadlines a later loop meets. */
void test_bringup_waits_out_the_reset(void) {
    attach_fitted();
    TEST_ASSERT_EQUAL(FITTED, bring_up(NULL));
    TEST_ASSERT_EQUAL(0, ms_early);
    TEST_ASSERT_EQUAL(0, bmp_early);
}

void test_bringup_at_the_boards_speed(void) {
    attach_fitted();
    bring_up(NULL);
#ifdef HAS_MS5607
    TEST_ASSERT_EQUAL_UINT(BOARD_MS5607_I2C_HZ, ms_dev.hz_last);
#else
    TEST_ASSERT_EQUAL_UINT(BOARD_BMP280_I2C_HZ, bmp_dev.hz_last);
#endif
}

void test_bringup_without_a_sensor(void) {
    int loops;
    TEST_ASSERT_EQUAL(PRESSURE_SENSOR_NONE, bring_up(&loops));
    TEST_ASSERT_TRUE(loops <= 60);
}

#if defined(HAS_MS5607) && defined(HAS_BMP280)
/* MK1B: the BMP280 pad is probed first, in standard mode, and a BMP280 there
 * is the sensor. */
void test_bringup_mk1b_bmp280(void) {
    attach_bmp280();
    TEST_ASSERT_EQUAL(PRESSURE_SENSOR_BMP280, bring_up(NULL));
    TEST_ASSERT_EQUAL_UINT(BOARD_BMP280_I2C_HZ, bmp_dev.hz_last);
    TEST_ASSERT_EQUAL(0, bmp_early);
}

/* MK1B with its MS5607: the BMP280 pad is let go before the MS5607's. */
void test_bringup_mk1b_releases_the_bmp280_pad(void) {
    attach_ms5607();
    TEST_ASSERT_EQUAL(PRESSURE_SENSOR_MS5607, bring_up(NULL));
    TEST_ASSERT_NOT_EQUAL(GPIO_FUNC_I2C, fake_func[BMP_SDA]);
}
#endif

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bringup_finds_its_sensor);
    RUN_TEST(test_bringup_recovers_the_bus_first);
    RUN_TEST(test_bringup_waits_out_the_reset);
    RUN_TEST(test_bringup_at_the_boards_speed);
    RUN_TEST(test_bringup_without_a_sensor);
#if defined(HAS_MS5607) && defined(HAS_BMP280)
    RUN_TEST(test_bringup_mk1b_bmp280);
    RUN_TEST(test_bringup_mk1b_releases_the_bmp280_pad);
#endif
    return UNITY_END();
}
