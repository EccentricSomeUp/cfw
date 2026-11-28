<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: upgraded-protocol I/O over TLS transports (ADR 0058 follow-up)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: 9dafee9
- Category: audit catalog items 1 (memory safety), 4 (protocol state
  machines), 8 (cryptography and transport)
- Scope: `cfw_upgraded_recv`/`cfw_upgraded_send` in `src/cfw/server.c`,
  `tests/unit/test_server.c`

## Goals

- Confirm protocol-upgrade connections (WebSocket and the generic
  `upgrade` hook) honor the `cfw_http1_transport_t` seam so `wss://`
  works and ciphertext is never exposed to upgraded protocols.
- Verify staged `upgrade_pending` bytes (already-decrypted HTTP-reader
  leftovers) and SSL-internal buffered plaintext are both delivered in
  order with no loss or duplication.

## Method

- Code review of `cfw_upgraded_recv`: it called raw `recv()` on the
  fd, bypassing `conn->http.transport` — on a TLS connection the
  upgraded protocol would receive ciphertext (functional break and a
  confidentiality/confusion risk). `cfw_upgraded_send` was already
  safe because it queues through `cfw_http1_conn_flush`, which honors
  the transport.
- Fix: `cfw_upgraded_recv` now routes through
  `transport->read` when configured, preserving the exact
  bytes/EOF/WOULDBLOCK/fatal contract of `recv`.
- New test `server/server_tls_upgrade_echo`: TLS handshake, HTTP 101
  upgrade, then raw protocol bytes echoed through the upgraded
  connection — fails before the fix (memcmp on ciphertext), passes
  after.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | high | src/cfw/server.c `cfw_upgraded_recv` | Upgraded-protocol reads bypassed the transport, so `wss://` connections would feed TLS ciphertext to the application protocol | fixed — transport-aware read added; regression test proves plaintext delivery |

## Verification

```text
cmake --build --preset debug-tls
./build/debug-tls/tests/cfw_unit_tests --filter server_tls_upgrade_echo
# FAIL before fix (ciphertext echoed), PASS after
ctest --preset debug-tls --output-on-failure  # 6/6
ctest --preset debug --output-on-failure      # 6/6
```

## Follow-up

- `wss://` now works end-to-end via the generic upgrade hook;
  `cfw/ws.c` benefits automatically since it consumes
  `cfw_upgraded_recv`/`cfw_upgraded_send`.
- ALPN (`http/1.1` vs future protocols) remains deferred per ADR
  0058.
