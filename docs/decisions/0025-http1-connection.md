<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0025: HTTP/1.1 connection binding

- Status: Accepted
- Date: 2026-11-26

## Context

The `cfw_http1_server_t` reader (ADR 0024) consumes `cfw_span_t` chunks but
knows nothing about sockets. Phase 3's "socket lifecycle and connection
management" needs a transport binding: nonblocking reads into the reader,
queued response writes, and close semantics — without leaking pollset or
private-net details into application code.

## Decision

Add `cfw_http1_conn_t` (`cfw/http1_conn.h`, POSIX-only for now) owning an
accepted nonblocking `cfw_fd_t`, the request reader, and an outbound
buffer:

- **Ownership:** `init` takes ownership of the fd wrapper (released);
  `free`/`close` release it. The struct is public but fields are private.
- **Read path:** `next_event` drains reader events first; on
  `WOULDBLOCK` it performs one `recv` (16 KiB stack buffer) and feeds the
  reader, looping until an event or true would-block. `recv == 0` marks
  input closed; socket errors latch `failed` and return `CFW_ERROR_IO`.
- **Write path:** `queue` appends to a bounded buffer (`max_out_size`);
  overflow is rejected so callers flush first. `flush` does nonblocking
  `send` with `MSG_NOSIGNAL` where available, returning `WOULDBLOCK` with
  the remainder retained.
- **Close semantics:** `close_after_write` is set automatically on
  `MESSAGE_DONE` for non-keep-alive requests and on any `ERROR` event;
  the socket closes once the output queue drains (or immediately on
  `close`). This gives graceful "respond then FIN" behavior.
- **Poll contract:** poll `CFW_POLL_IN` while open, `CFW_POLL_OUT` while
  `pending_out() > 0`; either order is safe since reads and writes are
  independent buffers.

## Alternatives considered

- **Exposing pollset/net publicly:** rejected; keeps the internal
  abstraction boundary (ADR 0011/0014) intact — applications program
  against conn readiness, not fds.
- **Integrating TLS:** deferred; the conn operates on a byte stream and a
  TLS layer can slot beneath `feed`/`queue` later.
- **Write-combining gather (writev):** deferred until response pipelining
  makes it measurable.

## Consequences

- A complete request→response cycle works over real sockets today; the
  remaining Phase 3/5 work is listener management, multi-connection
  scheduling, and graceful server shutdown.
- Windows returns `CFW_ERROR_UNAVAILABLE` until the IOCP backend lands.
