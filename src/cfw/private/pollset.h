/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_POLLSET_H
#define CFW_PRIVATE_POLLSET_H

/*
 * Internal event-set abstraction. Not part of the public API; see
 * docs/decisions/0011-fd-ownership-and-event-design.md for the contract.
 * Level-triggered semantics. Backends: epoll on Linux/Android, poll()
 * elsewhere on POSIX, unavailable on Windows until IOCP lands.
 *
 * A pollset is not thread-safe: registration changes must not race with
 * cfw_pollset_wait on the same set. Concurrent use requires external
 * synchronization or one pollset per thread.
 */

#include <cfw/status.h>
#include <cfw/time.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cfw_poll_events {
    CFW_POLL_IN = 1u << 0,
    CFW_POLL_OUT = 1u << 1,
    CFW_POLL_ERR = 1u << 2,
    CFW_POLL_HUP = 1u << 3
} cfw_poll_events_t;

typedef struct cfw_poll_event {
    intptr_t handle;
    uint32_t events;
    void *token;
} cfw_poll_event_t;

typedef struct cfw_pollset cfw_pollset_t;

cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset);
void cfw_pollset_destroy(cfw_pollset_t *pollset);
cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset, intptr_t handle,
                                     uint32_t events, void *token);
cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset, intptr_t handle,
                                        uint32_t events, void *token);
cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset,
                                        intptr_t handle);
cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                                      const cfw_timepoint_t *deadline,
                                      cfw_poll_event_t *events,
                                      size_t max_events, size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* CFW_PRIVATE_POLLSET_H */
