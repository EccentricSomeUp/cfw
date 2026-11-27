<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0072: Windows socket and pollset backends (Winsock + WSAPoll)

## Status

Accepted.

## Context

ADR 0011 designed the event-loop abstraction with IOCP as the planned
Windows backend, and the platform matrix records sockets, pollset,
`cfw_fd_close`, the HTTP server, and the HTTP/WebSocket clients as
Windows stubs returning `CFW_STATUS_UNAVAILABLE`. The 0.x series has
shipped a complete POSIX implementation; Windows is the last major
platform gap.

The `cfw_pollset_t` contract (ADR 0014/0069) is **readiness-based**:
`add`/`modify` register interest in `CFW_POLL_IN`/`CFW_POLL_OUT` and
`wait` reports which handles are currently readable/writable. IOCP is
**completion-based**: it delivers finished overlapped operations, not
readiness. Emulating readiness on IOCP would require the pollset to own
per-socket probe buffers and issue dummy `WSARecv`/`WSASend` calls —
state the abstraction deliberately does not have — or restructuring the
entire server/conn stack around completions.

`WSAPoll` (Windows 8+/Server 2012+, declared in `ws2tcpip.h`) provides
`poll(2)`-equivalent semantics on `SOCKET` handles with no
`FD_SETSIZE`-style limit, matching the readiness contract directly. The
framework already supports a non-scalable `poll()` fallback backend on
POSIX, so a `WSAPoll` backend is consistent with existing precedent.

## Decision

Implement Windows support in two layers:

1. **Socket/address layer** (`src/cfw/net.c`, `src/cfw/fd.c`): real
   Winsock2 implementations of `cfw_addr_*` and `cfw_net_*`, with
   `closesocket` in `cfw_fd_close`. `SOCKET` values are stored in the
   existing `intptr_t` handle field; `INVALID_SOCKET` maps to the
   invalid `cfw_fd_t` value. Winsock is initialized lazily and exactly
   once per process via `InitOnceExecuteOnce` + `WSAStartup(2.2)`;
   `WSACleanup` is never called (the library does not own process
   lifetime). Nonblocking mode uses `ioctlsocket(FIONBIO)`;
   `cfw_net_set_nonblocking` toggles the same flag.
2. **Pollset backend** (`src/cfw/pollset.c`): a `WSAPoll` backend
   selected on `CFW_PLATFORM_WINDOWS`, structurally mirroring the
   `poll()` fallback (per-handle entries + `WSAPOLLFD` array, millisecond
   deadline timeouts, `POLLIN`-family → `CFW_POLL_IN`, `POLLOUT` →
   `CFW_POLL_OUT`, `POLLERR`/`POLLNVAL` → `CFW_POLL_ERR`,
   `POLLHUP` → `CFW_POLL_HUP`).

This unblocks the portable consumers that were already written against
`cfw_net_*`/`cfw_pollset_*`/`cfw_fd_*`: `cfw_http_client_*` and
`cfw_ws_client_*` (cleartext `ws://`; `wss://` and all TLS remain
`CFW_ERROR_UNAVAILABLE` until the TLS module gains a Windows backend).
`cfw_http1_conn_t`/`cfw_server_t` remain POSIX-only stubs pending a
dedicated port (wake channel, direct `recv`/`send` call sites, and
`socketpair` equivalents).

## Alternatives considered

- **IOCP as the v1 Windows backend:** rejected for now. It is the
  scalable native mechanism, but it is completion-based and cannot
  satisfy the readiness contract without an internal probe/emulation
  layer that adds buffers, ordering, and cancellation complexity the
  current abstraction does not model. Deferred; a future ADR may add an
  IOCP backend behind the same public contract if a workload demands
  it.
- **`select()`:** rejected; the 1024-handle `FD_SETSIZE` ceiling and
  fd-set copying make it unsuitable even as a fallback when `WSAPoll`
  is available on every supported Windows target.
- **Porting `cfw_server_t` in the same change:** deferred. The server
  has POSIX-specific wake-channel (`socketpair(AF_UNIX)`) and raw
  `recv`/`send` call sites; mixing that port with the socket-layer
  foundation would produce an unreviewable change.

## Consequences

- `cfw_net_*`, `cfw_addr_*`, `cfw_pollset_*`, and `cfw_fd_close` become
  functional on Windows; `cfw_http_client_request` and
  `cfw_ws_client_connect` (cleartext) work there.
- Windows builds link `ws2_32`; `CFW_HAVE_GETADDRINFO` and
  `CFW_HAVE_TCP_NODELAY` are enabled for Windows in platform detection.
- WSAPoll requires Windows 8/Server 2012 or newer; the pinned
  `_WIN32_WINNT` floor (0x0A00) already exceeds that.
- TLS on Windows remains unavailable (`CFW_ENABLE_TLS` is still
  rejected on `WIN32`); OpenSSL's Windows TLS backend is a separate
  decision.
- The Windows CI job remains compile-only until the server port lands
  and tests can run on `windows-latest`.
