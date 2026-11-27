<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0073: http1_conn transport via cfw_net_*

## Status

Accepted.

## Context

`cfw_http1_conn_t` performed raw `recv`/`send` on the descriptor handle
with POSIX `errno` checks and `MSG_NOSIGNAL` handling, which tied it to
POSIX and duplicated logic already owned by `cfw_net_recv`/`cfw_net_send`
(ADR 0067). With the Windows Winsock layer in place (ADR 0072), keeping
raw calls would have required a parallel Winsock path.

## Decision

`cfw_http1_conn_t` now reads and writes exclusively through
`cfw_net_recv`/`cfw_net_send` when no transport hook is configured.
Those functions already map the required outcomes — bytes,
orderly-EOF (`CFW_OK` with zero bytes), `CFW_ERROR_WOULDBLOCK`, and
fatal errors — onto `cfw_status_t`, and they own platform signal/error
semantics (`MSG_NOSIGNAL` on Linux, `SO_NOSIGPIPE` elsewhere, plain
`send` on Windows). The transport-seam path (TLS, ADR 0058) is
unchanged.

The Windows stub block for `cfw_http1_conn_*` is removed; the
implementation compiles on every platform.

## Alternatives considered

- **Platform `#ifdef` recv/send wrappers inside http1_conn.c:**
  rejected; it would duplicate the error-mapping table that `net.c`
  already maintains and create a second place to audit WSAGetLastError
  vs errno handling.
- **Keep POSIX-only and stub Windows:** rejected; the conn layer has no
  inherent POSIX dependency once socket I/O goes through `cfw_net_*`.

## Consequences

- `cfw_http1_conn_t` works on Windows (still unused there until
  `cfw_server_t` is ported).
- One fewer direct socket call site in shared code; the audit surface
  for signal/error semantics shrinks to `net.c`.
- No public API or behavioral change on POSIX.
