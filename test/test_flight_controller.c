#include "unity.h"

void setUp(void) {
    // Set up test fixtures
}

void tearDown(void) {
    // Clean up after tests
}

void test_example(void) {
    TEST_ASSERT_EQUAL(1, 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_example);
    return UNITY_END();
}
