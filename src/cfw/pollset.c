/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/pollset.h>

#include <cfw/checked_arithmetic.h>
#include <cfw/time.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* CFW_POLLF_BACKEND is a test-only override accepting epoll, kqueue, poll,
 * or none. It is never defined by the build; backend fixtures use it to
 * compile paths that are not selected by capability detection on the host. */
#if defined(CFW_POLLF_BACKEND)
    #define CFW_POLLF_BACKEND_epoll 1
    #define CFW_POLLF_BACKEND_kqueue 2
    #define CFW_POLLF_BACKEND_poll 3
    #define CFW_POLLF_BACKEND_none 4
    #define CFW_POLLF_BACKEND_CAT_I(a, b) a##b
    #define CFW_POLLF_BACKEND_CAT(a, b) CFW_POLLF_BACKEND_CAT_I(a, b)
    #define CFW_POLLF_BACKEND_VALUE \
        CFW_POLLF_BACKEND_CAT(CFW_POLLF_BACKEND_, CFW_POLLF_BACKEND)
    #if CFW_POLLF_BACKEND_VALUE != CFW_POLLF_BACKEND_epoll && \
        CFW_POLLF_BACKEND_VALUE != CFW_POLLF_BACKEND_kqueue && \
        CFW_POLLF_BACKEND_VALUE != CFW_POLLF_BACKEND_poll && \
        CFW_POLLF_BACKEND_VALUE != CFW_POLLF_BACKEND_none
        #error "CFW_POLLF_BACKEND must be one of: epoll, kqueue, poll, none"
    #endif
#endif

#if defined(CFW_POLLF_BACKEND)
    #if CFW_POLLF_BACKEND_VALUE == CFW_POLLF_BACKEND_epoll
        #include <sys/epoll.h>
        #include <unistd.h>
        #define CFW_POLLF_HAVE_EPOLL 1
    #elif CFW_POLLF_BACKEND_VALUE == CFW_POLLF_BACKEND_kqueue
        #include <fcntl.h>
        #include <sys/event.h>
        #include <sys/time.h>
        #include <time.h>
        #include <unistd.h>
        #define CFW_POLLF_HAVE_KQUEUE 1
    #elif CFW_POLLF_BACKEND_VALUE == CFW_POLLF_BACKEND_poll
        #include <poll.h>
        #include <unistd.h>
        #define CFW_POLLF_HAVE_POLL 1
    #else
        #define CFW_POLLF_HAVE_NONE 1
    #endif
#elif CFW_HAVE_EPOLL
    #include <sys/epoll.h>
    #include <unistd.h>
    #define CFW_POLLF_HAVE_EPOLL 1
#elif CFW_HAVE_KQUEUE
    #include <fcntl.h>
    #include <sys/event.h>
    #include <sys/time.h>
    #include <time.h>
    #include <unistd.h>
    #define CFW_POLLF_HAVE_KQUEUE 1
#elif CFW_HAVE_POLL
    #include <poll.h>
    #include <unistd.h>
    #define CFW_POLLF_HAVE_POLL 1
#else
    /* IOCP backend is designed but not implemented (ADR 0011). */
    #define CFW_POLLF_HAVE_NONE 1
#endif

#if defined(CFW_POLLF_BACKEND)
    #undef CFW_POLLF_BACKEND_epoll
    #undef CFW_POLLF_BACKEND_kqueue
    #undef CFW_POLLF_BACKEND_poll
    #undef CFW_POLLF_BACKEND_none
    #undef CFW_POLLF_BACKEND_CAT_I
    #undef CFW_POLLF_BACKEND_CAT
    #undef CFW_POLLF_BACKEND_VALUE
#endif

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* The none backend validates nothing; these helpers serve the real
 * backends only. */
#if defined(CFW_POLLF_HAVE_EPOLL) || defined(CFW_POLLF_HAVE_KQUEUE) || \
    defined(CFW_POLLF_HAVE_POLL)
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
#endif /* real backends */

/* epoll and poll() wait on millisecond timeouts; kqueue computes its own
 * relative timespec and the none backend never waits. */
#if defined(CFW_POLLF_HAVE_EPOLL) || defined(CFW_POLLF_HAVE_POLL)
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
#endif /* epoll || poll */

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
    struct epoll_event *ready;
    size_t ready_cap;
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
    free(pollset->ready);
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

    record = pollset->slots[index].record;

    memset(&ev, 0, sizeof(ev));
    ev.events = cfw_cfw_to_epoll(events);
    ev.data.ptr = record;

    if (epoll_ctl(pollset->epfd, EPOLL_CTL_MOD, (int)handle, &ev) != 0) {
        return CFW_ERROR_IO;
    }

    record->token = token;
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

    if (pollset->ready_cap < max_events) {
        struct epoll_event *grown =
            (struct epoll_event *)realloc(pollset->ready,
                                          max_events * sizeof(*grown));
        if (grown == NULL) {
            return CFW_ERROR_NOMEM;
        }
        pollset->ready = grown;
        pollset->ready_cap = max_events;
    }
    ready = pollset->ready;

    for (;;) {
        result = epoll_wait(pollset->epfd, ready, (int)max_events,
                            timeout_ms);
        if (result >= 0) {
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

    if (result == 0) {
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
    return CFW_OK;
}

/* ------------------------------------------------------------------ */
/* kqueue backend (macOS/BSD, ADR 0052)                                */
/* ------------------------------------------------------------------ */

#elif defined(CFW_POLLF_HAVE_KQUEUE)

typedef struct cfw_kqueue_record {
    intptr_t handle;
    void *token;
    int want_read;
    int want_write;
    uint32_t ready;
} cfw_kqueue_record_t;

struct cfw_pollset {
    int kqfd;
    cfw_kqueue_record_t **records;
    size_t count;
    size_t capacity;
    struct kevent *ready;
    size_t ready_cap;
};

static void cfw_kqueue_evset(struct kevent *kev, intptr_t handle,
                             int16_t filter, uint16_t flags, void *udata)
{
    EV_SET(kev, handle, filter, flags, 0, 0, udata);
}

static ssize_t cfw_kqueue_find(const cfw_pollset_t *pollset,
                               intptr_t handle)
{
    size_t i;

    for (i = 0; i < pollset->count; i++) {
        if (pollset->records[i]->handle == handle) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static cfw_status_t cfw_kqueue_reserve(cfw_pollset_t *pollset,
                                       size_t needed)
{
    size_t new_capacity;
    cfw_kqueue_record_t **new_records;

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

    new_records = (cfw_kqueue_record_t **)realloc(
        pollset->records, new_capacity * sizeof(*new_records));
    if (new_records == NULL) {
        return CFW_ERROR_NOMEM;
    }
    pollset->records = new_records;
    pollset->capacity = new_capacity;
    return CFW_OK;
}

/* Apply a changelist synchronously (nevents == 0 returns after the
 * kernel has processed every change). */
static cfw_status_t cfw_kqueue_apply(cfw_pollset_t *pollset,
                                     struct kevent *changes, int nchanges)
{
    if (kevent(pollset->kqfd, changes, nchanges, NULL, 0, NULL) != 0) {
        if (errno == ENOMEM || errno == EAGAIN) {
            return CFW_ERROR_NOMEM;
        }
        return CFW_ERROR_IO;
    }
    return CFW_OK;
}

static cfw_status_t cfw_kqueue_register(cfw_pollset_t *pollset,
                                        cfw_kqueue_record_t *record,
                                        int want_read, int want_write)
{
    struct kevent changes[2];
    int nchanges = 0;
    cfw_status_t status;

    if (want_read) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_READ, EV_ADD | EV_ENABLE,
                         record);
    }
    if (want_write) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_WRITE, EV_ADD | EV_ENABLE,
                         record);
    }

    status = cfw_kqueue_apply(pollset, changes, nchanges);
    if (status != CFW_OK) {
        /* Roll back any filter that may have been applied. */
        for (int i = 0; i < nchanges; i++) {
            changes[i].flags = EV_DELETE;
        }
        (void)cfw_kqueue_apply(pollset, changes, nchanges);
        return status;
    }
    return CFW_OK;
}

static void cfw_kqueue_unregister(cfw_pollset_t *pollset,
                                  cfw_kqueue_record_t *record)
{
    struct kevent changes[2];
    int nchanges = 0;

    if (record->want_read) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_READ, EV_DELETE, record);
    }
    if (record->want_write) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_WRITE, EV_DELETE, record);
    }
    if (nchanges > 0) {
        (void)cfw_kqueue_apply(pollset, changes, nchanges);
    }
}

static cfw_status_t cfw_kqueue_change_filters(cfw_pollset_t *pollset,
                                              cfw_kqueue_record_t *record,
                                              int want_read,
                                              int want_write)
{
    struct kevent changes[4];
    int nchanges = 0;

    if (record->want_read && !want_read) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_READ, EV_DELETE, record);
    }
    if (record->want_write && !want_write) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_WRITE, EV_DELETE, record);
    }
    if (!record->want_read && want_read) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_READ, EV_ADD | EV_ENABLE,
                         record);
    }
    if (!record->want_write && want_write) {
        cfw_kqueue_evset(&changes[nchanges++], record->handle,
                         EVFILT_WRITE, EV_ADD | EV_ENABLE,
                         record);
    }
    if (nchanges == 0) {
        return CFW_OK;
    }
    return cfw_kqueue_apply(pollset, changes, nchanges);
}

cfw_status_t cfw_pollset_create(cfw_pollset_t **pollset)
{
    int kqfd;
    cfw_pollset_t *set;

    if (pollset == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    kqfd = kqueue();
    if (kqfd < 0) {
        return CFW_ERROR_UNAVAILABLE;
    }
    (void)fcntl(kqfd, F_SETFD, FD_CLOEXEC);

    set = (cfw_pollset_t *)calloc(1, sizeof(*set));
    if (set == NULL) {
        close(kqfd);
        return CFW_ERROR_NOMEM;
    }
    set->kqfd = kqfd;
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
        free(pollset->records[i]);
    }
    free(pollset->records);
    free(pollset->ready);
    close(pollset->kqfd);
    free(pollset);
}

cfw_status_t cfw_pollset_add(cfw_pollset_t *pollset, intptr_t handle,
                             uint32_t events, void *token)
{
    cfw_kqueue_record_t *record;
    cfw_status_t status;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle) ||
        !cfw_pollset_level_is_valid(events) ||
        cfw_kqueue_find(pollset, handle) >= 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    status = cfw_kqueue_reserve(pollset, pollset->count + 1);
    if (status != CFW_OK) {
        return status;
    }

    record = (cfw_kqueue_record_t *)calloc(1, sizeof(*record));
    if (record == NULL) {
        return CFW_ERROR_NOMEM;
    }
    record->handle = handle;
    record->token = token;
    record->want_read = (events & CFW_POLL_IN) != 0;
    record->want_write = (events & CFW_POLL_OUT) != 0;
    record->ready = 0;

    status = cfw_kqueue_register(pollset, record, record->want_read,
                                 record->want_write);
    if (status != CFW_OK) {
        free(record);
        return status;
    }

    pollset->records[pollset->count++] = record;
    return CFW_OK;
}

cfw_status_t cfw_pollset_modify(cfw_pollset_t *pollset, intptr_t handle,
                                uint32_t events, void *token)
{
    ssize_t index;
    cfw_kqueue_record_t *record;
    int want_read;
    int want_write;
    cfw_status_t status;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle) ||
        !cfw_pollset_level_is_valid(events)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_kqueue_find(pollset, handle);
    if (index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    record = pollset->records[index];
    want_read = (events & CFW_POLL_IN) != 0;
    want_write = (events & CFW_POLL_OUT) != 0;

    status = cfw_kqueue_change_filters(pollset, record, want_read,
                                       want_write);
    if (status != CFW_OK) {
        return status;
    }

    record->want_read = want_read;
    record->want_write = want_write;
    record->token = token;
    return CFW_OK;
}

cfw_status_t cfw_pollset_remove(cfw_pollset_t *pollset, intptr_t handle)
{
    ssize_t index;

    if (pollset == NULL || !cfw_pollset_handle_is_valid(handle)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    index = cfw_kqueue_find(pollset, handle);
    if (index < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    cfw_kqueue_unregister(pollset, pollset->records[index]);
    free(pollset->records[index]);
    pollset->records[index] = pollset->records[pollset->count - 1];
    pollset->count--;
    return CFW_OK;
}

static uint32_t cfw_kqueue_to_cfw(int16_t filter, uint32_t kev_flags)
{
    uint32_t events = 0;

    if ((kev_flags & EV_ERROR) != 0) {
        events |= CFW_POLL_ERR;
    }
    /* READ/WRITE readiness is the delivery of the matching filter
     * event; NOTE_* fflags are filter-specific hints, not readiness bits. */
    if (filter == EVFILT_READ) {
        events |= CFW_POLL_IN;
    } else if (filter == EVFILT_WRITE) {
        events |= CFW_POLL_OUT;
    }
    if ((kev_flags & EV_EOF) != 0) {
        events |= CFW_POLL_HUP;
    }
    return events;
}

cfw_status_t cfw_pollset_wait(cfw_pollset_t *pollset,
                              const cfw_timepoint_t *deadline,
                              cfw_poll_event_t *events, size_t max_events,
                              size_t *count)
{
    struct kevent *ready;
    struct timespec ts;
    struct timespec *timeout;
    int nready;
    size_t i;
    size_t produced;
    cfw_status_t status;

    if (pollset == NULL || events == NULL || count == NULL ||
        max_events == 0 || max_events > INT32_MAX ||
        max_events > SIZE_MAX / sizeof(*ready)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (pollset->ready_cap < max_events) {
        struct kevent *grown =
            (struct kevent *)realloc(pollset->ready,
                                     max_events * sizeof(*grown));
        if (grown == NULL) {
            return CFW_ERROR_NOMEM;
        }
        pollset->ready = grown;
        pollset->ready_cap = max_events;
    }
    ready = pollset->ready;

    /* kqueue timeouts are relative; recompute on EINTR like the other
     * backends. */
    timeout = NULL;
    memset(&ts, 0, sizeof(ts));
    if (deadline != NULL) {
        cfw_timepoint_t now;
        cfw_duration_t remaining;
        int64_t rem_ns;

        status = cfw_time_now(&now);
        if (status != CFW_OK) {
            return status;
        }
        status = cfw_timepoint_diff(deadline, &now, &remaining);
        if (status != CFW_OK) {
            return status;
        }
        rem_ns = remaining.nanoseconds;
        if (rem_ns < 0) {
            rem_ns = 0;
        }
        ts.tv_sec = (time_t)(rem_ns / INT64_C(1000000000));
        ts.tv_nsec = (long)(rem_ns % INT64_C(1000000000));
        timeout = &ts;
    }

    for (;;) {
        nready = kevent(pollset->kqfd, NULL, 0, ready, (int)max_events,
                        timeout);
        if (nready >= 0) {
            break;
        }
        if (errno == EINTR) {
            if (deadline != NULL) {
                cfw_timepoint_t now;
                cfw_duration_t remaining;
                int64_t rem_ns;

                status = cfw_time_now(&now);
                if (status != CFW_OK) {
                    return status;
                }
                status = cfw_timepoint_diff(deadline, &now, &remaining);
                if (status != CFW_OK) {
                    return status;
                }
                rem_ns = remaining.nanoseconds;
                if (rem_ns < 0) {
                    rem_ns = 0;
                }
                ts.tv_sec = (time_t)(rem_ns / INT64_C(1000000000));
                ts.tv_nsec = (long)(rem_ns % INT64_C(1000000000));
            }
            continue;
        }
        return CFW_ERROR_IO;
    }

    /* Merge per-filter readiness into per-handle records, then emit one
     * event per ready handle. Merged events cannot exceed nready. */
    for (i = 0; i < (size_t)nready; i++) {
        cfw_kqueue_record_t *record =
            (cfw_kqueue_record_t *)ready[i].udata;
        if (record == NULL) {
            continue;
        }
        record->ready |= cfw_kqueue_to_cfw(ready[i].filter, ready[i].flags);
    }

    if (nready == 0) {
        *count = 0;
        return CFW_ERROR_TIMEOUT;
    }

    produced = 0;
    for (i = 0; i < pollset->count && produced < max_events; i++) {
        cfw_kqueue_record_t *record = pollset->records[i];
        if (record->ready == 0) {
            continue;
        }
        events[produced].handle = record->handle;
        events[produced].events = record->ready;
        events[produced].token = record->token;
        record->ready = 0;
        produced++;
    }
    *count = produced;
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
