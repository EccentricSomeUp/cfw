/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <string.h>

static int test_random_bytes(void)
{
    unsigned char buffer[64];
    unsigned char again[64];
    size_t i;
    int all_zero = 1;
    int all_same = 1;

    memset(buffer, 0, sizeof(buffer));
    CFW_TEST_ASSERT_EQ(cfw_random_bytes(buffer, sizeof(buffer)), CFW_OK);
    for (i = 0; i < sizeof(buffer); i++) {
        if (buffer[i] != 0) {
            all_zero = 0;
        }
        if (i > 0 && buffer[i] != buffer[0]) {
            all_same = 0;
        }
    }
    CFW_TEST_ASSERT_FALSE(all_zero);
    CFW_TEST_ASSERT_FALSE(all_same);

    /* Independent draws must differ with overwhelming probability. */
    CFW_TEST_ASSERT_EQ(cfw_random_bytes(again, sizeof(again)), CFW_OK);
    CFW_TEST_ASSERT(memcmp(buffer, again, sizeof(buffer)) != 0);

    CFW_TEST_ASSERT_EQ(cfw_random_bytes(NULL, 1), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_random_bytes(buffer, 0), CFW_OK);
    return 0;
}

static int test_random_u64(void)
{
    uint64_t a;
    uint64_t b;

    CFW_TEST_ASSERT_EQ(cfw_random_u64(NULL), CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_random_u64(&a), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_random_u64(&b), CFW_OK);
    CFW_TEST_ASSERT(a != b);
    return 0;
}

static int test_random_below(void)
{
    uint64_t value;
    uint64_t i;
    int ones = 0;

    CFW_TEST_ASSERT_EQ(cfw_random_below(0, &value),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_random_below(1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    /* Bound of 1 must always produce 0. */
    for (i = 0; i < 100; i++) {
        CFW_TEST_ASSERT_EQ(cfw_random_below(1, &value), CFW_OK);
        CFW_TEST_ASSERT_EQ(value, 0);
    }

    /* Values must stay strictly below the bound. */
    for (i = 0; i < 1000; i++) {
        CFW_TEST_ASSERT_EQ(cfw_random_below(7, &value), CFW_OK);
        CFW_TEST_ASSERT(value < 7);
    }
    CFW_TEST_ASSERT_EQ(cfw_random_below(UINT64_MAX, &value), CFW_OK);

    /* Loose distribution sanity check for bound 2. */
    for (i = 0; i < 10000; i++) {
        CFW_TEST_ASSERT_EQ(cfw_random_below(2, &value), CFW_OK);
        ones += (int)value;
    }
    CFW_TEST_ASSERT(ones > 4000 && ones < 6000);
    return 0;
}

const cfw_test_case cfw_test_random_cases[] = {
    {"random_bytes", test_random_bytes},
    {"random_u64", test_random_u64},
    {"random_below", test_random_below},
    {NULL, NULL},
};
