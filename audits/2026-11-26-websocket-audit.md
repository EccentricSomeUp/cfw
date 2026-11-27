<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: WebSocket module (ADR 0035)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Scope: `cfw/ws.h`/`src/cfw/ws.c` — handshake helpers (SHA-1, base64,
  validation, accept key), RFC 6455 frame codec (masked receive,
  fragmentation reassembly, UTF-8 validation, control frames), send
  helpers, and the `cfw_upgraded_out_free` addition to `server.h`;
  integration tests in `tests/unit/test_ws.c`
- Method: catalog-driven review per `docs/security/audit-catalog.md`

## Catalog coverage

| # | Area | Result | Evidence |
|---|------|--------|----------|
| 1 | Memory safety | Pass | ASan/UBSan preset green; parser stages at most 14 header bytes in a fixed array; control payloads bounded to 125 bytes; reassembly uses `cfw_buffer_t` with checked growth; `cfw_ws_destroy` frees all state; opaque `cfw_ws_t` prevents field misuse |
| 2 | Integer/bounds | Pass | 64-bit payload lengths validated (MSB must be 0, len 127 path); message-size check `assembly.size > max - len` avoids overflow after the `len > max` early-out; send pre-flight accumulates with `UINT64_MAX` saturation and early exit; frame header writer bounds lengths to the three RFC encodings |
| 3 | Parser/protocol state machine | Pass with notes | State machine covers partial headers/payloads across recv chunks; enforces masking (required client→server), RSV=0, opcode whitelist, control FIN=1 and ≤125 payload, continuation rules (no continuation without open message, no new data opcode mid-message), close-payload length/status/UTF-8 checks; see findings W-101/W-102 |
| 4 | API misuse | Pass | NULL/closed/closing arguments rejected; `cfw_ws_send*` are transactional (pre-flight against `cfw_upgraded_out_free`, nothing queued on `WOULDBLOCK`); message payload is a borrowed span documented as valid until the next event; `cfw_ws_on_event` always drains input so `CFW_UPGRADE_EV_READY` staged bytes are not lost |
| 5 | Concurrency/cancellation | Pass | Single event-loop thread per connection; no shared mutable state; `cfw_ws_t` is per-connection and not thread-safe by design (same contract as the rest of the server) |
| 6 | DoS resistance | Pass | `max_message_size` (default 1 MiB) caps reassembly; oversized single frames fail 1009 at header parse before buffering; control frames capped at 125; auto-pong/close-echo use a single pending-control slot and drop under backpressure rather than queue unboundedly; pending control frames respect `max_out_size` via `cfw_upgraded_out_free` |
| 7 | Platform portability | Pass | Module is platform-neutral C11; POSIX socket paths unchanged; Windows stubs for `cfw_upgraded_*` already return errors, so `cfw_ws_*` fails cleanly there |
| 8 | Cryptography/transport | Pass with notes | SHA-1 and base64 implemented in-tree with known-answer tests; SHA-1 use is confined to the RFC-mandated accept transform and documented as non-security; no TLS (`wss://`) per ADR 0030 |
| 9 | Supply chain/license | Pass | No new dependencies |
| 10 | Documentation/ABI | Pass | ADR 0035 updated to match the shipped API; `dispatch.h` now documents that response-header strviews must outlive the dispatch call (latent footgun found while writing the WS handshake handler); additive API only |

## Findings and dispositions

| ID | Severity | Summary | Disposition |
|----|----------|---------|-------------|
| W-101 | Low | A masked-bit-clear frame was initially unparseable: the header stager always reserved 4 mask-key bytes, so malformed unmasked frames were never rejected | Fixed — stager computes header length from the MASK bit; `ws_unmasked_rejected` covers it |
| W-102 | Low | Single pending-control slot drops additional auto-pongs/close echoes under extreme send-queue backpressure | Accepted — bounded memory beats strict pong-per-ping under DoS pressure; RFC does not mandate pong delivery, and the connection is closing anyway in the close case |
| W-103 | Info | Text UTF-8 is validated on the reassembled message, not incrementally per fragment | Accepted — equivalent accept/reject outcome; sequences spanning fragments are handled correctly because validation runs on the complete message |
| W-104 | Info | `cfw_ws_send` pre-flight uses `cfw_upgraded_out_free`, which reflects queue headroom at call time; concurrent sends from multiple callbacks on the same connection could interleave | Not applicable — callbacks are serialized on the connection's event loop; documented single-threaded contract |
| W-105 | Medium | `cfw_ws_send_close` did not validate `status` (reserved/undefined codes like 1005/999 were emitted), allowed a reason without a status (payload misparsed as a status by the peer), and dereferenced `reason` when `reason_size` was nonzero but `reason` was NULL | Fixed post-release — status checked via `cfw_ws_close_status_ok`, reason-without-status and NULL-reason rejected; covered by new assertions in `ws_send_close_from_app` |

## Verified behaviors (tests)

- RFC 6455 §1.3 accept-key known-answer vector; handshake accept/
  validate accept and reject matrices.
- Live-server integration: 101 handshake with `Sec-WebSocket-Accept`,
  masked text echo, fragmented message reassembly, automatic
  ping→pong, close echo + `cfw_ws_is_closed`, unmasked-frame rejection
  (1002), invalid UTF-8 rejection (1007), `max_message_size` rejection
  (1009), and app-initiated `cfw_ws_send_close`.

## Verification

```text
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure  # 5/5 pass
./build/debug/tests/cfw_unit_tests --filter ws  # 14/14 pass
```
