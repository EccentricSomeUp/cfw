<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0015: Internal socket and address primitives

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 2 requires a socket abstraction before the HTTP/transport layer. The
first implementation targets POSIX; a Winsock adapter is deferred. Sockets
are represented by the `cfw_fd_t` owner from ADR 0011.

## Decision

Add `src/cfw/private/net.h` and `src/cfw/private/net.c` (POSIX; Windows
builds provide `CFW_ERROR_UNAVAILABLE` stubs):

- `cfw_sockaddr_t`: `sockaddr_storage` plus length, the internal address
  currency.
- `cfw_addr_from_str` / `cfw_addr_to_str` / `cfw_addr_port` /
  `cfw_addr_is_ipv4` / `cfw_addr_is_ipv6` / `cfw_addr_any`: numeric-literal
  IPv4/IPv6 address handling via `inet_pton`/`inet_ntop`. No DNS.
- `cfw_net_socket`: creates a nonblocking, close-on-exec socket (`SOCK_*`
  flags where available, `fcntl` fallback) owned by a `cfw_fd_t`.
- `cfw_net_set_nonblocking`, `cfw_net_bind`, `cfw_net_listen`,
  `cfw_net_accept` (nonblocking `accept4` on Linux, `accept`+`fcntl`
  elsewhere), `cfw_net_connect`, `cfw_net_local_addr` (`getsockname`).

`CFW_ERROR_WOULDBLOCK` is added to the public status enum (append-only,
ABI-safe) and `EAGAIN`/`EWOULDBLOCK`/`EINPROGRESS` map to it. Other errnos
map to `IO`, `NOMEM`, `TIMEOUT`, or `INVALID_ARGUMENT`.

## Alternatives considered

- **Public socket API now:** deferred; like the pollset, the contract has
  no external consumer and the Windows backend is missing.
- **`getaddrinfo` in `cfw_addr_from_str`:** rejected for now; DNS introduces
  blocking, lifetime, and injection complexity better isolated in a later
  resolver layer.
- **Blocking sockets with pollset-driven timeouts:** rejected; nonblocking
  sockets plus level-triggered readiness is the standard scalable model.
- **`send`/`recv` wrappers in this change:** deferred to the transport
  layer which will own buffering and partial-I/O policy.

## Consequences

- Loopback TCP connect/accept is unit-tested end to end.
- All socket creation is nonblocking and close-on-exec by default.
- A real bug class was caught in review: `accept4`'s value-result
  `addrlen` must be initialized before the call.
- Security audits must cover errno mapping, fd ownership transfer on
  accept, address validation, and partial-I/O behavior once send/recv land.
