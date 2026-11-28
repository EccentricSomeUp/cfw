<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: HTTP pipeline and server lifecycle

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: 7d3b387
- Scope: `src/cfw/{http,http1,http1_conn,route,dispatch,server}.c`,
  `include/cfw/{http,http1,http1_conn,route,dispatch,server}.h`,
  `tests/{unit,integration}/*`
- Category: audit catalog 1 (memory safety), 2 (integer/bounds), 3
  (parser/state machines), 5 (concurrency/cancellation), 8 (DoS
  resistance), 9 (platform behavior)

## Goals

- Verify the request path from socket bytes to dispatched response is
  memory-safe and bounded at every stage.
- Confirm connection floods, slowloris patterns, and oversized inputs
  cannot exhaust server resources.
- Check ownership/lifetime rules across conn, dispatch, and arena use.

## Method

- Line-by-line review of server.c event loop, conn lifecycle, deadline
  handling, and all allocation/free paths.
- DoS vector analysis per connection phase (accept, head, body,
  response drain, keep-alive idle).
- `cmake --preset debug`/`sanitizers`/`shared` builds plus `ctest`;
  deterministic parser fuzz suite re-run under sanitizers.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| S-001 | High | `src/cfw/server.c` `cfw_server_handle_done` | The request-phase deadline stayed armed after MESSAGE_DONE; the sweeper could close a connection while its response was still queued for write, truncating replies on `Connection: close` paths | Fixed: deadline is unconditionally re-armed to `keep_alive_timeout` after DONE, which also serves as the drain deadline |
| S-002 | Medium | `src/cfw/server.c` `cfw_server_handle_done` | Non-keep-alive responses were sent without a `Connection: close` header; clients could misinterpret the socket close | Fixed: header added when `close_after_write` is set (failure to add, e.g. full header scratch, is non-fatal) |
| S-003 | Medium | `src/cfw/server.c` `cfw_server_step` | `cfw_pollset_wait` timeout (`CFW_ERROR_TIMEOUT`) was propagated as a step failure, breaking deadline-driven sweeps | Fixed: timeout yields a normal iteration after sweeping |
| S-004 | Info | `src/cfw/server.c` accept path | `cfw_server_find_conn` is O(connections) per event; with very large `max_connections` and busy loops this is quadratic per step | Accepted for v1: bounded by `max_connections`; a handle→conn index is a documented future optimization |
| S-005 | Info | `src/cfw/server.c` drain deadline | When both `keep_alive_timeout` and `request_timeout` are 0, a client that never reads can hold a connection indefinitely (TCP backpressure) | Accepted: operators must configure timeouts; documented in ADR 0029 options. OS-level TCP keepalive remains an operator concern |
| S-006 | Info | `src/cfw/dispatch.c` | Header scratch overflow drops extra headers silently (counted in `headers_dropped`); handlers must check if header completeness matters (e.g. auth) | Accepted: documented contract; security-sensitive middleware should reject `headers_dropped > 0` |
| S-007 | Info | `src/cfw/server.c` `cfw_server_send_error` | Error responses queue without checking `cfw_http1_conn_queue` capacity; on a full out-buffer the error bytes are dropped and the connection closes anyway | Accepted: connection is already terminating; client sees close |

## DoS resistance summary

- **Connection flood:** listener unregistered at `max_connections`; excess
  accepts closed immediately. Per-conn cost bounded (2 mallocs + 2
  buffers + http1 reader).
- **Slowloris (headers):** deadline armed at accept and re-armed at HEAD;
  `request_timeout` bounds head+body total.
- **Slow body trickle:** same single deadline — no per-chunk refresh.
- **Slow reader:** `keep_alive_timeout` doubles as response-drain
  deadline (S-001 fix).
- **Input floods:** `max_pending_size` bounds unread pipelined bytes;
  `max_head_size`/`max_body_size` bound per-message memory; body
  accumulation capped by `max_body_size` before dispatch.
- **Header/param floods:** fixed scratch arrays; overflow counted, never
  written out of bounds.
- **Wake pipe:** single byte per stop; `stopping` flag prevents
  repeated writes.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure        # incl. server_soak
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure
cmake --preset shared && cmake --build --preset shared
ctest --preset shared --output-on-failure
```

All presets pass. `server_soak` exercises 100 sequential keep-alive
requests plus 4 concurrent connections.
