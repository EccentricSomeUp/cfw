<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: WebSocket over TLS (wss://)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: pending (pre-commit)
- Category: parser/protocol state machines; platform-specific behavior
- Scope: `cfw_ws_*` over `cfw_server_t` TLS listeners; upgrade path
  (ADR 0034) riding the `cfw_http1_transport_t` seam (ADR 0058);
  `examples/ws_echo.c`; `tests/unit/test_server.c`
  (`server_tls_ws_echo`)

## Goals

- Verify the claim (already present in `docs/architecture.md`) that
  upgraded protocols — specifically the full RFC 6455 stack — work
  end-to-end over TLS listeners.
- Exercise masked client frames, handshake accept-key computation,
  echo, and ping/pong through real TLS records rather than inferring
  transport-agnosticism from unit tests with fake connections.
- Confirm no cleartext assumptions remain in `cfw/ws.h` /
  `examples/ws_echo.c` documentation.

## Method

- New unit-suite test `server_tls_ws_echo` (debug-tls preset):
  server `tls_ctx` + `upgrade` hook owning a real `cfw_ws_t`; client
  drives `cfw_tls_conn_t` handshake/write/read while stepping the
  server, sends the RFC 6455 §1.3 example handshake (known-answer
  accept key `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`), a masked text frame,
  and a masked ping; asserts the unmasked echo frame
  (`\x81\x08wss-echo`) and pong (`\x8a\x02hi`).
- Manual smoke: `cfw_example_ws_echo` with the generated test
  certificates; `openssl s_client` completed the TLS handshake and
  received `101 Switching Protocols` with the correct accept key.
- Review: the `cfw_ws_*` module only sees decrypted plaintext via
  `cfw_upgraded_recv`/`cfw_upgraded_send`; frame parsing, masking
  rules, UTF-8 validation, and close handling are unchanged and were
  already covered by `tests/unit/test_ws.c`.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| W-001 | info | include/cfw/ws.h, examples/ws_echo.c | Docs claimed wss:// was deferred pending ADR 0030; TLS has since landed, making the note stale. | fixed (docs updated, ADR 0070) |

No memory-safety, framing, or protocol issues found: the ws module
operates on the same bounded buffers regardless of transport, and
TLS record boundaries are invisible above the transport seam.

## Verification

```text
cmake --build --preset debug-tls --target cfw_unit_tests
./build/debug-tls/tests/cfw_unit_tests
  → PASS server/server_tls_ws_echo; summary: 336 executed, 0 failed
cmake --build --preset debug --target cfw_example_ws_echo   (clean)
cmake --build --preset debug-tls --target cfw_example_ws_echo (clean)
manual: openssl s_client → HTTP/1.1 101 + Sec-WebSocket-Accept
```

## Follow-up

- Client-side WebSocket remains unimplemented; revisit only with a
  concrete consumer (mirrors the staged discipline of ADR 0068).
