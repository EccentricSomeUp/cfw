<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0067: Public socket and address API

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0015 deliberately kept the socket/address layer internal: at the
time there was no external consumer, the Windows backend was missing,
and `send`/`recv` were deferred to the transport layer. Since then the
transport layer (`cfw_http1_conn_t`, `cfw_server_t`, `cfw_upgraded_*`)
landed and owns its own buffering, but applications still have no
supported way to open outbound connections — a prerequisite for any
client-side feature (HTTP client, outbound webhook dispatch, upstream
proxying) and for applications that want raw TCP/UDP-adjacent sockets.

The internal implementation lived in `src/cfw/private/net.[ch]` with
the platform type `cfw_sockaddr_t` wrapping `struct sockaddr_storage`
— unusable in a public header because `sockaddr_storage` does not
exist on Windows.

## Decision

Promote the layer to a public API in `include/cfw/net.h` +
`src/cfw/net.c`, replacing `src/cfw/private/net.[ch]`:

- `cfw_addr_t` is a portable fixed-size address value: a union of
  `cfw_max_align_t` and `unsigned char[CFW_ADDR_STORAGE_CAPACITY]`
  (128 bytes, matching `sockaddr_storage` on every supported
  platform) plus a `uint32_t length`. Storage is opaque; all access
  goes through `cfw_addr_*` helpers. A `_Static_assert` in `net.c`
  guarantees `sockaddr_storage` fits.
- `cfw_addr_family_t` (`CFW_ADDR_FAMILY_IPV4`/`IPV6`) replaces the raw
  `AF_INET`/`AF_INET6` argument of `cfw_addr_resolve` so callers never
  need platform socket constants for the common case. `cfw_net_socket`
  still takes raw `domain`/`type`/`protocol` values (documented as
  platform socket constants) because it is a thin escape hatch.
- New functions completing the deferred surface: `cfw_net_send`,
  `cfw_net_recv` (nonblocking, `MSG_NOSIGNAL` where available),
  `cfw_net_peer_addr` (`getpeername`), and `cfw_net_so_error`
  (`SO_ERROR` read-and-clear, needed to complete nonblocking connects).
- All functions return `CFW_ERROR_UNAVAILABLE` on Windows, matching
  the existing stub convention until the Winsock/IOCP backend lands.

`cfw_sockaddr_t` is renamed `cfw_addr_t` throughout `server.c` and the
test suite; `addr.len` becomes `addr.length`.

## Alternatives considered

- **Keep internal layer, add a separate public wrapper with distinct
  names:** rejected — two parallel socket APIs would double the audit
  surface and invite drift; the internal names were never a stable
  contract.
- **Expose `sockaddr_storage` in the public struct:** rejected —
  requires `<sys/socket.h>` on the consumer side and has no Windows
  equivalent.
- **Heap-allocated opaque `cfw_addr_t*`:** rejected — addresses are
  small values passed by pointer everywhere else in the API; an
  inline 132-byte struct matches the existing by-pointer convention
  and avoids allocation in connect paths.

## Consequences

- Outbound TCP is now possible for applications: resolve →
  `cfw_net_socket` → `cfw_net_connect` → poll writability →
  `cfw_net_so_error` → `cfw_net_send`/`cfw_net_recv`.
- The blocking `cfw_addr_resolve` (getaddrinfo) remains documented as
  init-time-only; event-loop code must resolve beforehand.
- Security audit must cover errno mapping, fd ownership transfer on
  accept, address validation, partial-I/O behavior of send/recv, and
  the opaque-storage boundary (callers cannot forge lengths beyond
  `CFW_ADDR_STORAGE_CAPACITY` — `cfw_addr_to_sockaddr` revalidates).
