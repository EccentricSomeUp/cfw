/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include <cfw/cfw.h>

#include "test_harness.h"

#include <stdint.h>

static int test_duration_from_units(void)
{
    cfw_duration_t duration;
    int64_t nanos;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(INT64_C(7), &duration), CFW_OK);
    CFW_TEST_ASSERT_EQ(duration.nanoseconds, 7);

    CFW_TEST_ASSERT_EQ(cfw_duration_from_micros(INT64_C(3), &duration), CFW_OK);
    CFW_TEST_ASSERT_EQ(duration.nanoseconds, INT64_C(3000));

    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(INT64_C(2), &duration), CFW_OK);
    CFW_TEST_ASSERT_EQ(duration.nanoseconds, INT64_C(2000000));

    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(INT64_C(1), &duration),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(duration.nanoseconds, INT64_C(1000000000));

    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(-5, &duration), CFW_OK);
    CFW_TEST_ASSERT_EQ(duration.nanoseconds, INT64_C(-5000000));

    CFW_TEST_ASSERT_EQ(cfw_duration_to_nanos(&duration, &nanos), CFW_OK);
    CFW_TEST_ASSERT_EQ(nanos, INT64_C(-5000000));
    return 0;
}

static int test_duration_from_rejects_null(void)
{
    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(1, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_duration_from_overflow(void)
{
    cfw_duration_t duration;

    CFW_TEST_ASSERT_EQ(
        cfw_duration_from_seconds(INT64_MAX / 1000000000, &duration), CFW_OK);
    CFW_TEST_ASSERT_EQ(
        cfw_duration_from_seconds(INT64_MAX / 1000000000 + 1, &duration),
        CFW_ERROR_OVERFLOW);
    CFW_TEST_ASSERT_EQ(
        cfw_duration_from_seconds(INT64_MIN / 1000000000 - 1, &duration),
        CFW_ERROR_OVERFLOW);
    return 0;
}

static int test_duration_add_sub_compare(void)
{
    cfw_duration_t a;
    cfw_duration_t b;
    cfw_duration_t result;
    int comparison;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(5, &a), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(3, &b), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_duration_add(&a, &b, &result), CFW_OK);
    {
        int64_t millis;
        CFW_TEST_ASSERT_EQ(cfw_duration_to_millis(&result, &millis), CFW_OK);
        CFW_TEST_ASSERT_EQ(millis, 8);
    }

    CFW_TEST_ASSERT_EQ(cfw_duration_sub(&a, &b, &result), CFW_OK);
    {
        int64_t millis;
        CFW_TEST_ASSERT_EQ(cfw_duration_to_millis(&result, &millis), CFW_OK);
        CFW_TEST_ASSERT_EQ(millis, 2);
    }

    CFW_TEST_ASSERT_EQ(cfw_duration_compare(&a, &b, &comparison), CFW_OK);
    CFW_TEST_ASSERT_EQ(comparison, 1);
    CFW_TEST_ASSERT_EQ(cfw_duration_compare(&b, &a, &comparison), CFW_OK);
    CFW_TEST_ASSERT_EQ(comparison, -1);
    CFW_TEST_ASSERT_EQ(cfw_duration_compare(&a, &a, &comparison), CFW_OK);
    CFW_TEST_ASSERT_EQ(comparison, 0);

    CFW_TEST_ASSERT_EQ(cfw_duration_add(NULL, &b, &result),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_compare(&a, NULL, &comparison),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_duration_arithmetic_overflow(void)
{
    cfw_duration_t max_duration;
    cfw_duration_t one_ns;
    cfw_duration_t result;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(INT64_MAX, &max_duration),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(1, &one_ns), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_duration_add(&max_duration, &one_ns, &result),
                       CFW_ERROR_OVERFLOW);

    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(INT64_MIN, &max_duration),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_sub(&max_duration, &one_ns, &result),
                       CFW_ERROR_OVERFLOW);
    return 0;
}

static int test_duration_to_truncates_toward_zero(void)
{
    cfw_duration_t duration;
    int64_t millis;
    int64_t micros;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(INT64_C(1500000), &duration),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_millis(&duration, &millis), CFW_OK);
    CFW_TEST_ASSERT_EQ(millis, 1);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_micros(&duration, &micros), CFW_OK);
    CFW_TEST_ASSERT_EQ(micros, 1500);

    CFW_TEST_ASSERT_EQ(cfw_duration_from_nanos(INT64_C(-1500000), &duration),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_millis(&duration, &millis), CFW_OK);
    CFW_TEST_ASSERT_EQ(millis, -1);
    return 0;
}

static int test_time_now_is_monotonic_and_positive(void)
{
    cfw_timepoint_t first;
    cfw_timepoint_t second;
    cfw_duration_t elapsed;
    int comparison;

    CFW_TEST_ASSERT_EQ(cfw_time_now(&first), CFW_OK);
    CFW_TEST_ASSERT(first.nanoseconds > 0);

    CFW_TEST_ASSERT_EQ(cfw_time_now(&second), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_compare(&second, &first, &comparison),
                       CFW_OK);
    CFW_TEST_ASSERT(comparison >= 0);

    CFW_TEST_ASSERT_EQ(cfw_timepoint_diff(&second, &first, &elapsed), CFW_OK);
    CFW_TEST_ASSERT(elapsed.nanoseconds >= 0);

    CFW_TEST_ASSERT_EQ(cfw_time_now(NULL), CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_timepoint_arithmetic(void)
{
    cfw_timepoint_t base;
    cfw_timepoint_t later;
    cfw_timepoint_t earlier;
    cfw_timepoint_t round_trip;
    cfw_duration_t one_second;
    cfw_duration_t elapsed;
    int comparison;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(1, &one_second), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_time_now(&base), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_timepoint_add(&base, &one_second, &later), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_sub(&base, &one_second, &earlier), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_diff(&later, &earlier, &elapsed),
                       CFW_OK);
    {
        int64_t seconds = elapsed.nanoseconds / 1000000000;
        CFW_TEST_ASSERT_EQ(seconds, 2);
    }

    CFW_TEST_ASSERT_EQ(cfw_timepoint_sub(&later, &one_second, &round_trip),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_compare(&round_trip, &base, &comparison),
                       CFW_OK);
    CFW_TEST_ASSERT_EQ(comparison, 0);

    /* Subtracting more than the elapsed time must fail closed. */
    {
        cfw_duration_t huge;
        CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(INT64_MAX / 2000000000,
                                                     &huge),
                           CFW_OK);
        CFW_TEST_ASSERT_EQ(cfw_timepoint_sub(&base, &huge, &earlier),
                           CFW_ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

static int test_deadline_zero_timeout_is_expired(void)
{
    cfw_timepoint_t deadline;
    cfw_duration_t zero = cfw_duration_zero();
    int expired;

    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, zero), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_expired(&deadline, &expired), CFW_OK);
    CFW_TEST_ASSERT_TRUE(expired);
    return 0;
}

static int test_deadline_future_timeout_not_expired(void)
{
    cfw_timepoint_t deadline;
    cfw_duration_t one_hour;
    int expired;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(3600, &one_hour), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, one_hour), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_expired(&deadline, &expired), CFW_OK);
    CFW_TEST_ASSERT_FALSE(expired);

    CFW_TEST_ASSERT_EQ(cfw_deadline_expired(NULL, &expired),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_deadline_expired(&deadline, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_deadline_past_is_expired(void)
{
    cfw_timepoint_t deadline;
    cfw_duration_t negative;
    int expired;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_seconds(-1, &negative), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(&deadline, negative), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_deadline_expired(&deadline, &expired), CFW_OK);
    CFW_TEST_ASSERT_TRUE(expired);
    return 0;
}

static int test_time_wall_now(void)
{
    int64_t a;
    int64_t b;

    CFW_TEST_ASSERT_EQ(cfw_time_wall_now(&a), CFW_OK);
    CFW_TEST_ASSERT_TRUE(a > INT64_C(1700000000)); /* post-2023 sanity */
    CFW_TEST_ASSERT_EQ(cfw_time_wall_now(&b), CFW_OK);
    CFW_TEST_ASSERT_TRUE(b >= a);
    CFW_TEST_ASSERT_EQ(cfw_time_wall_now(NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

static int test_time_null_output_args(void)
{
    cfw_duration_t dur;
    cfw_duration_t dur2;
    cfw_timepoint_t tp;

    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(5, &dur), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_duration_from_millis(7, &dur2), CFW_OK);
    CFW_TEST_ASSERT_EQ(cfw_time_now(&tp), CFW_OK);

    CFW_TEST_ASSERT_EQ(cfw_duration_sub(&dur, &dur2, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_compare(&dur, &dur2, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_nanos(&dur, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_micros(&dur, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_duration_to_millis(&dur, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);

    CFW_TEST_ASSERT_EQ(cfw_timepoint_add(&tp, &dur, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_sub(&tp, &dur, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_diff(&tp, &tp, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_timepoint_compare(&tp, &tp, NULL),
                       CFW_ERROR_INVALID_ARGUMENT);
    CFW_TEST_ASSERT_EQ(cfw_deadline_from_now(NULL, dur),
                       CFW_ERROR_INVALID_ARGUMENT);
    return 0;
}

const cfw_test_case cfw_test_time_cases[] = {
    {"duration_from_units", test_duration_from_units},
    {"duration_from_rejects_null", test_duration_from_rejects_null},
    {"duration_from_overflow", test_duration_from_overflow},
    {"duration_add_sub_compare", test_duration_add_sub_compare},
    {"duration_arithmetic_overflow", test_duration_arithmetic_overflow},
    {"duration_to_truncates_toward_zero",
     test_duration_to_truncates_toward_zero},
    {"time_now_is_monotonic_and_positive", test_time_now_is_monotonic_and_positive},
    {"timepoint_arithmetic", test_timepoint_arithmetic},
    {"deadline_zero_timeout_is_expired", test_deadline_zero_timeout_is_expired},
    {"deadline_future_timeout_not_expired",
     test_deadline_future_timeout_not_expired},
    {"deadline_past_is_expired", test_deadline_past_is_expired},
    {"time_wall_now", test_time_wall_now},
    {"time_null_output_args", test_time_null_output_args},
    {NULL, NULL},
};
