/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_SYNC_H
#define CFW_SYNC_H

#include <cfw/export.h>
#include <cfw/max_align.h>
#include <cfw/status.h>
#include <cfw/time.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_mutex_t and cfw_cond_t are the framework's portable
 * synchronization primitives (ADR 0050). They provide non-recursive
 * mutual exclusion and condition variables with optional monotonic
 * deadlines, backed by pthreads on POSIX and SRWLOCK/CONDITION_VARIABLE
 * on Windows.
 *
 * Storage model:
 * - The structs carry fixed inline storage; no allocation occurs.
 * - Objects must be initialized with cfw_*_init before use and
 *   destroyed with cfw_*_destroy afterwards. Initializing an
 *   already-initialized object or using a destroyed object is
 *   undefined behavior.
 * - Neither type is copyable; copying the struct bytes does not create
 *   a second valid primitive.
 *
 * Mutex contract:
 * - Locking is non-recursive: a thread that already holds the mutex
 *   must not lock it again (undefined behavior on POSIX, deadlock on
 *   some platforms).
 * - cfw_mutex_unlock must only be called by the thread that holds the
 *   lock.
 * - cfw_mutex_try_lock returns CFW_OK when the lock was acquired and
 *   CFW_ERROR_WOULDBLOCK when it is held by another (or the same)
 *   thread.
 *
 * Condition-variable contract:
 * - cfw_cond_wait atomically releases `mutex` and blocks until
 *   signaled, the deadline expires, or a spurious wakeup occurs; the
 *   mutex is always held again when the call returns (on every return
 *   path, including timeout and error).
 * - `deadline == NULL` waits indefinitely; otherwise it is an absolute
 *   monotonic timepoint as produced by cfw_time_now /
 *   cfw_deadline_from_now. An already-expired deadline returns
 *   CFW_ERROR_TIMEOUT without blocking.
 * - Because wakeups may be spurious, callers must re-check their wait
 *   predicate while holding the mutex after every wait.
 * - cfw_cond_wait must only be called with `mutex` held by the calling
 *   thread. signal/broadcast do not require holding the mutex, though
 *   holding it while signaling can reduce missed-wakeup windows.
 *
 * All functions return CFW_OK on success or a cfw_status_t failure
 * class; NULL object arguments yield CFW_ERROR_INVALID_ARGUMENT.
 */

typedef struct cfw_mutex {
    union {
        /* Guarantees alignment for any platform primitive stored in
         * `storage` (pthread_mutex_t, SRWLOCK, ...). */
        cfw_max_align_t align;
        unsigned char storage[256];
    } u;
} cfw_mutex_t;

typedef struct cfw_cond {
    union {
        cfw_max_align_t align;
        unsigned char storage[256];
    } u;
} cfw_cond_t;

CFW_API cfw_status_t cfw_mutex_init(cfw_mutex_t *mutex);
CFW_API cfw_status_t cfw_mutex_destroy(cfw_mutex_t *mutex);
CFW_API cfw_status_t cfw_mutex_lock(cfw_mutex_t *mutex);
CFW_API cfw_status_t cfw_mutex_try_lock(cfw_mutex_t *mutex);
CFW_API cfw_status_t cfw_mutex_unlock(cfw_mutex_t *mutex);

CFW_API cfw_status_t cfw_cond_init(cfw_cond_t *cond);
CFW_API cfw_status_t cfw_cond_destroy(cfw_cond_t *cond);
CFW_API cfw_status_t cfw_cond_wait(cfw_cond_t *cond, cfw_mutex_t *mutex,
                                   const cfw_timepoint_t *deadline);
CFW_API cfw_status_t cfw_cond_signal(cfw_cond_t *cond);
CFW_API cfw_status_t cfw_cond_broadcast(cfw_cond_t *cond);

#ifdef __cplusplus
}
#endif

#endif /* CFW_SYNC_H */
