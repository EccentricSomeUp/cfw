/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#if defined(__linux__) || defined(__ANDROID__)
    #define _GNU_SOURCE
#endif

#include "pollset.h"

#include <cfw/checked_arithmetic.h>
#include <cfw/time.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* CFW_POLLF_FORCE_POLL is a test-only override that selects the poll()
 * backend on platforms that would otherwise use epoll, so the fallback
 * path is exercised in CI on Linux. It is never defined by the build. */
#if (defined(__linux__) || defined(__ANDROID__)) && \
    !defined(CFW_POLLF_FORCE_POLL)
    #include <sys/epoll.h>
    #include <unistd.h>
    #define CFW_POLLF_HAVE_EPOLL 1
#elif defined(_WIN32)
    /* IOCP backend is designed but not implemented (ADR 0011). */
    #define CFW_POLLF_HAVE_NONE 1
#else
    #include <poll.h>
    #include <unistd.h>
    #define CFW_POLLF_HAVE_POLL 1
#endif

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static int cfw_pollset_level_is_valid(uint32_t events)
{
    const uint32_t known = CFW_POLL_IN | CFW_POLL_OUT | CFW_POLL_ERR |
                           CFW_POLL_HUP;
    return (events & ~known) == 0;
}

static int cfw_pollset_handle_is_valid(intptr_t handle)
{
    return handle >= 0 && handle <= INT32_MAX;
}

static cfw_status_t cfw_pollset_timeout_ms(const cfw_timepoint_t *deadline,
                                           int *timeout_ms)
{
    cfw_timepoint_t now;
    cfw_duration_t remaining;
    cfw_status_t status;
    int64_t millis;

    if (deadline == NULL) {
        *timeout_ms = -1;
        return CFW_OK;
    }

    status = cfw_time_now(&now);
    if (status != CFW_OK) {
        return status;
    }

    status = cfw_timepoint_diff(deadline, &now, &remaining);
    if (status != CFW_OK || remaining.nanoseconds <= 0) {
        *timeout_ms = 0;
        return CFW_OK;
    }

    /* Round up to milliseconds so a sub-ms remainder cannot expire early. */
    millis = (remaining.nanoseconds + INT64_C(999999)) / INT64_C(1000000);
    if (millis > INT32_MAX) {
        millis = INT32_MAX;
    }
    if (millis < 0) {
        millis = 0;
    }
    *timeout_ms = (int)millis;
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* epoll backend (Linux/Android)                                       */
/* ------------------------------------------------------------------ */

#ifdef CFW_POLLF_HAVE_EPOLL

typedef struct cfw_epoll_record {
    intptr_t handle;
    void *token;
} cfw_epoll_record_t;

typedef struct cfw_epoll_slot {
    int fd;
    cfw_epoll_record_t *record;
} cfw_epoll_slot_t;

struct cfw_pollset {
    int epfd;
    cfw_epoll_slot_t *slots;
    size_t count;
    size_t capacity;
};

static uint32_t cfw_epoll_to_cfw(uint32_t epoll_events)
{
    uint32_t events = 0;

    if ((epoll_events & EPOLLIN) != 0) {
        events |= CFW_POLL_IN;
    }
    if ((epoll_events & EPOLLOUT) != 0) {
        events |= CFW_POLL_OUT;
    }
    if ((epoll_events & EPOLLERR) != 0) {
        events |= CFW_POLL_ERR;
    }
    if ((epoll_events & EPOLLHUP) != 0) {
        events |= CFW_POLL_HUP;
    }
    return events;
}

static uint32_t cfw_cfw_to_epoll(uint32_t events)
{
    uint32_t epoll_events = 0;

    if ((events & CFW_POLL_IN) != 0) {
        epoll_events |= EPOLLIN;
    }
    if ((events & CFW_POLL_OUT) != 0) {
        epoll_events |= EPOLLOUT;
    }
    return epoll_events;
}

static ssize_t cfw_epoll_find(const cfw_pollset_t *pollset, int fd)
{
    size_t i;

    for (i = 0; i < pollset->count; i++) {
        if (pollset->slots[i].fd == fd) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static cfw_status_t cfw_epoll_reserve(cfw_pollset_t *pollset, size_t needed)
{
    size_t new_capacity;
    cfw_epoll_slot_t *new_slots;

    if (needed <= pollset->capacity) {
        return CFW_OK;
    }

    new_capacity = pollset->capacity == 0 ? 8 : pollset->capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    new_slots = (cfw_epoll_slot_t *)realloc(
        pollset->slots, new_capacity * sizeof(*new_slots));
    if (new_slots == NULL) {
        return CFW_ERROR_NOMEM;
    }
    pollset->slots = new_slots;
    pollset->capacity = new_capacity;
    return CFW_OK;
}

cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset)
{
    int epfd;
    cfw_pollset_t *set;

    if (pollset == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return CFW_ERROR_UNAVAILABLE;
    }

    set = (cfw_pollset_t *)calloc(1, sizeof(*set));
    if (set == NULL) {
        close(epfd);
        return CFW_ERROR_NOMEM;
    }
    set->epfd = epfd;
    *pollset = set;
    return CFW_OK;
}

void cfw_pollset_destroy(cfw_pollset_t *pollset)
{
    size_t i;

    if (pollset == NULL) {
        return;
    }
    for (i = 0; i < pollset->count; i++) {
        free(pollset->slots[i].record);
    }
    free(pollset->slots);
    close(pollset->epfd);
    free(pollset);
}

cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset, intptr_t handle,
                             uint32_t events, void *token)
{
    cfw_epoll_record_t *record;
    struct epoll_event ev;
    cfw_status_t status;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle) ||
        !cfw_pollset_level_is_valid(events) ||
        cfw_epoll_find(pollset, (int)handle) >= 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_epoll_reserve(pollset, pollset->count + 1);
    if (status != CFW_OK) {
        return status;
    }

    record = (cfw_epoll_record_t *)malloc(sizeof(*record));
    if (record == NULL) {
        return CFW_ERROR_NOMEM;
    }
    record->handle = handle;
    record->token = token;

    memset(&ev, 0, sizeof(ev));
    ev.events = cfw_cfw_to_epoll(events);
    ev.data.ptr = record;

    if (epoll_ctl(pollset->epfd, EPOLL_CTL_ADD, (int)handle, &ev) != 0) {
        free(record);
        if (errno == EEXIST) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        return CFW_ERROR_IO;
    }

    pollset->slots[pollset->count].fd = (int)handle;
    pollset->slots[pollset->count].record = record;
    pollset->count++;
    return CFW_OK;
}

cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset, intptr_t handle,
                                uint32_t events, void *token)
{
    ssize_t index;
    cfw_epoll_record_t *record;
    struct epoll_event ev;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle) ||
        !cfw_pollset_level_is_valid(events)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_epoll_find(pollset, (int)handle);
    if (index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    record = (cfw_epoll_record_t *)malloc(sizeof(*record));
    if (record == NULL) {
        return CFW_ERROR_NOMEM;
    }
    record->handle = handle;
    record->token = token;

    memset(&ev, 0, sizeof(ev));
    ev.events = cfw_cfw_to_epoll(events);
    ev.data.ptr = record;

    if (epoll_ctl(pollset->epfd, EPOLL_CTL_MOD, (int)handle, &ev) != 0) {
        free(record);
        return CFW_ERROR_IO;
    }

    free(pollset->slots[index].record);
    pollset->slots[index].record = record;
    return CFW_OK;
}

cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset, intptr_t handle)
{
    ssize_t index;
    size_t i;
    struct epoll_event ev;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_epoll_find(pollset, (int)handle);
    if (index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memset(&ev, 0, sizeof(ev));
    if (epoll_ctl(pollset->epfd, EPOLL_CTL_DEL, (int)handle, &ev) != 0) {
        if (errno == ENOENT || errno == EBADF) {
            /* Kernel already dropped it (e.g. fd closed); still clean up. */
        } else {
            return CFW_ERROR_IO;
        }
    }

    free(pollset->slots[index].record);
    for (i = (size_t)index; i + 1 < pollset->count; i++) {
        pollset->slots[i] = pollset->slots[i + 1];
    }
    pollset->count--;
    return CFW_OK;
}

cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                              const cfw_timepoint_t *deadline,
                              cfw_poll_event_t *events, size_t max_events,
                              size_t *count)
{
    struct epoll_event *ready;
    int timeout_ms;
    int result;
    size_t i;
    cfw_status_t status;

    if (pollset == NULL || events == NULL || count == NULL ||
        max_events == 0 || max_events > INT32_MAX ||
        max_events > SIZE_MAX / sizeof(*ready)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_pollset_timeout_ms(deadline, &timeout_ms);
    if (status != CFW_OK) {
        return status;
    }

    ready = (struct epoll_event *)malloc(max_events * sizeof(*ready));
    if (ready == NULL) {
        return CFW_ERROR_NOMEM;
    }

    for (;;) {
        result = epoll_wait(pollset->epfd, ready, (int)max_events,
                            timeout_ms);
        if (result >= 0) {
            break;
        }
        if (errno == EINTR) {
            status = cfw_pollset_timeout_ms(deadline, &timeout_ms);
            if (status != CFW_OK) {
                free(ready);
                return status;
            }
            continue;
        }
        free(ready);
        return CFW_ERROR_IO;
    }

    if (result == 0) {
        free(ready);
        *count = 0;
        return CFW_ERROR_TIMEOUT;
    }

    {
        size_t produced = 0;
        for (i = 0; i < (size_t)result && produced < max_events; i++) {
            cfw_epoll_record_t *record =
                (cfw_epoll_record_t *)ready[i].data.ptr;
            if (record == NULL) {
                continue;
            }
            events[produced].handle = record->handle;
            events[produced].events = cfw_epoll_to_cfw(ready[i].events);
            events[produced].token = record->token;
            produced++;
        }
        *count = produced;
    }
    free(ready);
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* poll() fallback backend (other POSIX)                               */
/* ------------------------------------------------------------------ */

#elif defined(CFW_POLLF_HAVE_POLL)

typedef struct cfw_poll_entry {
    intptr_t handle;
    short poll_events;
    void *token;
} cfw_poll_entry_t;

struct cfw_pollset {
    struct pollfd *pfds;
    cfw_poll_entry_t *entries;
    size_t count;
    size_t capacity;
};

static short cfw_events_to_poll(uint32_t events)
{
    short mask = 0;
    if ((events & CFW_POLL_IN) != 0) {
        mask |= POLLIN;
    }
    if ((events & CFW_POLL_OUT) != 0) {
        mask |= POLLOUT;
    }
    return mask;
}

static uint32_t cfw_poll_to_events(short revents)
{
    uint32_t events = 0;
    if ((revents & POLLIN) != 0) {
        events |= CFW_POLL_IN;
    }
    if ((revents & POLLOUT) != 0) {
        events |= CFW_POLL_OUT;
    }
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        events |= CFW_POLL_ERR;
    }
    if ((revents & POLLHUP) != 0) {
        events |= CFW_POLL_HUP;
    }
    return events;
}

static cfw_status_t cfw_poll_reserve(cfw_pollset_t *pollset, size_t needed)
{
    size_t new_capacity;
    struct pollfd *new_pfds;
    cfw_poll_entry_t *new_entries;

    if (needed <= pollset->capacity) {
        return CFW_OK;
    }

    new_capacity = pollset->capacity == 0 ? 8 : pollset->capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    new_entries = (cfw_poll_entry_t *)realloc(
        pollset->entries, new_capacity * sizeof(*new_entries));
    if (new_entries == NULL) {
        return CFW_ERROR_NOMEM;
    }
    pollset->entries = new_entries;

    new_pfds = (struct pollfd *)realloc(pollset->pfds,
                                        new_capacity * sizeof(*new_pfds));
    if (new_pfds == NULL) {
        return CFW_ERROR_NOMEM;
    }
    pollset->pfds = new_pfds;
    pollset->capacity = new_capacity;
    return CFW_OK;
}

static ssize_t cfw_poll_index_of(const cfw_pollset_t *pollset,
                                 intptr_t handle)
{
    size_t i;
    for (i = 0; i < pollset->count; i++) {
        if (pollset->entries[i].handle == handle) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static cfw_status_t cfw_poll_register(cfw_pollset_t *pollset,
                                      intptr_t handle, uint32_t events,
                                      void *token, int must_exist)
{
    ssize_t index;
    cfw_poll_entry_t *entry;
    cfw_status_t status;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle) ||
        !cfw_pollset_level_is_valid(events)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_poll_index_of(pollset, handle);
    if (must_exist && index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (!must_exist && index >= 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (index < 0) {
        status = cfw_poll_reserve(pollset, pollset->count + 1);
        if (status != CFW_OK) {
            return status;
        }
        index = (ssize_t)pollset->count++;
    }

    entry = &pollset->entries[index];
    entry->handle = handle;
    entry->poll_events = cfw_events_to_poll(events);
    entry->token = token;

    pollset->pfds[index].fd = (int)handle;
    pollset->pfds[index].events = entry->poll_events;
    pollset->pfds[index].revents = 0;
    return CFW_OK;
}

cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset)
{
    cfw_pollset_t *set;

    if (pollset == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    set = (cfw_pollset_t *)calloc(1, sizeof(*set));
    if (set == NULL) {
        return CFW_ERROR_NOMEM;
    }
    *pollset = set;
    return CFW_OK;
}

void cfw_pollset_destroy(cfw_pollset_t *pollset)
{
    if (pollset == NULL) {
        return;
    }
    free(pollset->pfds);
    free(pollset->entries);
    free(pollset);
}

cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset, intptr_t handle,
                             uint32_t events, void *token)
{
    return cfw_poll_register(pollset, handle, events, token, 0);
}

cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset, intptr_t handle,
                                uint32_t events, void *token)
{
    return cfw_poll_register(pollset, handle, events, token, 1);
}

cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset, intptr_t handle)
{
    ssize_t index;
    size_t i;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_poll_index_of(pollset, handle);
    if (index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    for (i = (size_t)index; i + 1 < pollset->count; i++) {
        pollset->entries[i] = pollset->entries[i + 1];
        pollset->pfds[i] = pollset->pfds[i + 1];
    }
    pollset->count--;
    return CFW_OK;
}

cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                              const cfw_timepoint_t *deadline,
                              cfw_poll_event_t *events, size_t max_events,
                              size_t *count)
{
    int timeout_ms;
    nfds_t nfds;
    int ready;
    size_t i;
    size_t produced;
    cfw_status_t status;

    if (pollset == NULL || events == NULL || count == NULL ||
        max_events == 0 || max_events > INT32_MAX) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_pollset_timeout_ms(deadline, &timeout_ms);
    if (status != CFW_OK) {
        return status;
    }

    for (;;) {
        nfds = (nfds_t)pollset->count;
        ready = poll(pollset->pfds, nfds, timeout_ms);
        if (ready >= 0) {
            break;
        }
        if (errno == EINTR) {
            status = cfw_pollset_timeout_ms(deadline, &timeout_ms);
            if (status != CFW_OK) {
                return status;
            }
            continue;
        }
        return CFW_ERROR_IO;
    }

    if (ready == 0) {
        *count = 0;
        return CFW_ERROR_TIMEOUT;
    }

    produced = 0;
    for (i = 0; i < pollset->count && produced < max_events; i++) {
        short revents = pollset->pfds[i].revents;
        if (revents == 0) {
            continue;
        }
        events[produced].handle = pollset->entries[i].handle;
        events[produced].events = cfw_poll_to_events(revents);
        events[produced].token = pollset->entries[i].token;
        produced++;
    }
    *count = produced;
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* No backend                                                          */
/* ------------------------------------------------------------------ */

#else /* CFW_POLLF_HAVE_NONE */

cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset)
{
    (void)pollset;
    return CFW_ERROR_UNAVAILABLE;
}

void cfw_pollset_destroy(cfw_pollset_t *pollset)
{
    (void)pollset;
}

cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset, intptr_t handle,
                             uint32_t events, void *token)
{
    (void)pollset;
    (void)handle;
    (void)events;
    (void)token;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset, intptr_t handle,
                                uint32_t events, void *token)
{
    (void)pollset;
    (void)handle;
    (void)events;
    (void)token;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset, intptr_t handle)
{
    (void)pollset;
    (void)handle;
    return CFW_ERROR_UNAVAILABLE;
}

cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                              const cfw_timepoint_t *deadline,
                              cfw_poll_event_t *events, size_t max_events,
                              size_t *count)
{
    (void)pollset;
    (void)deadline;
    (void)events;
    (void)max_events;
    (void)count;
    return CFW_ERROR_UNAVAILABLE;
}

#endif
