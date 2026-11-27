/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_TEST_LOOPBACK_H
#define CFW_TEST_LOOPBACK_H

/*
 * Portable test helpers for socket-driven unit tests. Included only by
 * translation units that use them, so nothing here may be relied on
 * elsewhere.
 *
 * cfw_test_socketpair emulates the POSIX socketpair(AF_UNIX) pattern
 * used throughout the suite with a connected TCP loopback pair built
 * from the public cfw_net_* API — it works identically on POSIX and
 * Windows (Winsock2). Both ends are nonblocking cfw_fd_t wrappers.
 */

#include "private/platform_detect.h"

#include <cfw/cfw.h>
#include <cfw/net.h>

#include <string.h>

#if CFW_PLATFORM_WINDOWS
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <sys/socket.h>
    #include <time.h>
    #include <unistd.h>
#endif

/* Portable sub-millisecond/millisecond sleep for retry loops. */
static inline void cfw_test_sleep_us(long usec)
{
#if CFW_PLATFORM_WINDOWS
    Sleep((DWORD)((usec + 999) / 1000));
#else
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    while (nanosleep(&ts, &ts) != 0) {
    }
#endif
}

/* Closes a raw descriptor value obtained from cfw_fd_get /
 * cfw_fd_release. */
static inline void cfw_test_close_raw(intptr_t handle)
{
#if CFW_PLATFORM_WINDOWS
    (void)closesocket((SOCKET)handle);
#else
    (void)close((int)handle);
#endif
}

/* Portable memmem (GNU extension, absent on Windows). */
static inline void *cfw_test_memmem(const void *haystack, size_t hlen,
                                    const void *needle, size_t nlen)
{
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    size_t i;

    if (nlen == 0) {
        return (void *)haystack;
    }
    if (hlen < nlen) {
        return NULL;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        if (memcmp(h + i, n, nlen) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}

/* Creates a connected, nonblocking loopback socket pair. On success
 * out[0] and out[1] own the two ends; on failure they are reset to
 * invalid wrappers and any partial state is released. */
static inline cfw_status_t cfw_test_socketpair(cfw_fd_t out[2])
{
    cfw_addr_t bind_addr;
    cfw_addr_t local_addr;
    cfw_addr_t peer_addr;
    cfw_fd_t listener;
    uint16_t port = 0;
    cfw_status_t st;

    cfw_fd_init(&out[0]);
    cfw_fd_init(&out[1]);
    cfw_fd_init(&listener);

    st = cfw_addr_any(0, 0, &bind_addr);
    if (st != CFW_OK) {
        return st;
    }
    st = cfw_net_socket(AF_INET, SOCK_STREAM, 0, &listener);
    if (st != CFW_OK) {
        return st;
    }
    st = cfw_net_bind(&listener, &bind_addr);
    if (st != CFW_OK) {
        goto fail;
    }
    st = cfw_net_listen(&listener, 4);
    if (st != CFW_OK) {
        goto fail;
    }
    st = cfw_net_local_addr(&listener, &local_addr);
    if (st != CFW_OK) {
        goto fail;
    }
    st = cfw_addr_port(&local_addr, &port);
    if (st != CFW_OK) {
        goto fail;
    }

    /* Blocking connect for determinism, then restore nonblocking. */
    st = cfw_addr_from_str(&peer_addr, "127.0.0.1", port);
    if (st != CFW_OK) {
        goto fail;
    }
    st = cfw_net_socket(AF_INET, SOCK_STREAM, 0, &out[0]);
    if (st != CFW_OK) {
        goto fail;
    }
    if (cfw_net_set_nonblocking(&out[0], 0) != CFW_OK) {
        goto fail;
    }
    st = cfw_net_connect(&out[0], &peer_addr);
    (void)cfw_net_set_nonblocking(&out[0], 1);
    if (st != CFW_OK) {
        goto fail;
    }
    st = cfw_net_accept(&listener, NULL, &out[1]);
    if (st != CFW_OK) {
        goto fail;
    }

    cfw_fd_close(&listener);
    return CFW_OK;

fail:
    cfw_fd_close(&out[0]);
    cfw_fd_close(&out[1]);
    cfw_fd_close(&listener);
    cfw_fd_init(&out[0]);
    cfw_fd_init(&out[1]);
    return st;
}

#endif /* CFW_TEST_LOOPBACK_H */
