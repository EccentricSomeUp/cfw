/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#if defined(__linux__) || defined(__ANDROID__)
    #define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#if defined(_WIN32)

/* Winsock adapter is a later phase; provide unavailable stubs so the
 * translation unit still builds on Windows. */

#include <cfw/fd.h>
#include <cfw/status.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cfw_sockaddr {
    unsigned char opaque[128];
    int len;
} cfw_sockaddr_t;

cfw_status_t cfw_addr_from_str(cfw_sockaddr_t *a, const char *t, uint16_t p)
{ (void)a; (void)t; (void)p; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_addr_to_str(const cfw_sockaddr_t *a, char *b, size_t c)
{ (void)a; (void)b; (void)c; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_addr_port(const cfw_sockaddr_t *a, uint16_t *p)
{ (void)a; (void)p; return CFW_ERROR_UNAVAILABLE; }
int cfw_addr_is_ipv4(const cfw_sockaddr_t *a) { (void)a; return 0; }
int cfw_addr_is_ipv6(const cfw_sockaddr_t *a) { (void)a; return 0; }
cfw_status_t cfw_addr_any(uint16_t p, int v6, cfw_sockaddr_t *a)
{ (void)p; (void)v6; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_socket(int d, int t, int p, cfw_fd_t *f)
{ (void)d; (void)t; (void)p; (void)f; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *f, int e)
{ (void)f; (void)e; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_bind(cfw_fd_t *f, const cfw_sockaddr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_listen(cfw_fd_t *f, int b)
{ (void)f; (void)b; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_accept(cfw_fd_t *f, cfw_sockaddr_t *p, cfw_fd_t *o)
{ (void)f; (void)p; (void)o; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_connect(cfw_fd_t *f, const cfw_sockaddr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_local_addr(cfw_fd_t *f, cfw_sockaddr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }

#else /* !_WIN32 */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

static cfw_status_t cfw_net_errno(int error)
{
    switch (error) {
    case 0:
        return CFW_OK;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return CFW_ERROR_WOULDBLOCK;
    case ENOMEM:
        return CFW_ERROR_NOMEM;
    case ETIMEDOUT:
        return CFW_ERROR_TIMEOUT;
    case EINPROGRESS:
        return CFW_ERROR_WOULDBLOCK;
    case EINVAL:
        return CFW_ERROR_INVALID_ARGUMENT;
    default:
        return CFW_ERROR_IO;
    }
}

static int cfw_addr_valid(const cfw_sockaddr_t *addr)
{
    if (addr == NULL || addr->len == 0 ||
        addr->len > sizeof(addr->storage)) {
        return 0;
    }
    return 1;
}

cfw_status_t cfw_addr_from_str(cfw_sockaddr_t *addr, const char *text,
                               uint16_t port)
{
    struct sockaddr_in *in4;
    struct sockaddr_in6 *in6;
    int is_v6;

    if (addr == NULL || text == NULL || text[0] == '\0') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    is_v6 = strchr(text, ':') != NULL;
    memset(addr, 0, sizeof(*addr));

    if (!is_v6) {
        in4 = (struct sockaddr_in *)&addr->storage;
        if (inet_pton(AF_INET, text, &in4->sin_addr) != 1) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        in4->sin_family = AF_INET;
        in4->sin_port = htons(port);
        addr->len = (socklen_t)sizeof(*in4);
        return CFW_OK;
    }

    in6 = (struct sockaddr_in6 *)&addr->storage;
    if (inet_pton(AF_INET6, text, &in6->sin6_addr) != 1) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(port);
    addr->len = (socklen_t)sizeof(*in6);
    return CFW_OK;
}

cfw_status_t cfw_addr_to_str(const cfw_sockaddr_t *addr, char *buffer,
                             size_t capacity)
{
    const char *result;
    int family;

    if (!cfw_addr_valid(addr) || buffer == NULL || capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    family = addr->storage.ss_family;
    if (family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)&addr->storage;
        result = inet_ntop(AF_INET, &in4->sin_addr, buffer,
                           (socklen_t)capacity);
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)&addr->storage;
        result = inet_ntop(AF_INET6, &in6->sin6_addr, buffer,
                           (socklen_t)capacity);
    } else {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (result == NULL) {
        return CFW_ERROR_IO;
    }
    return CFW_OK;
}

cfw_status_t cfw_addr_port(const cfw_sockaddr_t *addr, uint16_t *port)
{
    if (!cfw_addr_valid(addr) || port == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (addr->storage.ss_family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)&addr->storage;
        *port = ntohs(in4->sin_port);
        return CFW_OK;
    }
    if (addr->storage.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)&addr->storage;
        *port = ntohs(in6->sin6_port);
        return CFW_OK;
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

int cfw_addr_is_ipv4(const cfw_sockaddr_t *addr)
{
    return cfw_addr_valid(addr) && addr->storage.ss_family == AF_INET;
}

int cfw_addr_is_ipv6(const cfw_sockaddr_t *addr)
{
    return cfw_addr_valid(addr) && addr->storage.ss_family == AF_INET6;
}

cfw_status_t cfw_addr_any(uint16_t port, int ipv6, cfw_sockaddr_t *addr)
{
    struct sockaddr_in *in4;
    struct sockaddr_in6 *in6;

    if (addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(addr, 0, sizeof(*addr));

    if (!ipv6) {
        in4 = (struct sockaddr_in *)&addr->storage;
        in4->sin_family = AF_INET;
        in4->sin_addr.s_addr = htonl(INADDR_ANY);
        in4->sin_port = htons(port);
        addr->len = (socklen_t)sizeof(*in4);
        return CFW_OK;
    }

    in6 = (struct sockaddr_in6 *)&addr->storage;
    in6->sin6_family = AF_INET6;
    in6->sin6_addr = in6addr_any;
    in6->sin6_port = htons(port);
    addr->len = (socklen_t)sizeof(*in6);
    return CFW_OK;
}

cfw_status_t cfw_net_socket(int domain, int type, int protocol,
                            cfw_fd_t *fd)
{
    int raw;
    int flags = SOCK_CLOEXEC;

    if (fd == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#ifdef SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK;
#endif

    raw = socket(domain, type | flags, protocol);
    if (raw < 0) {
        return cfw_net_errno(errno);
    }

#ifdef SOCK_NONBLOCK
    fd->handle = raw;
#else
    fd->handle = raw;
    {
        int fl = fcntl(raw, F_GETFL, 0);
        if (fl < 0 || fcntl(raw, F_SETFL, fl | O_NONBLOCK) != 0) {
            close(raw);
            return cfw_net_errno(errno);
        }
    }
#endif
    return CFW_OK;
}

cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *fd, int enable)
{
    int fl;

    if (fd == NULL || !cfw_fd_valid(fd)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    fl = fcntl((int)fd->handle, F_GETFL, 0);
    if (fl < 0) {
        return cfw_net_errno(errno);
    }
    if (enable) {
        fl |= O_NONBLOCK;
    } else {
        fl &= ~O_NONBLOCK;
    }
    if (fcntl((int)fd->handle, F_SETFL, fl) != 0) {
        return cfw_net_errno(errno);
    }
    return CFW_OK;
}

cfw_status_t cfw_net_bind(cfw_fd_t *fd, const cfw_sockaddr_t *addr)
{
    if (fd == NULL || !cfw_fd_valid(fd) || !cfw_addr_valid(addr)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (bind((int)fd->handle, (const struct sockaddr *)&addr->storage,
             addr->len) != 0) {
        return cfw_net_errno(errno);
    }
    return CFW_OK;
}

cfw_status_t cfw_net_listen(cfw_fd_t *fd, int backlog)
{
    if (fd == NULL || !cfw_fd_valid(fd) || backlog < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (listen((int)fd->handle, backlog) != 0) {
        return cfw_net_errno(errno);
    }
    return CFW_OK;
}

cfw_status_t cfw_net_accept(cfw_fd_t *fd, cfw_sockaddr_t *peer,
                            cfw_fd_t *out)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int raw;

    if (fd == NULL || !cfw_fd_valid(fd) || out == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if defined(__linux__) || defined(__ANDROID__)
    raw = accept4((int)fd->handle, (struct sockaddr *)&ss, &len,
                  SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    raw = accept((int)fd->handle, (struct sockaddr *)&ss, &len);
    if (raw >= 0) {
        int fl = fcntl(raw, F_GETFL, 0);
        if (fl < 0 || fcntl(raw, F_SETFL, fl | O_NONBLOCK) != 0) {
            close(raw);
            return cfw_net_errno(errno);
        }
    }
#endif
    if (raw < 0) {
        return cfw_net_errno(errno);
    }

    if (peer != NULL) {
        memset(peer, 0, sizeof(*peer));
        memcpy(&peer->storage, &ss, len < sizeof(ss) ? len : sizeof(ss));
        peer->len = len;
    }

    out->handle = raw;
    return CFW_OK;
}

cfw_status_t cfw_net_connect(cfw_fd_t *fd, const cfw_sockaddr_t *addr)
{
    if (fd == NULL || !cfw_fd_valid(fd) || !cfw_addr_valid(addr)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (connect((int)fd->handle, (const struct sockaddr *)&addr->storage,
                addr->len) != 0) {
        return cfw_net_errno(errno);
    }
    return CFW_OK;
}

cfw_status_t cfw_net_local_addr(cfw_fd_t *fd, cfw_sockaddr_t *addr)
{
    socklen_t len;

    if (fd == NULL || !cfw_fd_valid(fd) || addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    len = sizeof(addr->storage);
    if (getsockname((int)fd->handle, (struct sockaddr *)&addr->storage,
                    &len) != 0) {
        return cfw_net_errno(errno);
    }
    addr->len = len;
    return CFW_OK;
}

#endif /* !_WIN32 */
