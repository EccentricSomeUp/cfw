<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0024: HTTP/1.1 server message reader

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 3 requires keep-alive, pipelining, and bounded resource behavior.
The head reader (ADR 0023), framing rules (ADR 0020), and body decoders
(ADR 0021) are primitives; the server needs one object that drives them
as a per-connection state machine over fragmented transport input.

## Decision

Add `cfw_http1_server_t` in `cfw/http1.h` — a pull-based, allocation-free
(per-connection struct) state machine:

- **Feed/event split:** `cfw_http1_server_feed` buffers transport bytes;
  `cfw_http1_server_next_event` produces `HEAD`, `BODY`, `MESSAGE_DONE`,
  or `ERROR` events. Events must be drained before the next feed.
- **Zero-copy bodies:** `BODY` spans view server-owned pending storage;
  they remain valid until the next `feed` call.
- **State machine:** `HEAD → (BODY_CL | BODY_CHUNKED) → HEAD … → CLOSED`.
  Framing is resolved once per head; `Content-Length` bodies emit
  contiguous spans, chunked bodies flow through the ADR 0021 decoder.
- **Keep-alive:** HTTP/1.1 defaults persistent; HTTP/1.0 defaults closed.
  The `Connection` header's comma-separated tokens (`close`,
  `keep-alive`) override the default, case-insensitively.
- **Pipelining:** after `MESSAGE_DONE`, buffered bytes are parsed as the
  next request head automatically; `feed` reports post-body bytes as
  ordinary input.
- **Resource limits:** `max_head_size` (per request head),
  `max_body_size` (per message; `0` = unlimited, checked for both
  Content-Length up front and incrementally for chunked), and
  `max_pending_size` (total buffered input per connection — bounds memory
  against fast clients and pipelined floods). Violations produce
  `CFW_HTTP1_ERR_PROTOCOL` (400) or `CFW_HTTP1_ERR_BODY_TOO_LARGE` (413)
  and latch the connection closed.
- **Fail-closed:** any parse/framing/decode error latches `failed`;
  subsequent `feed`/`next_event` calls return `CFW_ERROR_WOULDBLOCK`.

## Alternatives considered

- **Callback/push API:** rejected; pull events compose better with the
  pollset loop and keep lifetime rules explicit.
- **Retaining a separate read buffer per body type:** rejected; one
  pending buffer with a consume offset serves head, CL body, chunked
  framing, and pipelined data uniformly.
- **Eager full-message buffering:** rejected; streaming `BODY` events
  bound memory regardless of body size.

## Consequences

- One struct (~4.3 KiB, dominated by the chunked line buffer) per
  connection; all limits are caller-configured at init.
- Response writing and routing remain Phase 4; the reader only consumes
  requests.
- Socket/TLS integration is a thin adapter: `recv → feed`,
  `next_event → dispatch`.
