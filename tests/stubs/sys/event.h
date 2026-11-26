/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TEST_STUB_SYS_EVENT_H
#define CFW_TEST_STUB_SYS_EVENT_H

/*
 * Compile-only kqueue fixture for hosts without <sys/event.h>. This header
 * provides just enough of the kqueue ABI for pollset.c to compile; it is not
 * a runtime implementation and is only used by cfw_pollset_kqueue_compile.
 */

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kevent {
    intptr_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    intptr_t data;
    void *udata;
};

#define EVFILT_READ (-1)
#define EVFILT_WRITE (-2)

#define EV_ADD 0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004

#define EV_ERROR 0x4000
#define EV_EOF 0x8000

#define EV_SET(kev, ident_value, filter_value, flags_value, fflags_value, \
              data_value, udata_value)                                      \
    do {                                                                    \
        (kev)->ident = (intptr_t)(ident_value);                             \
        (kev)->filter = (int16_t)(filter_value);                            \
        (kev)->flags = (uint16_t)(flags_value);                             \
        (kev)->fflags = (uint32_t)(fflags_value);                           \
        (kev)->data = (intptr_t)(data_value);                               \
        (kev)->udata = (void *)(udata_value);                               \
    } while (0)

int kqueue(void);
int kevent(int kqfd, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents,
           const struct timespec *timeout);

#ifdef __cplusplus
}
#endif

#endif /* CFW_TEST_STUB_SYS_EVENT_H */
