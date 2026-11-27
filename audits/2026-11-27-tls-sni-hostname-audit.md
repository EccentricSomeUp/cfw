<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: TLS client SNI and hostname verification (ADRs 0064/0066)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: dcec63a, faf4a14
- Category: 8 cryptography and transport, 4 API misuse/lifetime
- Scope: `src/cfw/tls.c` (`cfw_tls_conn_set_server_name`,
  `cfw_tls_conn_get_server_name`, conn `server_side`/
  `handshake_started` tracking), `include/cfw/tls.h`,
  `tests/unit/test_tls.c`

## Goals

- Verify the new client-side SNI/hostname-verification API closes the
  chain-only verification gap without weakening existing behavior.

## Method

- Review of `cfw_tls_conn_set_server_name`: rejects NULL/empty/>255
  names, server-side connections, and post-handshake calls; applies
  `SSL_set_tlsext_host_name` (SNI) and `SSL_set1_host` (hostname pin
  into `X509_check_host` during `verify_peer` chain validation).
- Confirmed `verify_peer` semantics unchanged when
  `set_server_name` is not called (chain-only, as documented).
- `cfw_tls_conn_get_server_name` returns a borrowed view into
  session memory via `SSL_get_servername`; empty view when no SNI —
  no ownership transfer, consistent with `cfw_tls_conn_alpn`.
- Test coverage: accepted hostname (CN fallback), rejected wrong
  hostname (handshake IO failure), invalid-argument matrix,
  get-server-name present/absent on both roles.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | low | include/cfw/tls.h | `verify_peer` alone does not check hostname — callers must opt in via `set_server_name`; documented on both APIs | accepted, documented |
| A-002 | info | src/cfw/tls.c | `handshake_started` flag added to conn state; set on first `cfw_tls_handshake` call, gates `set_server_name` | fixed |

## Verification

```text
ctest --preset debug-tls   # 6/6 pass incl. 5 server_name tests
ctest --preset debug       # 6/6 pass (stub branch)
```

## Follow-up

- IDN/punycode remains the caller's responsibility (delegated to
  OpenSSL `SSL_set1_host` semantics), as recorded in ADR 0064.
