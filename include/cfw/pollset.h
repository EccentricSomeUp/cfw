/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_POLLSET_H
#define CFW_POLLSET_H

#include <cfw/export.h>
#include <cfw/status.h>
#include <cfw/time.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Level-triggered readiness event set for nonblocking descriptors
 * (ADR 0069; contract per ADRs 0011/0014). This is the wait primitive
 * that pairs with cfw/net.h: register socket descriptors, then block
 * in cfw_pollset_wait until any of them is ready or the deadline
 * expires.
 *
 * Semantics:
 * - Level-triggered: a descriptor that remains ready is reported on
 *   every wait until its readiness is consumed by I/O or it is
 *   removed/modified. Loop until CFW_ERROR_WOULDBLOCK after acting on
 *   events when edge-triggered-style batching is desired.
 * - `handle` is an opaque descriptor identifier — for sockets use the
 *   value returned by cfw_fd_get(); for TLS connections use
 *   cfw_tls_conn_fd(). It must be nonnegative and fit in int32_t.
 * - `token` is caller data returned verbatim in the matching
 *   cfw_poll_event_t; it is never dereferenced by the pollset.
 * - Registration persists until cfw_pollset_remove or
 *   cfw_pollset_destroy; re-adding an existing handle fails with
 *   CFW_ERROR_INVALID_ARGUMENT — use cfw_pollset_modify to change
 *   interest.
 * - CFW_POLL_ERR and CFW_POLL_HUP are always reported when the backend
 *   signals them, regardless of the registered interest mask.
 *
 * Ownership and threading:
 * - cfw_pollset_create allocates; cfw_pollset_destroy releases and
 *   clears every registration. Descriptors themselves are NOT owned —
 *   closing a registered fd without removing it first is a contract
 *   violation (remove before close, or destroy the set).
 * - A pollset is NOT thread-safe: registration changes must not race
 *   cfw_pollset_wait. Concurrent use requires external synchronization
 *   or one pollset per thread.
 *
 * Platform support: POSIX only — epoll on Linux/Android, kqueue on
 * macOS/BSD, poll() fallback elsewhere. On Windows (and any platform
 * without a backend) every function returns CFW_ERROR_UNAVAILABLE.
 */

typedef enum cfw_poll_events {
    CFW_POLL_IN = 1u << 0,
    CFW_POLL_OUT = 1u << 1,
    /* Peer error / hangup conditions; always reported. */
    CFW_POLL_ERR = 1u << 2,
    CFW_POLL_HUP = 1u << 3
} cfw_poll_events_t;

typedef struct cfw_poll_event {
    /* Descriptor handle as registered (cfw_fd_get / cfw_tls_conn_fd). */
    intptr_t handle;
    /* Ready subset of the registered interest plus ERR/HUP. */
    uint32_t events;
    /* Token supplied at add/modify time. */
    void *token;
} cfw_poll_event_t;

typedef struct cfw_pollset cfw_pollset_t;

/* Allocates an empty pollset. Returns CFW_ERROR_NOMEM on allocation
 * failure, CFW_ERROR_UNAVAILABLE without a poll backend. */
CFW_API cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset);

/* Releases the pollset and all registrations. NULL-safe; does not
 * close or otherwise touch registered descriptors. */
CFW_API void cfw_pollset_destroy(cfw_pollset_t *pollset);

/* Registers `handle` with interest `events` (CFW_POLL_IN/OUT;
 * ERR/HUP are implicit) and `token`. Fails with
 * CFW_ERROR_INVALID_ARGUMENT for invalid arguments or a duplicate
 * handle, CFW_ERROR_UNAVAILABLE without a backend. */
CFW_API cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset,
                                     intptr_t handle, uint32_t events,
                                     void *token);

/* Replaces the interest mask and token for a registered handle.
 * Returns CFW_ERROR_INVALID_ARGUMENT when the handle is not
 * registered or the arguments are invalid. */
CFW_API cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset,
                                        intptr_t handle, uint32_t events,
                                        void *token);

/* Drops a registration. Returns CFW_ERROR_INVALID_ARGUMENT for
 * invalid arguments or a handle that is not registered. */
CFW_API cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset,
                                        intptr_t handle);

/* Blocks until at least one registered descriptor is ready, the
 * deadline expires, or the call is interrupted. On success *count is
 * the number of ready descriptors written to `events` (up to
 * max_events; further ready descriptors are reported on the next
 * call — level-triggered semantics guarantee no loss). A NULL
 * deadline waits indefinitely. Returns CFW_ERROR_TIMEOUT with
 * *count == 0 when the deadline expires, CFW_ERROR_INVALID_ARGUMENT
 * for bad arguments, CFW_ERROR_NOMEM if the internal ready buffer
 * cannot grow, CFW_ERROR_UNAVAILABLE without a backend. */
CFW_API cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                                      const cfw_timepoint_t *deadline,
                                      cfw_poll_event_t *events,
                                      size_t max_events, size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* CFW_POLLSET_H */
