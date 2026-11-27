/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "platform_detect.h"

#if CFW_PLATFORM_WINDOWS

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
cfw_status_t cfw_addr_resolve(cfw_sockaddr_t *a, const char *h, uint16_t p,
                              int f)
{ (void)a; (void)h; (void)p; (void)f; return CFW_ERROR_UNAVAILABLE; }
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

#else /* POSIX */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#if CFW_HAVE_TCP_NODELAY
#include <netinet/tcp.h>
#endif
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Suppresses SIGPIPE on platforms lacking MSG_NOSIGNAL (macOS, BSD);
 * send() then reports EPIPE instead of killing the process. */
static void cfw_net_set_nosigpipe(int raw)
{
#if !CFW_HAVE_MSG_NOSIGNAL && CFW_HAVE_SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(raw, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    (void)raw;
}

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

cfw_status_t cfw_addr_resolve(cfw_sockaddr_t *addr, const char *host,
                              uint16_t port, int family)
{
    if (addr == NULL || host == NULL || host[0] == '\0' ||
        (family != AF_INET && family != AF_INET6)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_HAVE_GETADDRINFO
    {
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *it;
        char port_str[8];
        int rc;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

        rc = getaddrinfo(host, port_str, &hints, &res);
        if (rc != 0) {
            return rc == EAI_NONAME ? CFW_ERROR_INVALID_ARGUMENT
                                    : CFW_ERROR_IO;
        }
        for (it = res; it != NULL; it = it->ai_next) {
            if (it->ai_addrlen <= sizeof(addr->storage) &&
                it->ai_family == family) {
                memcpy(&addr->storage, it->ai_addr, it->ai_addrlen);
                addr->len = (socklen_t)it->ai_addrlen;
                freeaddrinfo(res);
                return CFW_OK;
            }
        }
        freeaddrinfo(res);
        return CFW_ERROR_IO;
    }
#else
    (void)addr;
    (void)host;
    (void)port;
    (void)family;
    return CFW_ERROR_UNAVAILABLE;
#endif
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
    int flags = 0;

    if (fd == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

#if CFW_HAVE_SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK;
#endif
#if CFW_HAVE_SOCK_CLOEXEC
    flags |= SOCK_CLOEXEC;
#endif

    raw = socket(domain, type | flags, protocol);
    if (raw < 0) {
        return cfw_net_errno(errno);
    }

#if !CFW_HAVE_SOCK_CLOEXEC
    if (fcntl(raw, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(raw);
        errno = saved;
        return cfw_net_errno(errno);
    }
#endif

#if CFW_HAVE_SOCK_NONBLOCK
    fd->handle = raw;
#else
    {
        int fl = fcntl(raw, F_GETFL, 0);
        if (fl < 0 || fcntl(raw, F_SETFL, fl | O_NONBLOCK) != 0) {
            int saved = errno;
            close(raw);
            errno = saved;
            return cfw_net_errno(errno);
        }
    }
    fd->handle = raw;
#endif

    cfw_net_set_nosigpipe(raw);
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

#if CFW_HAVE_ACCEPT4
    int accept_flags = 0;
#if CFW_HAVE_SOCK_NONBLOCK
    accept_flags |= SOCK_NONBLOCK;
#endif
#if CFW_HAVE_SOCK_CLOEXEC
    accept_flags |= SOCK_CLOEXEC;
#endif
    raw = accept4((int)fd->handle, (struct sockaddr *)&ss, &len,
                  accept_flags);
#else
    raw = accept((int)fd->handle, (struct sockaddr *)&ss, &len);
#endif
    if (raw >= 0) {
#if !CFW_HAVE_ACCEPT4 || !CFW_HAVE_SOCK_NONBLOCK || !CFW_HAVE_SOCK_CLOEXEC
        int fl = fcntl(raw, F_GETFL, 0);
        int fdfl = fcntl(raw, F_GETFD, 0);
        if (fl < 0 || fdfl < 0 ||
            fcntl(raw, F_SETFL, fl | O_NONBLOCK) != 0 ||
            fcntl(raw, F_SETFD, fdfl | FD_CLOEXEC) != 0) {
            close(raw);
            return cfw_net_errno(errno);
        }
#endif
    }
    if (raw < 0) {
        return cfw_net_errno(errno);
    }

    if (peer != NULL) {
        memset(peer, 0, sizeof(*peer));
        memcpy(&peer->storage, &ss, len < sizeof(ss) ? len : sizeof(ss));
        peer->len = len;
    }

    cfw_net_set_nosigpipe(raw);
#if CFW_HAVE_TCP_NODELAY
    /* Disable Nagle on TCP connections: HTTP request/response
     * exchanges are small and latency-sensitive, and delayed-ACK
     * interaction otherwise stalls every other segment. */
    if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
        int one = 1;
        (void)setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, &one,
                         sizeof(one));
    }
#endif
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

#endif /* POSIX */
