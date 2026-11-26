# ADR 0034: Protocol upgrades (101 Switching Protocols)

## Status

Accepted.

## Context

The HTTP layer already parses and serializes `101 Switching Protocols`
(`CFW_HTTP_STATUS_SWITCHING_PROTOCOLS`), but `cfw_dispatch` documented
that upgrades are unsupported: after a 101 response the connection
remained an HTTP/1.1 connection, so protocols such as WebSocket
(RFC 6455) or h2c could not be carried. The audit catalog already
flags "websocket or upgrade paths if added later" as a parser/protocol
audit focus area, so the takeover path needs the same rigor as the
rest of the framing code.

Constraints:

- Single-threaded, pull-based event loop; all app callbacks run on the
  connection's event-loop thread (same model as handlers and upload
  hooks).
- Bounded memory per connection; no unbounded buffering may be
  introduced by the upgrade path.
- The HTTP/1.1 reader may already hold post-request bytes in its
  pending buffer (pipelined protocol data sent in the same segment as
  the handshake). Those bytes belong to the new protocol and must not
  be lost or reinterpreted as HTTP.
- The 101 response head is queued before takeover; application writes
  must never overtake it on the wire.

## Decision

Add an opt-in protocol-upgrade hook to `cfw_server_options_t`:

```c
typedef cfw_status_t (*cfw_upgrade_fn)(cfw_upgraded_conn_t *conn,
                                       uint32_t events, void *user);
```

An upgrade happens only when all of the following hold:

- `options.upgrade` is non-NULL.
- The dispatched response status is `101 Switching Protocols`.
- The request head contains an `Upgrade` header field (the handler is
  responsible for validating the protocol token and headers; the
  server's check is only a takeover guard).

When triggered, after the response head is queued the server:

1. Clears `close_after_write` — an upgraded connection outlives HTTP
   keep-alive semantics regardless of `Connection` headers.
2. Moves any bytes already buffered past the request (the HTTP
   reader's pending input) into a per-connection upgrade buffer so
   `cfw_upgraded_recv` returns them before touching the socket.
3. Marks the connection upgraded, stops HTTP event servicing, and
   invokes `upgrade` once with `events == CFW_UPGRADE_EV_READY`.
4. On each subsequent readiness event invokes `upgrade` with a mask of
   `CFW_UPGRADE_EV_READ` / `CFW_UPGRADE_EV_WRITE` /
   `CFW_UPGRADE_EV_HUP` / `CFW_UPGRADE_EV_ERR`.

The app drives the protocol through `cfw_upgraded_conn_t`:

- `cfw_upgraded_recv` — drains buffered post-handshake bytes first,
  then nonblocking `recv`. `CFW_OK` with `*n == 0` means orderly peer
  shutdown; `CFW_ERROR_WOULDBLOCK` means no data available.
- `cfw_upgraded_send` — appends to the connection's bounded output
  queue (same `max_out_size` cap as HTTP responses) and attempts a
  flush. All-or-nothing: `CFW_ERROR_WOULDBLOCK` when the queue cannot
  accept the whole span; the app must retry when `CFW_UPGRADE_EV_WRITE`
  is delivered (sent when queued output fully drains).
- `cfw_upgraded_close` — graceful close: queued output is flushed,
  then the socket closes. `CFW_UPGRADE_EV_*` callbacks stop once
  closing begins.
- `cfw_upgraded_set_user` / `cfw_upgraded_user` — per-connection
  application state, since the hook's `user` is shared.

Interest management reuses the existing rule: `POLL_IN` while the
socket is open for reading, `POLL_OUT` only while output is queued —
so an idle upgraded connection never busy-loops. `keep_alive_timeout`
continues to apply as an idle deadline for upgraded connections
(refreshed on activity); zero disables it, so applications needing
long-lived idle connections (e.g. WebSocket ping/pong keepalive) must
set it accordingly or enforce their own timeout.

If `options.upgrade` is NULL, a 101 response is serialized like any
other response and the connection remains HTTP/1.1 — preserving the
previous behavior for applications that do not opt in.

## Alternatives considered

- **Detach/ownership handoff**: remove the connection from the server
  and hand the raw fd to the application. Rejected: the fd is
  nonblocking and registered in the server's private pollset; the app
  could not service it without new public polling API, and the
  connection would escape connection limits, deadlines, and drain
  semantics.
- **Blocking per-connection thread**: run the upgrade hook on a
  worker thread. Rejected: violates the single-threaded model (ADR
  0011/0029) and would require the threading primitives deferred since
  Phase 2.
- **Implicit upgrade on any 101**: rejected; a stray or buggy 101
  would silently detach the connection. Requiring the request `Upgrade`
  header plus an explicit server option keeps takeover opt-in at both
  layers.
- **Unbounded send buffering for upgraded connections**: rejected;
  `max_out_size` bounds memory per connection exactly as for HTTP
  responses. Large protocol frames must be fragmented by the app.
- **WebSocket implementation in this change**: deferred; this ADR
  lands the transport takeover only. A `cfw_ws_*` module (handshake
  validation, frame codec, masking, ping/pong, close handshake) is a
  follow-up that consumes this API and gets its own audit pass per
  catalog item 3.

## Consequences

- Protocol upgrades become possible without new dependencies; the
  boundary is byte-stream level, matching the TLS deferral rationale
  (ADR 0030) — any upgraded protocol is the application's
  responsibility, including its own security review.
- The HTTP parser never sees post-upgrade bytes: the pending-buffer
  handoff guarantees pipelined protocol data is delivered intact, and
  `service_read` stops at upgrade so leftover bytes cannot be
  reinterpreted as a second HTTP request.
- Applications must handle `CFW_UPGRADE_EV_HUP`/`ERR` by calling
  `cfw_upgraded_close`; ignoring them can spin the event loop until the
  idle deadline fires (documented contract, same as handler errors).
- `max_out_size` now also bounds upgraded-protocol send buffering;
  applications with larger frames fragment at the protocol layer.
- ABI/API impact: additive only — new option fields (zero-initialized
  by existing `memset` options pattern), new opaque typedef, new
  functions. No existing behavior changes when `upgrade` is unset.
- Follow-up: WebSocket module ADR + implementation; fuzz seeds for
  upgrade handshakes; audit catalog item 3 coverage for the takeover
  path.
