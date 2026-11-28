/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#include "private/platform_detect.h"
#include "private/net_resolve.h"
#include "private/winsock.h"

#include <cfw/net.h>

#if CFW_PLATFORM_WINDOWS

/* Winsock2 adapter (ADR 0072): readiness-based sockets over ws2_32.
 * SOCKET values live in the intptr_t cfw_fd_t handle; INVALID_SOCKET
 * ((SOCKET)-1) is the invalid value, matching CFW_FD_INVALID. */

#include <ws2tcpip.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sockaddr_storage must fit in the public cfw_addr_t storage on every
 * supported platform (128 bytes on POSIX and Windows). */
_Static_assert(sizeof(struct sockaddr_storage) <=
                   CFW_ADDR_STORAGE_CAPACITY,
               "sockaddr_storage does not fit cfw_addr_t storage");
_Static_assert(_Alignof(struct sockaddr_storage) <=
                   _Alignof(cfw_max_align_t),
               "sockaddr_storage alignment exceeds cfw_max_align_t");

static cfw_status_t cfw_net_wsa_error(void)
{
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK:
        return CFW_ERROR_WOULDBLOCK;
    case WSAETIMEDOUT:
        return CFW_ERROR_TIMEOUT;
    case WSAEINPROGRESS:
        return CFW_ERROR_WOULDBLOCK;
    case WSAENOBUFS:
    case WSAEMFILE:
        return CFW_ERROR_NOMEM;
    case WSAEINVAL:
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

static const struct sockaddr_storage *cfw_addr_sockaddr(
    const cfw_addr_t *addr)
{
    /* Read through the union's aligned member: opaque is unsigned char
     * storage, so casting it directly trips -Wcast-align when callers
     * convert to sockaddr_in/sockaddr_in6. */
    return (const struct sockaddr_storage *)(const void *)
        &addr->storage.alignment;
}

static cfw_status_t cfw_addr_store(cfw_addr_t *addr,
                                   const struct sockaddr *sa, int len)
{
    if (addr == NULL || sa == NULL || len <= 0 ||
        (size_t)len > CFW_ADDR_STORAGE_CAPACITY) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(addr, 0, sizeof(*addr));
    memcpy(addr->storage.opaque, sa, (size_t)len);
    addr->length = (uint32_t)len;
    return CFW_OK;
}

static int cfw_addr_to_sockaddr(const cfw_addr_t *addr,
                                struct sockaddr_storage *storage, int *len)
{
    if (!cfw_addr_valid(addr)) {
        return 0;
    }
    memset(storage, 0, sizeof(*storage));
    memcpy(storage, addr->storage.opaque, addr->length);
    *len = (int)addr->length;
    return 1;
}

cfw_status_t cfw_addr_from_str(cfw_addr_t *addr, const char *text,
                               uint16_t port)
{
    struct sockaddr_in in4;
    struct sockaddr_in6 in6;
    int is_v6;
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
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
                              (int)sizeof(in4));
    }

    memset(&in6, 0, sizeof(in6));
    if (inet_pton(AF_INET6, text, &in6.sin6_addr) != 1) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(port);
    return cfw_addr_store(addr, (const struct sockaddr *)&in6,
                          (int)sizeof(in6));
}

cfw_status_t cfw_addr_resolve_all(cfw_addr_list_t *list,
                                  const char *host, uint16_t port,
                                  cfw_addr_family_t family)
{
    int af;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    char port_str[8];
    int rc;
    size_t count = 0;
    size_t filled = 0;
    cfw_addr_t *items = NULL;
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
    if (list == NULL || host == NULL || host[0] == '\0') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(list, 0, sizeof(*list));
    if (family == CFW_ADDR_FAMILY_IPV4) {
        af = AF_INET;
    } else if (family == CFW_ADDR_FAMILY_IPV6) {
        af = AF_INET6;
    } else {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        return rc == WSAHOST_NOT_FOUND || rc == WSANO_DATA ?
                   CFW_ERROR_INVALID_ARGUMENT
               : rc == WSAEINPROGRESS ? CFW_ERROR_WOULDBLOCK
                                      : CFW_ERROR_IO;
    }
    for (it = res; it != NULL; it = it->ai_next) {
        if (it->ai_addrlen <= CFW_ADDR_STORAGE_CAPACITY &&
            it->ai_family == af) {
            count++;
        }
    }
    if (count == 0) {
        freeaddrinfo(res);
        return CFW_ERROR_IO;
    }
    if (count > SIZE_MAX / sizeof(cfw_addr_t)) {
        freeaddrinfo(res);
        return CFW_ERROR_NOMEM;
    }
    items = malloc(count * sizeof(*items));
    if (items == NULL) {
        freeaddrinfo(res);
        return CFW_ERROR_NOMEM;
    }
    for (it = res; it != NULL; it = it->ai_next) {
        if (it->ai_addrlen <= CFW_ADDR_STORAGE_CAPACITY &&
            it->ai_family == af &&
            cfw_addr_store(&items[filled],
                           (const struct sockaddr *)it->ai_addr,
                           (int)it->ai_addrlen) == CFW_OK) {
            filled++;
        }
    }
    freeaddrinfo(res);
    if (filled == 0) {
        free(items);
        return CFW_ERROR_IO;
    }
    list->items = items;
    list->count = filled;
    return CFW_OK;
}

void cfw_addr_list_free(cfw_addr_list_t *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

cfw_status_t cfw_addr_resolve(cfw_addr_t *addr, const char *host,
                              uint16_t port, cfw_addr_family_t family)
{
    cfw_addr_list_t list;
    cfw_status_t status;

    if (addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    status = cfw_addr_resolve_all(&list, host, port, family);
    if (status != CFW_OK) {
        return status;
    }
    *addr = list.items[0];
    cfw_addr_list_free(&list);
    return CFW_OK;
}

cfw_status_t cfw_addr_to_str(const cfw_addr_t *addr, char *buffer,
                             size_t capacity)
{
    const struct sockaddr_storage *ss;
    PCSTR result;
    int family;
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
    if (!cfw_addr_valid(addr) || buffer == NULL || capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    ss = cfw_addr_sockaddr(addr);
    family = ss->ss_family;

    if (family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)ss;
        result = inet_ntop(AF_INET, &in4->sin_addr, buffer,
                           (size_t)capacity);
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)ss;
        result = inet_ntop(AF_INET6, &in6->sin6_addr, buffer,
                           (size_t)capacity);
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
    const struct sockaddr_storage *ss;

    if (!cfw_addr_valid(addr) || port == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    ss = cfw_addr_sockaddr(addr);

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)ss;
        *port = ntohs(in4->sin_port);
        return CFW_OK;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)ss;
        *port = ntohs(in6->sin6_port);
        return CFW_OK;
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

int cfw_addr_is_ipv4(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->ss_family == AF_INET;
}

int cfw_addr_is_ipv6(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->ss_family == AF_INET6;
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
                              (int)sizeof(in4));
    }

    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    /* sin6_addr stays all-zero (in6addr_any). */
    in6.sin6_port = htons(port);
    return cfw_addr_store(addr, (const struct sockaddr *)&in6,
                          (int)sizeof(in6));
}

static cfw_status_t cfw_net_socket_nonblock(SOCKET raw, int enable)
{
    u_long mode = enable ? 1ul : 0ul;
    if (ioctlsocket(raw, FIONBIO, &mode) != 0) {
        return cfw_net_wsa_error();
    }
    return CFW_OK;
}

cfw_status_t cfw_net_socket(int domain, int type, int protocol,
                            cfw_fd_t *fd)
{
    SOCKET raw;
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
    if (fd == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    raw = socket(domain, type, protocol);
    if (raw == INVALID_SOCKET) {
        return cfw_net_wsa_error();
    }

    /* All cfw sockets are nonblocking (net.h contract). */
    status = cfw_net_socket_nonblock(raw, 1);
    if (status != CFW_OK) {
        (void)closesocket(raw);
        return status;
    }
    fd->handle = (intptr_t)raw;
    return CFW_OK;
}

cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *fd, int enable)
{
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
    if (fd == NULL || !cfw_fd_valid(fd)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    return cfw_net_socket_nonblock((SOCKET)fd->handle, enable);
}

cfw_status_t cfw_net_bind(cfw_fd_t *fd, const cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    int len;

    if (fd == NULL || !cfw_fd_valid(fd) ||
        !cfw_addr_to_sockaddr(addr, &storage, &len)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (bind((SOCKET)fd->handle, (const struct sockaddr *)&storage,
             len) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    return CFW_OK;
}

cfw_status_t cfw_net_listen(cfw_fd_t *fd, int backlog)
{
    if (fd == NULL || !cfw_fd_valid(fd) || backlog < 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (listen((SOCKET)fd->handle, backlog) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    return CFW_OK;
}

cfw_status_t cfw_net_accept(cfw_fd_t *fd, cfw_addr_t *peer,
                            cfw_fd_t *out)
{
    struct sockaddr_storage ss;
    int len = (int)sizeof(ss);
    SOCKET raw;
    cfw_status_t status;

    status = cfw_wsa_ensure();
    if (status != CFW_OK) {
        return status;
    }
    if (fd == NULL || !cfw_fd_valid(fd) || out == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }

    raw = accept((SOCKET)fd->handle, (struct sockaddr *)&ss, &len);
    if (raw == INVALID_SOCKET) {
        return cfw_net_wsa_error();
    }

    status = cfw_net_socket_nonblock(raw, 1);
    if (status != CFW_OK) {
        (void)closesocket(raw);
        return status;
    }

    if (peer != NULL) {
        status = cfw_addr_store(peer, (const struct sockaddr *)&ss, len);
        if (status != CFW_OK) {
            (void)closesocket(raw);
            return status;
        }
    }

    if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
        int one = 1;
        (void)setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                         sizeof(one));
    }
    out->handle = (intptr_t)raw;
    return CFW_OK;
}

cfw_status_t cfw_net_connect(cfw_fd_t *fd, const cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    int len;

    if (fd == NULL || !cfw_fd_valid(fd) ||
        !cfw_addr_to_sockaddr(addr, &storage, &len)) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (connect((SOCKET)fd->handle, (const struct sockaddr *)&storage,
                len) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    return CFW_OK;
}

cfw_status_t cfw_net_local_addr(cfw_fd_t *fd, cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    int len = (int)sizeof(storage);

    if (fd == NULL || !cfw_fd_valid(fd) || addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getsockname((SOCKET)fd->handle, (struct sockaddr *)&storage,
                    &len) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    return cfw_addr_store(addr, (const struct sockaddr *)&storage, len);
}

cfw_status_t cfw_net_peer_addr(cfw_fd_t *fd, cfw_addr_t *addr)
{
    struct sockaddr_storage storage;
    int len = (int)sizeof(storage);

    if (fd == NULL || !cfw_fd_valid(fd) || addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getpeername((SOCKET)fd->handle, (struct sockaddr *)&storage,
                    &len) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    return cfw_addr_store(addr, (const struct sockaddr *)&storage, len);
}

cfw_status_t cfw_net_send(cfw_fd_t *fd, const void *data, size_t size,
                          size_t *sent)
{
    int n;

    if (fd == NULL || !cfw_fd_valid(fd) || sent == NULL ||
        (size != 0 && data == NULL) || size > (size_t)INT32_MAX) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        *sent = 0;
        return CFW_OK;
    }

    n = send((SOCKET)fd->handle, (const char *)data, (int)size, 0);
    if (n > 0) {
        *sent = (size_t)n;
        return CFW_OK;
    }
    *sent = 0;
    if (n == 0) {
        return CFW_ERROR_IO;
    }
    return cfw_net_wsa_error();
}

cfw_status_t cfw_net_recv(cfw_fd_t *fd, void *buffer, size_t size,
                          size_t *received)
{
    int n;

    if (fd == NULL || !cfw_fd_valid(fd) || received == NULL ||
        (size != 0 && buffer == NULL) || size > (size_t)INT32_MAX) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        *received = 0;
        return CFW_OK;
    }

    n = recv((SOCKET)fd->handle, (char *)buffer, (int)size, 0);
    if (n > 0) {
        *received = (size_t)n;
        return CFW_OK;
    }
    *received = 0;
    if (n == 0) {
        /* Orderly peer shutdown. */
        return CFW_OK;
    }
    return cfw_net_wsa_error();
}

cfw_status_t cfw_net_so_error(cfw_fd_t *fd, int *error)
{
    int err = 0;
    int len = (int)sizeof(err);

    if (fd == NULL || !cfw_fd_valid(fd) || error == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    if (getsockopt((SOCKET)fd->handle, SOL_SOCKET, SO_ERROR,
                   (char *)&err, &len) == SOCKET_ERROR) {
        return cfw_net_wsa_error();
    }
    *error = err;
    return CFW_OK;
}

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
_Static_assert(_Alignof(struct sockaddr_storage) <=
                   _Alignof(cfw_max_align_t),
               "sockaddr_storage alignment exceeds cfw_max_align_t");

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

static const struct sockaddr_storage *cfw_addr_sockaddr(
    const cfw_addr_t *addr)
{
    /* Read through the union's aligned member: opaque is unsigned char
     * storage, so casting it directly trips -Wcast-align when callers
     * convert to sockaddr_in/sockaddr_in6. */
    return (const struct sockaddr_storage *)(const void *)
        &addr->storage.alignment;
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

cfw_status_t cfw_addr_resolve_all(cfw_addr_list_t *list,
                                  const char *host, uint16_t port,
                                  cfw_addr_family_t family)
{
    int af;

    if (list == NULL || host == NULL || host[0] == '\0') {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    memset(list, 0, sizeof(*list));
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
        size_t count = 0;
        size_t filled = 0;
        cfw_addr_t *items = NULL;

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
                count++;
            }
        }
        if (count == 0) {
            freeaddrinfo(res);
            return CFW_ERROR_IO;
        }
        if (count > SIZE_MAX / sizeof(cfw_addr_t)) {
            freeaddrinfo(res);
            return CFW_ERROR_NOMEM;
        }
        items = malloc(count * sizeof(*items));
        if (items == NULL) {
            freeaddrinfo(res);
            return CFW_ERROR_NOMEM;
        }
        for (it = res; it != NULL; it = it->ai_next) {
            if (it->ai_addrlen <= CFW_ADDR_STORAGE_CAPACITY &&
                it->ai_family == af &&
                cfw_addr_store(&items[filled],
                               (const struct sockaddr *)it->ai_addr,
                               (socklen_t)it->ai_addrlen) == CFW_OK) {
                filled++;
            }
        }
        freeaddrinfo(res);
        if (filled == 0) {
            free(items);
            return CFW_ERROR_IO;
        }
        list->items = items;
        list->count = filled;
        return CFW_OK;
    }
#else
    (void)host;
    (void)port;
    (void)af;
    return CFW_ERROR_UNAVAILABLE;
#endif
}

void cfw_addr_list_free(cfw_addr_list_t *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

cfw_status_t cfw_addr_resolve(cfw_addr_t *addr, const char *host,
                              uint16_t port, cfw_addr_family_t family)
{
    cfw_addr_list_t list;
    cfw_status_t status;

    if (addr == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    status = cfw_addr_resolve_all(&list, host, port, family);
    if (status != CFW_OK) {
        return status;
    }
    *addr = list.items[0];
    cfw_addr_list_free(&list);
    return CFW_OK;
}

cfw_status_t cfw_addr_to_str(const cfw_addr_t *addr, char *buffer,
                             size_t capacity)
{
    const struct sockaddr_storage *ss;
    const char *result;
    int family;

    if (!cfw_addr_valid(addr) || buffer == NULL || capacity == 0) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    ss = cfw_addr_sockaddr(addr);
    family = ss->ss_family;

    if (family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)ss;
        result = inet_ntop(AF_INET, &in4->sin_addr, buffer,
                           (socklen_t)capacity);
    } else if (family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)ss;
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
    const struct sockaddr_storage *ss;

    if (!cfw_addr_valid(addr) || port == NULL) {
        return CFW_ERROR_INVALID_ARGUMENT;
    }
    ss = cfw_addr_sockaddr(addr);

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *in4 =
            (const struct sockaddr_in *)ss;
        *port = ntohs(in4->sin_port);
        return CFW_OK;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 =
            (const struct sockaddr_in6 *)ss;
        *port = ntohs(in6->sin6_port);
        return CFW_OK;
    }
    return CFW_ERROR_INVALID_ARGUMENT;
}

int cfw_addr_is_ipv4(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->ss_family == AF_INET;
}

int cfw_addr_is_ipv6(const cfw_addr_t *addr)
{
    return cfw_addr_valid(addr) &&
           cfw_addr_sockaddr(addr)->ss_family == AF_INET6;
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
