/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */

#ifndef CFW_PRIVATE_NET_H
#define CFW_PRIVATE_NET_H

/*
 * Internal socket and address abstraction (POSIX). Not part of the public
 * API; see docs/decisions/0011 and the Phase 2/3 records. Sockets are
 * represented by cfw_fd_t owners; all functions validate arguments and map
 * errno to cfw_status_t. Nonblocking operations report CFW_ERROR_WOULDBLOCK.
 */

#include <cfw/fd.h>
#include <cfw/status.h>

#if defined(_WIN32)
    /* Winsock adapter is a later phase; this header is POSIX-only. */
    #error "cfw private net.h is POSIX-only"
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfw_sockaddr {
    struct sockaddr_storage storage;
    socklen_t len;
} cfw_sockaddr_t;

/* Numeric-literal addresses only ("192.0.2.1", "::1"); no DNS. */
cfw_status_t cfw_addr_from_str(cfw_sockaddr_t *addr, const char *text,
                               uint16_t port);
cfw_status_t cfw_addr_to_str(const cfw_sockaddr_t *addr, char *buffer,
                             size_t capacity);
cfw_status_t cfw_addr_port(const cfw_sockaddr_t *addr, uint16_t *port);
int cfw_addr_is_ipv4(const cfw_sockaddr_t *addr);
int cfw_addr_is_ipv6(const cfw_sockaddr_t *addr);
cfw_status_t cfw_addr_any(uint16_t port, int ipv6, cfw_sockaddr_t *addr);

cfw_status_t cfw_net_socket(int domain, int type, int protocol,
                            cfw_fd_t *fd);
cfw_status_t cfw_net_set_nonblocking(cfw_fd_t *fd, int enable);
cfw_status_t cfw_net_bind(cfw_fd_t *fd, const cfw_sockaddr_t *addr);
cfw_status_t cfw_net_listen(cfw_fd_t *fd, int backlog);
cfw_status_t cfw_net_accept(cfw_fd_t *fd, cfw_sockaddr_t *peer,
                            cfw_fd_t *out);
cfw_status_t cfw_net_connect(cfw_fd_t *fd, const cfw_sockaddr_t *addr);
cfw_status_t cfw_net_local_addr(cfw_fd_t *fd, cfw_sockaddr_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* CFW_PRIVATE_NET_H */
