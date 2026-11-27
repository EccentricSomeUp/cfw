<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: WebSocket client (`cfw/ws_client.h`)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: pending (pre-commit)
- Category: parser/protocol state machines; memory safety; API
  misuse/ownership
- Scope: `include/cfw/ws_client.h`, `src/cfw/ws_client.c`,
  `src/cfw/private/ws_proto.*` (extracted from `src/cfw/ws.c`),
  `tests/integration/test_ws_client.c`, `examples/ws_client.c` —
  per audit catalog focus "websocket or upgrade paths"

## Goals

- Verify the client frame parser enforces RFC 6455 §5 rules in the
  client role: inbound frames MUST be unmasked, outbound frames MUST
  be masked, control frames ≤125 bytes and unfragmented, UTF-8 on
  text, bounded reassembly, close-code validation.
- Verify the handshake validates `Sec-WebSocket-Accept` as a
  known-answer (SHA-1 + base64 + GUID) and rejects non-101 responses.
- Verify no memory-safety issues in the incremental parser, the
  masked-send path, or the connect/pump/send lifetimes.
- Confirm the ws.c refactor (shared helpers moved to
  `src/cfw/private/ws_proto.*`) is behavior-preserving.

## Method

- New integration suite `tests/integration/test_ws_client.c` against
  a real `cfw_server` ws-echo endpoint: cleartext echo, 40 KiB
  binary message (both directions fragment/reassemble), non-101
  handshake rejection, invalid options, masked-server-frame
  rejection via a raw fake server, and wss:// echo under
  `CFW_HAVE_TLS`.
- Review of the parser state machine line-by-line against the
  server implementation it mirrors (`src/cfw/ws.c`); role differences
  enumerated explicitly (mask direction, close initiation).
- Full preset verification: debug, debug-tls, sanitizers (ASan +
  UBSan).

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| WC-001 | medium | src/cfw/ws_client.c `cfw_wsc_fail` | Close frame was never emitted on protocol failure: `closed` was set before `cfw_wsc_send_frame`, which rejects closed connections. | fixed before commit (send-then-close ordering) |
| WC-002 | low | src/cfw/ws_client.c `cfw_ws_client_send` | Public send path did not arm a fresh deadline, inheriting a stale/expired pump deadline for its bounded write. | fixed before commit (`cfw_wsc_set_deadline` per call) |
| WC-003 | info | src/cfw/ws_client.c `cfw_wsc_handshake_request` | Request target and Host header are length-capped (~640 bytes) and rejected rather than truncated when exceeded. | accepted (documented bound) |
| WC-004 | info | include/cfw/ws_client.h | Blocking `getaddrinfo` is not covered by `timeout`, matching the `cfw_http_client` contract. | accepted (documented) |
| WC-005 | medium | src/cfw/ws_client.c `cfw_wsc_handshake_request` | Request target and caller header name/value were emitted verbatim; SP/CTL bytes in `path` or CR/LF in header fields could inject into the Upgrade request. | fixed post-release (`cfw_http_request_head_format` grammar applied via shared `private/http_fields.h` helpers; rejected with `CFW_ERROR_INVALID_ARGUMENT`) |
| WC-006 | low | src/cfw/ws_client.c `cfw_ws_client_send_close` | Unlike `cfw_ws_client_send`, it did not arm a fresh per-call deadline, inheriting a stale or infinite pump deadline for its bounded write. | fixed post-release (`cfw_wsc_set_deadline` per call) |
| WC-007 | low | src/cfw/ws_client.c `cfw_ws_client_send` (CLOSE) | The generic send path emitted a raw CLOSE payload without validating status/reason, allowing frames a peer must protocol-fail. | fixed post-release (payload validated with `cfw_ws_close_status_ok` + UTF-8 reason check) |
| WC-008 | low | src/cfw/ws_client.c send paths | Sends remained accepted after a close frame was sent (`close_sent` did not gate the public send API), unlike the server-side `closing` guard; a second close could also be queued. | fixed post-release (`close_sent` rejects all public sends) |

No memory-safety, UTF-8, fragmentation, or close-handshake defects
found; sanitizer presets are clean including the new suite.

## Verification

```text
cmake --build --preset debug && ctest --preset debug
  → 6/6 pass; integration 14/14 (5 ws_client cases)
cmake --build --preset debug-tls && ctest --preset debug-tls
  → 6/6 pass; integration 16/16 (+ ws_client_tls_echo wss://)
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers
  → 5/5 pass, no ASan/UBSan reports
```

## Follow-up

- Subprotocol negotiation (`Sec-WebSocket-Protocol`) and extensions
  (permessage-deflate) remain out of scope; revisit with a consumer.
- A ws client has no reconnect/backoff policy — application concern,
  same as `cfw_http_client`.
