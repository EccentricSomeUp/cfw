<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0029: Server lifecycle and connection management

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 5 needs a top-level server object tying together the listener,
pollset, per-connection `cfw_http1_conn_t` readers, the dispatch
pipeline, timeouts, and graceful shutdown. The design must stay
dependency-free, single-threaded per server (matching the pollset
contract), bounded under connection floods, and testable without real
time sleeps where possible.

## Decision

Add `cfw/server.h` + `src/cfw/server.c` (POSIX-only; Windows stubs
report `CFW_ERROR_UNAVAILABLE`, like `http1_conn`):

- **`cfw_server_t`** owns the listening socket (created internally via
  the private net layer — host must be a numeric IP or NULL for
  any-address; no DNS in v1), an internal pollset, a self-pipe wakeup
  channel, and a linked list of connection records.
- **Configuration** (`cfw_server_options_t`): dispatch pointer (required),
  HTTP reader limits, per-connection output cap, `max_connections`,
  `keep_alive_timeout` (idle between requests; 0 disables),
  `request_timeout` (head+body deadline; 0 disables), per-request header
  and param scratch sizes, and an optional `cfw_logger_t`.
- **Event loop:** `cfw_server_run` loops `cfw_server_step` until
  `cfw_server_stop` is called (the wake pipe makes stop prompt even
  while polling). `cfw_server_step(server, wait_deadline)` performs one
  poll iteration for embedding and tests; `wait_deadline == NULL` waits
  until the nearest connection deadline.
- **Accept policy:** the listener is only registered for POLL_IN while
  `conn_count < max_connections`; accepts beyond the limit are closed
  immediately. This bounds fd/memory use without busy-looping.
- **Per-connection state:** `cfw_http1_conn_t` plus accumulated body
  buffer (bounded by `max_body_size`), parsed-head copy, header/param
  scratch arrays, output queue, and a single `deadline` field reused
  for request and keep-alive phases.
- **Request flow:** HEAD event stores the head and arms the request
  deadline; BODY events append to the accumulation buffer; MESSAGE_DONE
  runs `cfw_dispatch_request`, serializes the response head
  (`cfw_dispatch_response_head_format`), queues head + body, and
  registers POLL_OUT until the queue drains. Protocol ERROR events map
  to 400/413 responses before close.
- **Timeouts:** each step computes the soonest connection deadline and
  caps the poll wait; expired connections are closed and freed. Idle
  keep-alive connections time out per `keep_alive_timeout`.
- **Graceful stop:** `cfw_server_stop` stops accepting (listener
  unregistered), wakes the loop, and `run` returns. Existing connections
  are drained by continued `cfw_server_step` calls or closed by
  `cfw_server_free`. In-flight requests are not force-completed; this is
  documented as drain-on-free semantics for v1.
- **Diagnostics:** the server counts accepted connections, completed
  requests, and protocol errors, exposed via accessors and optional
  logger events.

## Consequences

- Single-threaded only; multi-threaded serving requires one server per
  thread with SO_REUSEPORT (deferred).
- No DNS-based listen addresses and no TLS (TLS remains an unapproved
  dependency decision).
- Body accumulation means request bodies are buffered whole per
  connection — bounded by `max_body_size`; streaming handlers remain a
  future extension.
- `cfw_server_step` with a zero/near-zero deadline gives deterministic
  test driving without sleeps beyond poll granularity.
