/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stddef.h>
#include <stdint.h>

static int test_checked_add_success(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u32(7, 5, &u32_result), CFW_OK);
    CFW_TEST_ASSERT_EQ(u32_result, 12);

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u64(UINT64_C(7), UINT64_C(5),
                                           &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(u64_result, UINT64_C(12));

    CFW_TEST_ASSERT_EQ(cfw_checked_add_size((size_t)7, (size_t)5,
                                            &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(size_result, (size_t)12);
    return 0;
}

static int test_checked_sub_success(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u32(12, 5, &u32_result), CFW_OK);
    CFW_TEST_ASSERT_EQ(u32_result, 7);

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u64(UINT64_C(12), UINT64_C(5),
                                           &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(u64_result, UINT64_C(7));

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_size((size_t)12, (size_t)5,
                                            &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(size_result, (size_t)7);
    return 0;
}

static int test_checked_mul_success(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u32(12, 3, &u32_result), CFW_OK);
    CFW_TEST_ASSERT_EQ(u32_result, 36);

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u64(UINT64_C(12), UINT64_C(3),
                                           &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(u64_result, UINT64_C(36));

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_size((size_t)12, (size_t)3,
                                            &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(size_result, (size_t)36);
    return 0;
}

static int test_checked_add_boundaries(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u32(UINT32_MAX - 1, 1, &u32_result),
                       CFW_OK);
    CFW_TEST_ASSERT(u32_result == UINT32_MAX);

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u64(UINT64_MAX - 1, 1, &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT(u64_result == UINT64_MAX);

    CFW_TEST_ASSERT_EQ(cfw_checked_add_size(SIZE_MAX - 1, (size_t)1,
                                            &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT(size_result == SIZE_MAX);
    return 0;
}

static int test_checked_sub_boundaries(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u32(UINT32_MAX, UINT32_MAX, &u32_result),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(u32_result, 0);

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u64(UINT64_MAX, UINT64_MAX, &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT(u64_result == 0);

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_size(SIZE_MAX, SIZE_MAX, &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT(size_result == 0);
    return 0;
}

static int test_checked_mul_boundaries(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u32(UINT32_MAX, 1, &u32_result), CFW_OK);
    CFW_TEST_ASSERT(u32_result == UINT32_MAX);

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u64((UINT64_MAX / 2), 2, &u64_result),
                       CFW_OK);
    CFW_TEST_ASSERT(u64_result == UINT64_MAX - 1);

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_size(SIZE_MAX / 2, (size_t)2,
                                            &size_result),
                       CFW_OK);
    CFW_TEST_ASSERT(size_result == SIZE_MAX - 1);
    return 0;
}

static int test_checked_add_overflow(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u32(UINT32_MAX, 1, &u32_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_u64(UINT64_MAX, 1, &u64_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_size(SIZE_MAX, (size_t)1, &size_result),
                       CFW_ERROR_OVERFLOW);
    return 0;
}

static int test_checked_sub_underflow(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u32(0, 1, &u32_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u64(0, 1, &u64_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_size((size_t)0, (size_t)1, &size_result),
                       CFW_ERROR_OVERFLOW);
    return 0;
}

static int test_checked_mul_overflow(void)
{
    uint32_t u32_result = 0;
    uint64_t u64_result = 0;
    size_t size_result = 0;

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u32(UINT32_MAX, 2, &u32_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u64((UINT64_MAX / 2) + 1, 2,
                                           &u64_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_size(SIZE_MAX / 2 + 1, (size_t)2,
                                            &size_result),
                       CFW_ERROR_OVERFLOW);
    return 0;
}

static int test_checked_i64_success_and_boundaries(void)
{
    int64_t r;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(7, -3, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, 4);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_i64(-7, -3, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, -4);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(-6, 7, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, -42);

    /* Boundary values that must succeed. */
    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(INT64_MAX, 0, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, INT64_MAX);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(INT64_MIN, 0, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, INT64_MIN);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_i64(INT64_MIN, 0, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, INT64_MIN);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(INT64_MIN, 1, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, INT64_MIN);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(0, INT64_MIN, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, 0);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(-1, -1, &r), CFW_OK);
    CFW_TEST_ASSERT_EQ(r, 1);
    return 0;
}

static int test_checked_i64_overflow(void)
{
    int64_t r = 77;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(INT64_MAX, 1, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(INT64_MIN, -1, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_i64(INT64_MIN, 1, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_i64(INT64_MAX, -1, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(INT64_MAX, 2, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(INT64_MIN, -1, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(-1, INT64_MIN, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(INT64_MIN, 2, &r),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(r, 77);
    return 0;
}

static int test_checked_arithmetic_null_result(void)
{
    CFW_TEST_ASSERT_EQ(cfw_checked_add_i64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_i64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_i64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_u32(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u32(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u32(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_u64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_u64(1, 1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_add_size((size_t)1, (size_t)1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_sub_size((size_t)1, (size_t)1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_checked_mul_size((size_t)1, (size_t)1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_checked_arithmetic_preserves_result_on_error(void)
{
    uint32_t u32_result = 77;
    uint64_t u64_result = 77;
    size_t size_result = (size_t)77;

    CFW_TEST_ASSERT_EQ(cfw_checked_add_u32(UINT32_MAX, 1, &u32_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(u32_result, 77);

    CFW_TEST_ASSERT_EQ(cfw_checked_sub_u64(0, 1, &u64_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(u64_result, 77);

    CFW_TEST_ASSERT_EQ(cfw_checked_mul_size((size_t)2, SIZE_MAX, &size_result),
                       CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(size_result, (size_t)77);
    return 0;
}

const cfw_test_case cfw_test_checked_arithmetic_cases[] = {
    {"checked_add_success", test_checked_add_success},
    {"checked_sub_success", test_checked_sub_success},
    {"checked_mul_success", test_checked_mul_success},
    {"checked_add_boundaries", test_checked_add_boundaries},
    {"checked_sub_boundaries", test_checked_sub_boundaries},
    {"checked_mul_boundaries", test_checked_mul_boundaries},
    {"checked_add_overflow", test_checked_add_overflow},
    {"checked_sub_underflow", test_checked_sub_underflow},
    {"checked_mul_overflow", test_checked_mul_overflow},
    {"checked_i64_success_and_boundaries",
     test_checked_i64_success_and_boundaries},
    {"checked_i64_overflow", test_checked_i64_overflow},
    {"checked_arithmetic_null_result", test_checked_arithmetic_null_result},
    {"checked_arithmetic_preserves_result_on_error",
     test_checked_arithmetic_preserves_result_on_error},
    {NULL, NULL},
};
