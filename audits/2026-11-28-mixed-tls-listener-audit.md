<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: mixed cleartext/TLS listener sniffing (ADR 0061)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: c82554d
- Category: audit catalog items 1 (memory safety), 3 (parser/protocol
  state machines), 4 (API misuse), 8 (cryptography and transport)
- Scope: `src/cfw/server.c` (`cfw_server_conn_attach_tls`,
  `cfw_server_service_conn` detect gate, accept path),
  `include/cfw/server.h`, `tests/unit/test_server.c`

## Goals

- Confirm first-byte protocol detection cannot misclassify traffic or
  consume bytes belonging to either protocol.
- Verify the detect state cannot leak resources or desynchronize poll
  interest (a connection stuck pre-decision must still time out).
- Confirm TLS attachment on the detect path has identical lifetime
  semantics to the immediate path (fd owned by `http1_conn`,
  `close_notify` on teardown).

## Method

- Code review of the detect gate: single `recv(MSG_PEEK)` of one
  byte; `0x16` → `cfw_server_conn_attach_tls`; other → cleartext;
  `n == 0`/error → teardown; `EAGAIN` → keep waiting.
- Analysis of classification safety: HTTP/1.1 request lines begin
  with an uppercase method token (never `0x16`); TLS record layer
  always begins client handshakes with content type 22 (`0x16`).
  SSLv2 ClientHello (`0x80`) is intentionally unhandled — the TLS
  floor is 1.2.
- Verified the peeked byte is never consumed (`MSG_PEEK`), so the
  subsequently chosen protocol reads its own byte stream intact —
  including the TLS ClientHello and the cleartext request line.
- New tests: `server_tls_detect_mixed` (cleartext GET + full TLS
  session on one port), `server_tls_detect_requires_ctx`
  (INVALID_ARGUMENT guard, runs in every build).

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | medium | src/cfw/server.c accept | `cfw_http1_conn_init` failure path leaked the accepted fd (pre-existing; init fails before taking ownership) | fixed in c82554d — fd now closed on init failure |
| A-002 | low | src/cfw/server.c detect | A client that connects and sends nothing keeps the conn until `request_timeout` — same bound as any stalled cleartext conn | accepted — deadline armed at accept covers the detect state |
| A-003 | info | docs/decisions/0061 | Operators must not treat a detect port as "always encrypted"; cleartext peers get cleartext service by design | accepted — documented in ADR and header |

## Verification

```text
cmake --build --preset debug-tls
./build/debug-tls/tests/cfw_unit_tests --filter tls_detect
# 2 executed, 0 failed (mixed cleartext+TLS on one port)
ctest --preset debug-tls --output-on-failure  # 6/6
ctest --preset debug --output-on-failure      # 6/6
```

## Follow-up

- Session resumption policy tuning remains deferred (OpenSSL
  server-side session cache is enabled by default).
- An opt-in strict mode (fatal alert / reject cleartext on a detect
  port) is application policy; the framework exposes enough state
  (`tls_detect` decision is internal, ALPN/`cfw_tls_conn_alpn` for
  post-handshake checks) for apps to enforce it.
