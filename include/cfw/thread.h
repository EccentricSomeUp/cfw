/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_THREAD_H
#define CFW_THREAD_H

#include <cfw/export.h>
#include <cfw/max_align.h>
#include <cfw/status.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cfw_thread_t is the framework's portable thread-spawn primitive
 * (ADR 0051), complementing the cfw_mutex_t/cfw_cond_t synchronization
 * primitives in cfw/sync.h. The server itself remains single-threaded;
 * this exists for application-owned concurrency (worker threads,
 * connection handoff, etc.).
 *
 * Lifecycle contract:
 * - A thread object becomes valid only after a successful
 *   cfw_thread_create. It must then be consumed exactly once by either
 *   cfw_thread_join or cfw_thread_detach; doing both, or doing either
 *   twice, is undefined behavior.
 * - cfw_thread_join blocks the calling thread until the spawned thread
 *   exits and reports its exit status. cfw_thread_detach releases
 *   joinability immediately; the thread runs to completion and the OS
 *   reclaims its resources.
 * - A consumed (joined or detached) object may be reused by a
 *   subsequent cfw_thread_create.
 * - A thread that is neither joined nor detached before process exit
 *   leaks its joinable state; this matches raw pthread/Win32
 *   semantics and is the caller's responsibility.
 *
 * Entry-point contract:
 * - cfw_thread_fn runs on the new thread with `arg` unchanged. Its
 *   int return value is the thread's exit status, reported by
 *   cfw_thread_join (or discarded for detached threads).
 * - The entry point must not return until it is done; there is no
 *   cancellation. Returning from main while joinable threads still run
 *   is the caller's responsibility (same as C/POSIX rules).
 */

typedef int (*cfw_thread_fn)(void *arg);

typedef struct cfw_thread {
    union {
        /* Guarantees alignment for any platform thread handle stored
         * in `storage` (pthread_t, HANDLE, ...). */
        cfw_max_align_t align;
        unsigned char storage[64];
    } u;
} cfw_thread_t;

CFW_API cfw_status_t cfw_thread_create(cfw_thread_t *thread,
                                       cfw_thread_fn fn, void *arg);
CFW_API cfw_status_t cfw_thread_join(cfw_thread_t *thread, int *exit_code);
CFW_API cfw_status_t cfw_thread_detach(cfw_thread_t *thread);

#ifdef __cplusplus
}
#endif

#endif /* CFW_THREAD_H */
