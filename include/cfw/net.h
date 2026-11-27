/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_NET_H
#define CFW_NET_H

#include <cfw/export.h>
#include <cfw/fd.h>
#include <cfw/max_align.h>
#include <cfw/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public socket and address primitives (ADR 0067; promotes the internal
 * layer from ADR 0015). Sockets are represented by the owning cfw_fd_t
 * wrapper: every cfw_net_* function that produces a descriptor returns
 * it in a caller-owned cfw_fd_t, and cfw_fd_close releases it.
 *
 * All sockets created by cfw_net_socket and cfw_net_accept are
 * nonblocking and close-on-exec by default. Connect, accept, send, and
 * recv therefore report CFW_ERROR_WOULDBLOCK when the operation cannot
 * complete immediately; callers drive progress with a pollset (see
 * cfw_pollset_* equivalents in server integrations) or blocking loops.
 *
 * cfw_addr_t is a portable, fixed-size address value. Its storage is
 * opaque: callers must use the cfw_addr_* helpers and must not inspect
 * or construct the bytes directly. The storage is sized for
 * sockaddr_storage on every supported platform (128 bytes).
 *
 * Platform support: implemented on POSIX systems and Windows (Winsock2,
 * ADR 0072). See docs/platform-matrix.md.
 */

/* Capacity of cfw_addr_t storage in bytes; matches sockaddr_storage. */
#define CFW_ADDR_STORAGE_CAPACITY 128u

typedef struct cfw_addr {
    union {
        /* Keeps the storage aligned for any sockaddr variant. */
        cfw_max_align_t alignment;
        unsigned char opaque[CFW_ADDR_STORAGE_CAPACITY];
    } storage;
    /* Number of valid bytes in storage.opaque. */
    uint32_t length;
} cfw_addr_t;

typedef enum cfw_addr_family {
    CFW_ADDR_FAMILY_IPV4 = 0,
    CFW_ADDR_FAMILY_IPV6 = 1
} cfw_addr_family_t;

/* Parses a numeric-literal IPv4 or IPv6 address ("192.0.2.1", "::1")
 * with the given port. No DNS: use cfw_addr_resolve for names. */
CFW_API cfw_status_t cfw_addr_from_str(cfw_addr_t *addr, const char *text,
                                       uint16_t port);

/* Resolves `host` (DNS name or numeric literal) via getaddrinfo and
 * returns the first address in `family`. BLOCKING: intended for
 * connection setup, never inside an event loop. Returns
 * CFW_ERROR_INVALID_ARGUMENT for unresolvable names. */
CFW_API cfw_status_t cfw_addr_resolve(cfw_addr_t *addr, const char *host,
                                      uint16_t port,
                                      cfw_addr_family_t family);

/* Writes the numeric address text (no port, no brackets) into buffer.
 * IPv6 text uses the canonical compressed form. */
CFW_API cfw_status_t cfw_addr_to_str(const cfw_addr_t *addr,
                                     char *buffer, size_t capacity);
CFW_API cfw_status_t cfw_addr_port(const cfw_addr_t *addr, uint16_t *port);
CFW_API int cfw_addr_is_ipv4(const cfw_addr_t *addr);
CFW_API int cfw_addr_is_ipv6(const cfw_addr_t *addr);

/* The unspecified/wildcard address (INADDR_ANY / in6addr_any). */
CFW_API cfw_status_t cfw_addr_any(uint16_t port, int ipv6, cfw_addr_t *addr);

/* Creates a nonblocking, close-on-exec socket owned by *fd.
 * `domain`, `type`, and `protocol` are platform socket constants
 * (POSIX: AF_INET/AF_INET6, SOCK_STREAM/SOCK_DGRAM, 0). */
CFW_API cfw_status_t cfw_net_socket(int domain, int type, int protocol,
                                    cfw_fd_t *fd);
CFW_API cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *fd, int enable);
CFW_API cfw_status_t cfw_net_bind(cfw_fd_t *fd, const cfw_addr_t *addr);
CFW_API cfw_status_t cfw_net_listen(cfw_fd_t *fd, int backlog);

/* Accepts one pending connection. On success `out` owns the new
 * nonblocking descriptor and `peer` (when non-NULL) receives the peer
 * address. Returns CFW_ERROR_WOULDBLOCK when no connection is pending. */
CFW_API cfw_status_t cfw_net_accept(cfw_fd_t *fd, cfw_addr_t *peer,
                                    cfw_fd_t *out);

/* Starts a nonblocking connect. May return CFW_OK immediately,
 * CFW_ERROR_WOULDBLOCK while in progress (wait for writability, then
 * check cfw_net_so_error), or an error status. */
CFW_API cfw_status_t cfw_net_connect(cfw_fd_t *fd, const cfw_addr_t *addr);

CFW_API cfw_status_t cfw_net_local_addr(cfw_fd_t *fd, cfw_addr_t *addr);
CFW_API cfw_status_t cfw_net_peer_addr(cfw_fd_t *fd, cfw_addr_t *addr);

/* Nonblocking I/O. On CFW_OK, the sent/received out-params hold the byte
 * count;
 * a zero count from cfw_net_recv signals orderly peer shutdown (EOF),
 * while cfw_net_send never reports a zero count for size > 0.
 * CFW_ERROR_WOULDBLOCK means retry when the socket is ready. */
CFW_API cfw_status_t cfw_net_send(cfw_fd_t *fd, const void *data,
                                  size_t size, size_t *sent);
CFW_API cfw_status_t cfw_net_recv(cfw_fd_t *fd, void *buffer, size_t size,
                                  size_t *received);

/* Returns the pending socket error (SO_ERROR) and clears it: *error is
 * 0 when the socket is healthy. Used after a nonblocking connect
 * completes to distinguish success from refusal/reset. */
CFW_API cfw_status_t cfw_net_so_error(cfw_fd_t *fd, int *error);

#ifdef __cplusplus
}
#endif

#endif /* CFW_NET_H */
