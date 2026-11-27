/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"

#include <cfw/net.h>

#if CFW_PLATFORM_WINDOWS

/* Winsock adapter is a later phase; provide unavailable stubs so the
 * translation unit still builds on Windows. */

cfw_status_t cfw_addr_from_str(cfw_addr_t *a, const char *t, uint16_t p)
{ (void)a; (void)t; (void)p; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_addr_resolve(cfw_addr_t *a, const char *h, uint16_t p,
                              cfw_addr_family_t f)
{ (void)a; (void)h; (void)p; (void)f; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_addr_to_str(const cfw_addr_t *a, char *b, size_t c)
{ (void)a; (void)b; (void)c; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_addr_port(const cfw_addr_t *a, uint16_t *p)
{ (void)a; (void)p; return CFW_ERROR_UNAVAILABLE; }
int cfw_addr_is_ipv4(const cfw_addr_t *a) { (void)a; return 0; }
int cfw_addr_is_ipv6(const cfw_addr_t *a) { (void)a; return 0; }
cfw_status_t cfw_addr_any(uint16_t p, int v6, cfw_addr_t *a)
{ (void)p; (void)v6; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_socket(int d, int t, int p, cfw_fd_t *f)
{ (void)d; (void)t; (void)p; (void)f; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *f, int e)
{ (void)f; (void)e; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_bind(cfw_fd_t *f, const cfw_addr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_listen(cfw_fd_t *f, int b)
{ (void)f; (void)b; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_accept(cfw_fd_t *f, cfw_addr_t *p, cfw_fd_t *o)
{ (void)f; (void)p; (void)o; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_connect(cfw_fd_t *f, const cfw_addr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_local_addr(cfw_fd_t *f, cfw_addr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_peer_addr(cfw_fd_t *f, cfw_addr_t *a)
{ (void)f; (void)a; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_send(cfw_fd_t *f, const void *d, size_t s, size_t *n)
{ (void)f; (void)d; (void)s; (void)n; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_recv(cfw_fd_t *f, void *b, size_t s, size_t *n)
{ (void)f; (void)b; (void)s; (void)n; return CFW_ERROR_UNAVAILABLE; }
cfw_status_t cfw_net_so_error(cfw_fd_t *f, int *e)
{ (void)f; (void)e; return CFW_ERROR_UNAVAILABLE; }

#else /* POSIX */

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

/* sockaddr_storage must fit in the public cfw_addr_t storage on every
 * supported platform (128 bytes on POSIX and Windows). */
_Static_assert(sizeof(struct sockaddr_storage) <=
                   CFW_ADDR_STORAGE_CAPACITY,
               "sockaddr_storage does not fit cfw_addr_t storage");

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

static int cfw_addr_valid(const cfw_addr_t *addr)
{
    if (addr == NULL || addr->length == 0 ||
        addr->length > CFW_ADDR_STORAGE_CAPACITY) {
        return 0;
    }
    return 1;
}

static const struct sockaddr *cfw_addr_sockaddr(const cfw_addr_t *addr)
{
    return (const struct sockaddr *)(const void *)addr->storage.opaque;
}

/* Copies a platform sockaddr into the public address value. */
static cfw_status_t cfw_addr_store(cfw_addr_t *addr,
                                   const struct sockaddr *sa,
                                   socklen_t len)
{
    if (addr == NULL || sa == NULL || len == 0 ||
        (size_t)len > CFW_ADDR_STORAGE_CAPACITY) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(addr, 0, sizeof(*addr));
    memcpy(addr->storage.opaque, sa, (size_t)len);
    addr->length = (uint32_t)len;
    return CFW_OK;
}

/* Copies the public address value into a platform sockaddr_storage for
 * the syscall layer. Returns 0 on invalid input. */
static int cfw_addr_to_sockaddr(const cfw_addr_t *addr,
                                struct sockaddr_storage *storage,
                                socklen_t *len)
{
    if (!cfw_addr_valid(addr)) {
        return 0;
    }
    memset(storage, 0, sizeof(*storage));
    memcpy(storage, addr->storage.opaque, addr->length);
    *len = (socklen_t)addr->length;
    return 1;
}

cfw_status_t cfw_addr_from_str(cfw_addr_t *addr, const char *text,
                               uint16_t port)
{
    struct sockaddr_in in4;
    struct sockaddr_in6 in6;
    int is_v6;

    if (addr == NULL || text == NULL || text[0] == '\0') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    is_v6 = strchr(text, ':') != NULL;

    if (!is_v6) {
        memset(&in4, 0, sizeof(in4));
        if (inet_pton(AF_INET, text, &in4.sin_addr) != 1) {
            return CFW_ERROR_INVALID_ARGUMENT;
        }
        in4.sin_family = AF_INET;
        in4.sin_port = htons(port);
        return cfw_addr_store(addr, (const struct sockaddr *)&in4,
                              (socklen_t)sizeof(in4));
    }

    memset(&in6, 0, sizeof(in6));
    if (inet_pton(AF_INET6, text, &in6.sin6_addr) != 1) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(port);
    return cfw_addr_store(addr, (const struct sockaddr *)&in6,
                          (socklen_t)sizeof(in6));
}

cfw_status_t cfw_addr_resolve(cfw_addr_t *addr, const char *host,
                              uint16_t port, cfw_addr_family_t family)
{
    int af;

    if (addr == NULL || host == NULL || host[0] == '\0') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (family == CFW_ADDR_FAMILY_IPV4) {
        af = AF_INET;
    } else if (family == CFW_ADDR_FAMILY_IPV6) {
        af = AF_INET6;
    } else {
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
        hints.ai_family = af;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

        rc = getaddrinfo(host, port_str, &hints, &res);
        if (rc != 0) {
            return rc == EAI_NONAME ? CFW_ERROR_INVALID_ARGUMENT
                                    : CFW_ERROR_IO;
        }
        for (it = res; it != NULL; it = it->ai_next) {
            if (it->ai_addrlen <= CFW_ADDR_STORAGE_CAPACITY &&
                it->ai_family == af) {
                cfw_status_t st = cfw_addr_store(
                    addr, (const struct sockaddr *)it->ai_addr,
                    (socklen_t)it->ai_addrlen);
                freeaddrinfo(res);
                return st;
            }
        }
        freeaddrinfo(res);
        return CFW_ERROR_IO;
    }
#else
    (void)addr;
    (void)host;
    (void)port;
    (void)af;
    return CFW_ERROR_UNAVAILABLE;
#endif
}

cfw_status_t cfw_addr_to_str(const cfw_addr_t *addr, char *buffer,
                             size_t capacity)
{
    const struct sockaddr *sa;
    const char *result;
    int family;

    if (!cfw_addr_valid(addr) || buffer == NULL || capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    sa = cfw_addr_sockaddr(addr);
    family = sa->sa_family;

    if (family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)sa;
        result = inet_ntop(AF_INET, &in4->sin_addr, buffer,
                           (socklen_t)capacity);
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)sa;
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

cfw_status_t cfw_addr_port(const cfw_addr_t *addr, uint16_t *port)
{
    const struct sockaddr *sa;

    if (!cfw_addr_valid(addr) || port == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    sa = cfw_addr_sockaddr(addr);

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)sa;
        *port = ntohs(in4->sin_port);
        return CFW_OK;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)sa;
        *port = ntohs(in6->sin6_port);
        return CFW_OK;
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

int cfw_addr_is_ipv4(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->sa_family == AF_INET;
}

int cfw_addr_is_ipv6(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->sa_family == AF_INET6;
}

cfw_status_t cfw_addr_any(uint16_t port, int ipv6, cfw_addr_t *addr)
{
    struct sockaddr_in in4;
    struct sockaddr_in6 in6;

    if (addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    if (!ipv6) {
        memset(&in4, 0, sizeof(in4));
        in4.sin_family = AF_INET;
        in4.sin_addr.s_addr = htonl(INADDR_ANY);
        in4.sin_port = htons(port);
        return cfw_addr_store(addr, (const struct sockaddr *)&in4,
                              (socklen_t)sizeof(in4));
    }

    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_addr = in6addr_any;
    in6.sin6_port = htons(port);
    return cfw_addr_store(addr, (const struct sockaddr *)&in6,
                          (socklen_t)sizeof(in6));
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

cfw_status_t cfw_net_bind(cfw_fd_t *fd, const cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    socklen_t len;

    if (fd == NULL || !cfw_fd_valid(fd) ||
        !cfw_addr_to_sockaddr(addr, &storage, &len)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (bind((int)fd->handle, (const struct sockaddr *)&storage,
             len) != 0) {
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

cfw_status_t cfw_net_accept(cfw_fd_t *fd, cfw_addr_t *peer,
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
        cfw_status_t st = cfw_addr_store(peer, (const struct sockaddr *)&ss,
                                        len);
        if (st != CFW_OK) {
            close(raw);
            return st;
        }
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

cfw_status_t cfw_net_connect(cfw_fd_t *fd, const cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    socklen_t len;

    if (fd == NULL || !cfw_fd_valid(fd) ||
        !cfw_addr_to_sockaddr(addr, &storage, &len)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (connect((int)fd->handle, (const struct sockaddr *)&storage,
                len) != 0) {
        return cfw_net_errno(errno);
    }
    return CFW_OK;
}

cfw_status_t cfw_net_local_addr(cfw_fd_t *fd, cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    socklen_t len = sizeof(storage);

    if (fd == NULL || !cfw_fd_valid(fd) || addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getsockname((int)fd->handle, (struct sockaddr *)&storage,
                    &len) != 0) {
        return cfw_net_errno(errno);
    }
    return cfw_addr_store(addr, (const struct sockaddr *)&storage, len);
}

cfw_status_t cfw_net_peer_addr(cfw_fd_t *fd, cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    socklen_t len = sizeof(storage);

    if (fd == NULL || !cfw_fd_valid(fd) || addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getpeername((int)fd->handle, (struct sockaddr *)&storage,
                    &len) != 0) {
        return cfw_net_errno(errno);
    }
    return cfw_addr_store(addr, (const struct sockaddr *)&storage, len);
}

cfw_status_t cfw_net_send(cfw_fd_t *fd, const void *data, size_t size,
                          size_t *sent)
{
    ssize_t n;

    if (fd == NULL || !cfw_fd_valid(fd) || sent == NULL ||
        (size != 0 && data == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        *sent = 0;
        return CFW_OK;
    }

#if CFW_HAVE_MSG_NOSIGNAL
    n = send((int)fd->handle, data, size, MSG_NOSIGNAL);
#else
    n = send((int)fd->handle, data, size, 0);
#endif
    if (n > 0) {
        *sent = (size_t)n;
        return CFW_OK;
    }
    *sent = 0;
    if (n == 0) {
        return CFW_ERROR_IO;
    }
    return cfw_net_errno(errno);
}

cfw_status_t cfw_net_recv(cfw_fd_t *fd, void *buffer, size_t size,
                          size_t *received)
{
    ssize_t n;

    if (fd == NULL || !cfw_fd_valid(fd) || received == NULL ||
        (size != 0 && buffer == NULL)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        *received = 0;
        return CFW_OK;
    }

    n = recv((int)fd->handle, buffer, size, 0);
    if (n > 0) {
        *received = (size_t)n;
        return CFW_OK;
    }
    *received = 0;
    if (n == 0) {
        /* Orderly peer shutdown. */
        return CFW_OK;
    }
    return cfw_net_errno(errno);
}

cfw_status_t cfw_net_so_error(cfw_fd_t *fd, int *error)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (fd == NULL || !cfw_fd_valid(fd) || error == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getsockopt((int)fd->handle, SOL_SOCKET, SO_ERROR, &err,
                   &len) != 0) {
        return cfw_net_errno(errno);
    }
    *error = err;
    return CFW_OK;
}

#endif /* POSIX */
