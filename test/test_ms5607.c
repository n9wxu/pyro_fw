/*
 * The MS5607's arithmetic, which runs in the sensor task on the codes the
 * collector hands it: the compensation, the temperature carried to each
 * pressure's own time, and the PROM's CRC.
 *
 * Figures are from docs/datasheets/MS5607-02BA03_2017-06.pdf and
 * docs/datasheets/AN520_C-code_MS56xx_004_2011-08.pdf.
 *
 * Verifies [SNS-PRES-12, SNS-PRES-15, SNS-PRES-16].
 */
#include "unity.h"
#include "ms5607_driver.h"

void setUp(void) {}
void tearDown(void) {}

/* The datasheet's example part, page 8. */
static const uint16_t PROM[8] = {0, 46372, 43981, 29059, 27842, 31553, 28165, 0};

static pressure_reading_t compensated(uint32_t d1, uint32_t d2) {
    pressure_reading_t r;
    ms5607_compensate_prom(PROM, d1, d2, &r);
    return r;
}

/* The temperature code this part gives at a temperature, by the datasheet's
 * own first-order line. */
static uint32_t d2_at(float temp_c) {
    return (uint32_t)(((int32_t)PROM[5] << 8) + (int32_t)((temp_c * 100.0f - 2000.0f) * 8388608.0f / (float)PROM[6]));
}

/* The pressure code that reads pa at this temperature code. */
static uint32_t d1_reading(float pa, uint32_t d2) {
    uint32_t lo = 0, hi = 1u << 24;
    while (hi - lo > 1u) {
        uint32_t mid = (lo + hi) / 2u;
        if (compensated(mid, d2).pressure_pa < pa)
            lo = mid;
        else
            hi = mid;
    }
    return hi;
}

/* Page 8's worked example: 1100.02 mbar at 20.00 C. */
void test_SNS_PRES_15_the_datasheets_example(void) {
    pressure_reading_t r = compensated(6465444u, 8077636u);
    TEST_ASSERT_EQUAL_FLOAT(110002.0f, r.pressure_pa);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, r.temperature_c);
}

/* Page 9: at and above 20 C the second order adds nothing. */
void test_SNS_PRES_15_nothing_changes_at_20_C_and_above(void) {
    const float warm[] = {20.0f, 45.0f, 85.0f};
    for (int i = 0; i < 3; i++) {
        uint32_t d2 = d2_at(warm[i]);
        int32_t dT = (int32_t)d2 - ((int32_t)PROM[5] << 8);
        int64_t off = ((int64_t)PROM[2] << 17) + (((int64_t)PROM[4] * dT) >> 6);
        int64_t sens = ((int64_t)PROM[1] << 16) + (((int64_t)PROM[3] * dT) >> 7);
        int32_t first_order = (int32_t)((((int64_t)6465444 * sens >> 21) - off) >> 15);
        TEST_ASSERT_EQUAL_FLOAT((float)first_order, compensated(6465444u, d2).pressure_pa);
    }
}

/* Page 9's low-temperature terms, worked by hand for this part at 0 C:
 * TEMP - 2000 = -2000, so OFF2 = 61 * 2000^2 / 16 = 15 250 000 and
 * SENS2 = 2 * 2000^2 = 8 000 000. At the code that read 1013.25 hPa before
 * them, (D1 * SENS2 / 2^21 - OFF2) / 2^15 is 277 Pa. */
void test_SNS_PRES_15_the_second_order_below_20_C(void) {
    uint32_t d2 = d2_at(0.0f);
    int32_t dT = (int32_t)d2 - ((int32_t)PROM[5] << 8);
    int64_t off = ((int64_t)PROM[2] << 17) + (((int64_t)PROM[4] * dT) >> 6);
    int64_t sens = ((int64_t)PROM[1] << 16) + (((int64_t)PROM[3] * dT) >> 7);
    uint32_t d1 = 0;
    for (uint32_t lo = 0, hi = 1u << 24; hi - lo > 1u;) {
        uint32_t mid = (lo + hi) / 2u;
        int32_t first_order = (int32_t)((((int64_t)mid * sens >> 21) - off) >> 15);
        if (first_order < 101325)
            lo = mid;
        else
            hi = d1 = mid;
    }
    pressure_reading_t r = compensated(d1, d2);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 101325.0f - 277.0f, r.pressure_pa);
    /* T2 = dT^2 / 2^31, and dT is -595 680 at this part's 0 C: 1.65 C. */
    TEST_ASSERT_FLOAT_WITHIN(0.02f, -1.65f, r.temperature_c);
}

/* Below -15 C the very-low terms add to them: at -40 C, 4.7 kPa at sea
 * level's pressure. */
void test_SNS_PRES_15_the_very_low_terms_below_minus_15_C(void) {
    uint32_t d2 = d2_at(-40.0f);
    uint32_t d1 = d1_reading(101325.0f, d2);
    int32_t dT = (int32_t)d2 - ((int32_t)PROM[5] << 8);
    int64_t off = ((int64_t)PROM[2] << 17) + (((int64_t)PROM[4] * dT) >> 6);
    int64_t sens = ((int64_t)PROM[1] << 16) + (((int64_t)PROM[3] * dT) >> 7);
    int32_t first_order = (int32_t)((((int64_t)d1 * sens >> 21) - off) >> 15);
    int64_t low = -6000, very_low = -2500;
    int64_t off2 = ((61 * low * low) >> 4) + 15 * very_low * very_low;
    int64_t sens2 = 2 * low * low + 8 * very_low * very_low;
    int32_t second_order = (int32_t)((((int64_t)d1 * (sens - sens2) >> 21) - (off - off2)) >> 15);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 101325.0f, compensated(d1, d2).pressure_pa);
    TEST_ASSERT_INT32_WITHIN(60, second_order, 101325);
    TEST_ASSERT_TRUE_MESSAGE(first_order - second_order > 4000, "about 4.7 kPa");
}

/* The line is straight in the pressure code, at any temperature, across
 * every code the ADC has. */
void test_SNS_PRES_15_a_straight_line_in_the_pressure_code(void) {
    const float temps[] = {-40.0f, 0.0f, 20.0f, 85.0f};
    for (int i = 0; i < 4; i++) {
        uint32_t d2 = d2_at(temps[i]);
        float previous = compensated(0u, d2).pressure_pa;
        float step = compensated(4096u, d2).pressure_pa - previous;
        for (uint32_t d1 = 4096u; d1 < (1u << 24); d1 += 4096u) {
            float now = compensated(d1, d2).pressure_pa;
            TEST_ASSERT_FLOAT_WITHIN(1.0f, step, now - previous);
            previous = now;
        }
    }
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

/* AN520 page 12: "the resulting calculated CRC should be 0xB". */
void test_SNS_PRES_16_the_application_notes_example(void) {
    uint16_t prom[8] = {0x3132, 0x3334, 0x3536, 0x3738, 0x3940, 0x4142, 0x4344, 0x4500};
    TEST_ASSERT_FALSE_MESSAGE(ms5607_prom_crc_ok(prom), "0 is not this PROM's CRC");
    prom[7] = 0x450B;
    TEST_ASSERT_TRUE(ms5607_prom_crc_ok(prom));
}

/* Bits 4 to 7 of the last word are outside the CRC: AN520 masks its whole
 * low byte. */
void test_SNS_PRES_16_one_wrong_bit_in_any_word_is_seen(void) {
    for (int word = 0; word < 8; word++) {
        for (int bit = 0; bit < 16; bit++) {
            if (word == 7 && bit >= 4 && bit < 8)
                continue;
            uint16_t prom[8] = {0x3132, 0x3334, 0x3536, 0x3738, 0x3940, 0x4142, 0x4344, 0x450B};
            prom[word] ^= (uint16_t)(1u << bit);
            TEST_ASSERT_FALSE(ms5607_prom_crc_ok(prom));
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_PRES_15_the_datasheets_example);
    RUN_TEST(test_SNS_PRES_15_nothing_changes_at_20_C_and_above);
    RUN_TEST(test_SNS_PRES_15_the_second_order_below_20_C);
    RUN_TEST(test_SNS_PRES_15_the_very_low_terms_below_minus_15_C);
    RUN_TEST(test_SNS_PRES_15_a_straight_line_in_the_pressure_code);
    RUN_TEST(test_SNS_PRES_12_the_line_interpolates_back);
    RUN_TEST(test_SNS_PRES_16_the_application_notes_example);
    RUN_TEST(test_SNS_PRES_16_one_wrong_bit_in_any_word_is_seen);
    return UNITY_END();
}
