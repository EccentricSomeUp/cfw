/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/sync.h>

#include <cfw/checked_arithmetic.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if CFW_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <pthread.h>
#endif

#if CFW_PLATFORM_POSIX
    _Static_assert(sizeof(pthread_mutex_t) <=
                       sizeof(((cfw_mutex_t *)NULL)->u.storage),
                   "cfw_mutex_t storage is too small for pthread_mutex_t");
    _Static_assert(sizeof(pthread_cond_t) <=
                       sizeof(((cfw_cond_t *)NULL)->u.storage),
                   "cfw_cond_t storage is too small for pthread_cond_t");
#endif

#define CFW_NS_PER_SECOND_I64 INT64_C(1000000000)

#if CFW_PLATFORM_POSIX
static cfw_status_t cfw_sync_map_error(int error)
{
    switch (error) {
    case 0:
        return CFW_OK;
    case EBUSY:
        return CFW_ERROR_WOULDBLOCK;
    case ETIMEDOUT:
        return CFW_ERROR_TIMEOUT;
    case ENOMEM:
    case EAGAIN:
        return CFW_ERROR_NOMEM;
    default:
        return CFW_ERROR_INTERNAL;
    }
}

/*
 * pthread_cond_timedwait interprets its abstime on the condition
 * variable's configured clock (CLOCK_REALTIME by default). cfw deadlines
 * are monotonic, so convert: abstime = realtime_now +
 * (deadline - monotonic_now). The small skew between the two clock reads
 * is immaterial for timeout semantics.
 */
static cfw_status_t cfw_sync_to_abstime(const cfw_timepoint_t *deadline,
                                        struct timespec *abstime)
{
    cfw_timepoint_t now;
    cfw_duration_t remaining;
    int64_t rem_ns;
    int64_t dsec;
    int64_t dnsec;
    struct timespec rt;
    cfw_status_t status;

    status = cfw_time_now(&now);
    if (status != CFW_OK) {
        return status;
    }
    status = cfw_timepoint_diff(deadline, &now, &remaining);
    if (status != CFW_OK) {
        return status;
    }
    rem_ns = remaining.nanoseconds;

    if (clock_gettime(CLOCK_REALTIME, &rt) != 0 || rt.tv_sec < 0 ||
        rt.tv_nsec < 0 || rt.tv_nsec >= CFW_NS_PER_SECOND_I64) {
        return CFW_ERROR_UNAVAILABLE;
    }

    if (rem_ns <= 0) {
        /* Already expired: abstime at the current instant so the wait
         * returns ETIMEDOUT without blocking. */
        abstime->tv_sec = rt.tv_sec;
        abstime->tv_nsec = rt.tv_nsec;
        return CFW_OK;
    }

    dsec = rem_ns / CFW_NS_PER_SECOND_I64;
    dnsec = rem_ns % CFW_NS_PER_SECOND_I64;

    status = cfw_checked_add_i64(rt.tv_sec, dsec, &abstime->tv_sec);
    if (status != CFW_OK) {
        return CFW_ERROR_OVERFLOW;
    }
    status = cfw_checked_add_i64(rt.tv_nsec, dnsec, &abstime->tv_nsec);
    if (status != CFW_OK) {
        return status;
    }
    if (abstime->tv_nsec >= CFW_NS_PER_SECOND_I64) {
        abstime->tv_nsec -= CFW_NS_PER_SECOND_I64;
        if (abstime->tv_sec == INT64_MAX) {
            return CFW_ERROR_OVERFLOW;
        }
        abstime->tv_sec++;
    }
    return CFW_OK;
}
#endif /* POSIX */

cfw_status_t cfw_mutex_init(cfw_mutex_t *mutex)
{
    if (mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(mutex->u.storage, 0, sizeof(mutex->u.storage));
#if CFW_PLATFORM_WINDOWS
    InitializeSRWLock((PSRWLOCK)mutex->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_mutex_init(
        (pthread_mutex_t *)mutex->u.storage, NULL));
#endif
}

cfw_status_t cfw_mutex_destroy(cfw_mutex_t *mutex)
{
    if (mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    /* SRWLOCK needs no destroy. */
    (void)mutex;
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_mutex_destroy(
        (pthread_mutex_t *)mutex->u.storage));
#endif
}

cfw_status_t cfw_mutex_lock(cfw_mutex_t *mutex)
{
    if (mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    AcquireSRWLockExclusive((PSRWLOCK)mutex->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_mutex_lock(
        (pthread_mutex_t *)mutex->u.storage));
#endif
}

cfw_status_t cfw_mutex_try_lock(cfw_mutex_t *mutex)
{
    if (mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    return TryAcquireSRWLockExclusive((PSRWLOCK)mutex->u.storage)
               ? CFW_OK
               : CFW_ERROR_WOULDBLOCK;
#else
    return cfw_sync_map_error(pthread_mutex_trylock(
        (pthread_mutex_t *)mutex->u.storage));
#endif
}

cfw_status_t cfw_mutex_unlock(cfw_mutex_t *mutex)
{
    if (mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    ReleaseSRWLockExclusive((PSRWLOCK)mutex->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_mutex_unlock(
        (pthread_mutex_t *)mutex->u.storage));
#endif
}

cfw_status_t cfw_cond_init(cfw_cond_t *cond)
{
    if (cond == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(cond->u.storage, 0, sizeof(cond->u.storage));
#if CFW_PLATFORM_WINDOWS
    InitConditionVariable((PCONDITION_VARIABLE)cond->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_cond_init(
        (pthread_cond_t *)cond->u.storage, NULL));
#endif
}

cfw_status_t cfw_cond_destroy(cfw_cond_t *cond)
{
    if (cond == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    (void)cond;
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_cond_destroy(
        (pthread_cond_t *)cond->u.storage));
#endif
}

cfw_status_t cfw_cond_wait(cfw_cond_t *cond, cfw_mutex_t *mutex,
                           const cfw_timepoint_t *deadline)
{
    cfw_status_t status;

    if (cond == NULL || mutex == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_PLATFORM_WINDOWS
    DWORD ms;
    BOOL woke;

    if (deadline == NULL) {
        ms = INFINITE;
    } else {
        cfw_timepoint_t now;
        cfw_duration_t remaining;
        int64_t rem_ns;

        status = cfw_time_now(&now);
        if (status != CFW_OK) {
            /* Lock is still held; surface the clock failure. */
            return status;
        }
        status = cfw_timepoint_diff(deadline, &now, &remaining);
        if (status != CFW_OK) {
            return status;
        }
        rem_ns = remaining.nanoseconds;
        /* SleepConditionVariableSRW takes milliseconds; round up so a
         * sub-millisecond remainder cannot fire early. INFINITE-1 ms is
         * ~49.7 days; anything longer waits indefinitely. */
        if (rem_ns <= 0) {
            ms = 0;
        } else if (rem_ns > (int64_t)(INFINITE - 1) * INT64_C(1000000)) {
            ms = INFINITE;
        } else {
            ms = (DWORD)((rem_ns + INT64_C(999999)) / INT64_C(1000000));
        }
    }

    woke = SleepConditionVariableSRW((PCONDITION_VARIABLE)cond->u.storage,
                                     (PSRWLOCK)mutex->u.storage, ms, 0);
    if (woke) {
        return CFW_OK;
    }
    /* FALSE means the wait timed out (ms granularity, rounded up) or a
     * wait error occurred; with no deadline only an error is possible. */
    if (deadline != NULL) {
        return CFW_ERROR_TIMEOUT;
    }
    return CFW_ERROR_INTERNAL;
#else
    if (deadline == NULL) {
        status = cfw_sync_map_error(pthread_cond_wait(
            (pthread_cond_t *)cond->u.storage,
            (pthread_mutex_t *)mutex->u.storage));
    } else {
        struct timespec abstime;
        status = cfw_sync_to_abstime(deadline, &abstime);
        if (status != CFW_OK) {
            /* The mutex is still held by the caller; the wait never
             * started. */
            return status;
        }
        status = cfw_sync_map_error(pthread_cond_timedwait(
            (pthread_cond_t *)cond->u.storage,
            (pthread_mutex_t *)mutex->u.storage, &abstime));
    }
    return status;
#endif
}

cfw_status_t cfw_cond_signal(cfw_cond_t *cond)
{
    if (cond == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    WakeConditionVariable((PCONDITION_VARIABLE)cond->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_cond_signal(
        (pthread_cond_t *)cond->u.storage));
#endif
}

cfw_status_t cfw_cond_broadcast(cfw_cond_t *cond)
{
    if (cond == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
#if CFW_PLATFORM_WINDOWS
    WakeAllConditionVariable((PCONDITION_VARIABLE)cond->u.storage);
    return CFW_OK;
#else
    return cfw_sync_map_error(pthread_cond_broadcast(
        (pthread_cond_t *)cond->u.storage));
#endif
}
