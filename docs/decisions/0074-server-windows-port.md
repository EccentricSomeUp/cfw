<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0074: cfw_server_t on Windows

## Status

Accepted.

## Context

ADR 0072 delivered the Winsock socket layer, WSAPoll pollset, and fd
close, and ADR 0073 moved `cfw_http1_conn_t` onto `cfw_net_*`. The
remaining blocker for `cfw_server_t` on Windows was a set of
POSIX-specific call sites: the `socketpair(AF_UNIX)` wake channel,
`fcntl` nonblocking/cloexec setup, raw `recv`/`send` with `errno`
checks (including a `MSG_PEEK` TLS-detection sniff), and `close()`.

## Decision

Port `cfw_server_t` to Windows with minimal, well-factored seams:

- **Socket I/O wrappers.** `cfw_server_sock_recv`/`cfw_server_sock_send`
  wrap `recv`/`send` for the paths that need flags (`MSG_PEEK`) or raw
  byte counts (TLS sniff, wake drain, upgraded-protocol reads, wake
  byte). They cast `intptr_t` handles to `SOCKET` on Windows, clamp
  sizes to `INT32_MAX` for the Winsock `int` length parameter, and
  return the raw byte count so callers keep their existing
  `>0`/`==0`/`<0` structure.
- **Error classification.** `cfw_server_sock_wouldblock` centralizes
  `errno == EAGAIN || EWOULDBLOCK` (POSIX) vs
  `WSAGetLastError() == WSAEWOULDBLOCK` (Windows) and is invoked
  immediately after the failed call.
- **Wake channel.** Windows lacks `socketpair`/`AF_UNIX`; the wake
  pair is emulated with a loopback TCP connection built entirely from
  `cfw_net_*` calls: bind `127.0.0.1:0`, listen, blocking client
  `connect` (instant on loopback), `accept`, then restore nonblocking
  on the client end. Both ends satisfy the net.h nonblocking contract
  and register with `CFW_POLL_IN` exactly like the POSIX pair.
- **Includes.** `winsock2.h` on Windows for `AF_INET`/`SOCK_STREAM`/
  `MSG_PEEK`; POSIX socket headers remain behind `#else`.

TLS-dependent paths compile unchanged: `CFW_HAVE_TLS` is 0 on Windows,
so `tls_ctx` options fail with `CFW_ERROR_UNAVAILABLE` and the
`cfw_tls_*` stubs cover any stray references.

## Alternatives considered

- **Duplicating server.c per platform:** rejected; the file is ~2100
  lines of platform-neutral logic and a fork would double the audit
  surface.
- **Named-pipe or event-based wake on Windows:** rejected; WSAPoll can
  only wait on sockets, and a TCP loopback pair is the established
  portable pattern (also what `socketpair` emulates on POSIX).
- **Waiting for IOCP first:** rejected per ADR 0072; WSAPoll satisfies
  the readiness contract directly.

## Consequences

- `cfw_server_init`/`run`/`step`/`stop` work on Windows for cleartext
  HTTP; all connection limits, timeouts, and DoS bounds are unchanged
  because they live in platform-neutral code.
- Windows tests remain gated off (`if(NOT WIN32)` in
  `tests/CMakeLists.txt`) until they are ported; CI stays
  compile-check only.
- `SO_REUSEPORT` remains unavailable on Windows (no equivalent
  setsockopt), matching the documented `CFW_ERROR_UNAVAILABLE`
  contract.
