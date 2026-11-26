/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/time.h>

#include <cfw/checked_arithmetic.h>

#include <stdint.h>
#include <time.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
#endif

#define CFW_NS_PER_SECOND INT64_C(1000000000)
#define CFW_NS_PER_MILLI INT64_C(1000000)
#define CFW_NS_PER_MICRO INT64_C(1000)

static int cfw_duration_is_valid(const cfw_duration_t *duration)
{
    return duration != NULL;
}

static int cfw_timepoint_is_valid(const cfw_timepoint_t *timepoint)
{
    return timepoint != NULL;
}

static cfw_status_t cfw_time_store_ns(int64_t nanoseconds, cfw_timepoint_t *out)
{
    if (nanoseconds < 0 || out == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    out->nanoseconds = nanoseconds;
    return CFW_OK;
}

static cfw_status_t cfw_time_monotonic_ns(int64_t *nanoseconds)
{
#if CFW_PLATFORM_WINDOWS
    LARGE_INTEGER counts;
    LARGE_INTEGER frequency;
    uint64_t count_value;
    uint64_t frequency_value;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t result;

    if (!QueryPerformanceCounter(&counts) || !QueryPerformanceFrequency(&frequency) ||
        counts.QuadPart < 0 || frequency.QuadPart <= 0) {
        return CFW_ERROR_UNAVAILABLE;
    }

    count_value = (uint64_t)counts.QuadPart;
    frequency_value = (uint64_t)frequency.QuadPart;

    if (frequency_value > UINT64_MAX / (uint64_t)CFW_NS_PER_SECOND) {
        return CFW_ERROR_UNAVAILABLE;
    }

    seconds = count_value / frequency_value;
    remainder = count_value % frequency_value;

    if (seconds > (uint64_t)INT64_MAX / (uint64_t)CFW_NS_PER_SECOND) {
        return CFW_ERROR_OVERFLOW;
    }
    result = seconds * (uint64_t)CFW_NS_PER_SECOND;
    result += (remainder * (uint64_t)CFW_NS_PER_SECOND) / frequency_value;
    if (result > (uint64_t)INT64_MAX) {
        return CFW_ERROR_OVERFLOW;
    }

    *nanoseconds = (int64_t)result;
    return CFW_OK;
#elif CFW_HAVE_CLOCK_MONOTONIC
    struct timespec ts;
    uint64_t seconds;
    uint64_t result;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return CFW_ERROR_UNAVAILABLE;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return CFW_ERROR_UNAVAILABLE;
    }

    seconds = (uint64_t)ts.tv_sec;
    if (seconds > (uint64_t)INT64_MAX / (uint64_t)CFW_NS_PER_SECOND) {
        return CFW_ERROR_OVERFLOW;
    }
    result = seconds * (uint64_t)CFW_NS_PER_SECOND + (uint64_t)ts.tv_nsec;
    if (result > (uint64_t)INT64_MAX) {
        return CFW_ERROR_OVERFLOW;
    }

    *nanoseconds = (int64_t)result;
    return CFW_OK;
#else
    (void)nanoseconds;
    return CFW_ERROR_UNAVAILABLE;
#endif
}

cfw_status_t cfw_duration_from_nanos(int64_t value, cfw_duration_t *duration)
{
    if (duration == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    duration->nanoseconds = value;
    return CFW_OK;
}

cfw_status_t cfw_duration_from_micros(int64_t value, cfw_duration_t *duration)
{
    int64_t nanos;
    cfw_status_t status;

    if (duration == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_mul_i64(value, CFW_NS_PER_MICRO, &nanos);
    if (status != CFW_OK) {
        return status;
    }

    duration->nanoseconds = nanos;
    return CFW_OK;
}

cfw_status_t cfw_duration_from_millis(int64_t value, cfw_duration_t *duration)
{
    int64_t nanos;
    cfw_status_t status;

    if (duration == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_mul_i64(value, CFW_NS_PER_MILLI, &nanos);
    if (status != CFW_OK) {
        return status;
    }

    duration->nanoseconds = nanos;
    return CFW_OK;
}

cfw_status_t cfw_duration_from_seconds(int64_t value, cfw_duration_t *duration)
{
    int64_t nanos;
    cfw_status_t status;

    if (duration == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_mul_i64(value, CFW_NS_PER_SECOND, &nanos);
    if (status != CFW_OK) {
        return status;
    }

    duration->nanoseconds = nanos;
    return CFW_OK;
}

cfw_duration_t cfw_duration_zero(void)
{
    cfw_duration_t duration = {0};
    return duration;
}

cfw_status_t cfw_duration_add(const cfw_duration_t *a, const cfw_duration_t *b,
                              cfw_duration_t *result)
{
    int64_t nanos;
    cfw_status_t status;

    if (!cfw_duration_is_valid(a) || !cfw_duration_is_valid(b) ||
        result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_add_i64(a->nanoseconds, b->nanoseconds, &nanos);
    if (status != CFW_OK) {
        return status;
    }

    result->nanoseconds = nanos;
    return CFW_OK;
}

cfw_status_t cfw_duration_sub(const cfw_duration_t *a, const cfw_duration_t *b,
                              cfw_duration_t *result)
{
    int64_t nanos;
    cfw_status_t status;

    if (!cfw_duration_is_valid(a) || !cfw_duration_is_valid(b) ||
        result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_sub_i64(a->nanoseconds, b->nanoseconds, &nanos);
    if (status != CFW_OK) {
        return status;
    }

    result->nanoseconds = nanos;
    return CFW_OK;
}

cfw_status_t cfw_duration_compare(const cfw_duration_t *a,
                                  const cfw_duration_t *b, int *result)
{
    if (!cfw_duration_is_valid(a) || !cfw_duration_is_valid(b) ||
        result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (a->nanoseconds < b->nanoseconds) {
        *result = -1;
    } else if (a->nanoseconds > b->nanoseconds) {
        *result = 1;
    } else {
        *result = 0;
    }
    return CFW_OK;
}

cfw_status_t cfw_duration_to_nanos(const cfw_duration_t *duration,
                                   int64_t *result)
{
    if (!cfw_duration_is_valid(duration) || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *result = duration->nanoseconds;
    return CFW_OK;
}

cfw_status_t cfw_duration_to_micros(const cfw_duration_t *duration,
                                    int64_t *result)
{
    if (!cfw_duration_is_valid(duration) || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *result = duration->nanoseconds / CFW_NS_PER_MICRO;
    return CFW_OK;
}

cfw_status_t cfw_duration_to_millis(const cfw_duration_t *duration,
                                    int64_t *result)
{
    if (!cfw_duration_is_valid(duration) || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    *result = duration->nanoseconds / CFW_NS_PER_MILLI;
    return CFW_OK;
}

cfw_status_t cfw_time_now(cfw_timepoint_t *timepoint)
{
    int64_t nanoseconds;
    cfw_status_t status;

    if (timepoint == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_time_monotonic_ns(&nanoseconds);
    if (status != CFW_OK) {
        return status;
    }

    return cfw_time_store_ns(nanoseconds, timepoint);
}

cfw_status_t cfw_time_wall_now(int64_t *unix_seconds)
{
    if (unix_seconds == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    FILETIME file_time;
    uint64_t ticks;
    GetSystemTimePreciseAsFileTime(&file_time);
    ticks = ((uint64_t)file_time.dwHighDateTime << 32) |
            (uint64_t)file_time.dwLowDateTime;
    /* FILETIME epoch is 1601-01-01; Unix epoch offset is
     * 11644473600 seconds. */
    if (ticks < 116444736000000000ULL) {
        return CFW_ERROR_UNAVAILABLE;
    }
    *unix_seconds = (int64_t)((ticks - 116444736000000000ULL) / 10000000ULL);
    return CFW_OK;
#elif CFW_HAVE_CLOCK_REALTIME
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
        return CFW_ERROR_UNAVAILABLE;
    }
    *unix_seconds = (int64_t)ts.tv_sec;
    return CFW_OK;
#else
    (void)unix_seconds;
    return CFW_ERROR_UNAVAILABLE;
#endif
}

cfw_status_t cfw_timepoint_add(const cfw_timepoint_t *timepoint,
                               const cfw_duration_t *duration,
                               cfw_timepoint_t *result)
{
    int64_t nanos;
    cfw_status_t status;

    if (!cfw_timepoint_is_valid(timepoint) ||
        !cfw_duration_is_valid(duration) || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_add_i64(timepoint->nanoseconds, duration->nanoseconds,
                                 &nanos);
    if (status != CFW_OK) {
        return status;
    }

    return cfw_time_store_ns(nanos, result);
}

cfw_status_t cfw_timepoint_sub(const cfw_timepoint_t *timepoint,
                               const cfw_duration_t *duration,
                               cfw_timepoint_t *result)
{
    int64_t nanos;
    cfw_status_t status;

    if (!cfw_timepoint_is_valid(timepoint) ||
        !cfw_duration_is_valid(duration) || result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_sub_i64(timepoint->nanoseconds, duration->nanoseconds,
                                 &nanos);
    if (status != CFW_OK) {
        return status;
    }

    return cfw_time_store_ns(nanos, result);
}

cfw_status_t cfw_timepoint_diff(const cfw_timepoint_t *later,
                                const cfw_timepoint_t *earlier,
                                cfw_duration_t *duration)
{
    int64_t nanos;
    cfw_status_t status;

    if (!cfw_timepoint_is_valid(later) || !cfw_timepoint_is_valid(earlier) ||
        duration == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_checked_sub_i64(later->nanoseconds, earlier->nanoseconds,
                                 &nanos);
    if (status != CFW_OK) {
        return status;
    }

    duration->nanoseconds = nanos;
    return CFW_OK;
}

cfw_status_t cfw_timepoint_compare(const cfw_timepoint_t *a,
                                   const cfw_timepoint_t *b, int *result)
{
    if (!cfw_timepoint_is_valid(a) || !cfw_timepoint_is_valid(b) ||
        result == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (a->nanoseconds < b->nanoseconds) {
        *result = -1;
    } else if (a->nanoseconds > b->nanoseconds) {
        *result = 1;
    } else {
        *result = 0;
    }
    return CFW_OK;
}

cfw_status_t cfw_deadline_from_now(cfw_timepoint_t *deadline,
                                   cfw_duration_t timeout)
{
    cfw_timepoint_t now;
    cfw_status_t status;

    if (deadline == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_time_now(&now);
    if (status != CFW_OK) {
        return status;
    }

    return cfw_timepoint_add(&now, &timeout, deadline);
}

cfw_status_t cfw_deadline_expired(const cfw_timepoint_t *deadline,
                                  int *expired)
{
    cfw_timepoint_t now;
    cfw_status_t status;
    int comparison;

    if (!cfw_timepoint_is_valid(deadline) || expired == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_time_now(&now);
    if (status != CFW_OK) {
        return status;
    }

    status = cfw_timepoint_compare(&now, deadline, &comparison);
    if (status != CFW_OK) {
        return status;
    }

    *expired = comparison >= 0;
    return CFW_OK;
}
