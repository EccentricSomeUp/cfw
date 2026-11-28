<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: transport seam and TLS server listeners (ADR 0058)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: 5545b05
- Category: audit catalog items 1 (memory safety), 4 (parser/protocol
  state machines), 5 (API misuse, ownership, lifetime), 8
  (cryptography and transport)
- Scope: `include/cfw/http1_conn.h`, `src/cfw/http1_conn.c`,
  `include/cfw/server.h`, `src/cfw/server.c`,
  `tests/unit/test_server.c`, `tests/unit/test_http1_conn.c`,
  `tests/integration/test_http_pipeline.c`,
  `docs/decisions/0058-transport-seam-tls-listeners.md`

## Goals

- Confirm the transport override cannot corrupt the cleartext path:
  NULL transport must behave identically to pre-change code.
- Verify fd ownership stays singular: `cfw_tls_conn_t` borrows the fd,
  `cfw_http1_conn_t` owns and closes it exactly once on every exit
  path (init failure, handshake failure, normal close, server stop).
- Confirm handshake gating cannot desynchronize poll interest: during
  the handshake the connection must be serviced only on the
  directions `cfw_tls_conn_want_*` report, and HTTP parsing must not
  begin before `cfw_tls_handshake_done`.
- Verify TLS teardown sends `close_notify` best-effort without
  blocking the event loop, and that `SSL*`/`cfw_tls_conn_t` are freed
  on all conn teardown paths.

## Method

- Code review of `src/cfw/server.c` accept path, `service_conn`
  handshake gate, `update_conn_interest` want-flag mapping, and
  `close_conn` shutdown ordering.
- Code review of `src/cfw/http1_conn.c` read/write/flush paths for
  transport-vs-raw equivalence, including EOF (`done == 0`) and
  `CFW_ERROR_WOULDBLOCK` handling.
- New integration test `server/server_tls_request_response`: full
  TLS handshake plus HTTP request/response driven through
  `cfw_server_step` with a real `SSL*` client.
- Regression: all pre-existing `http1_conn` and server tests run
  unmodified against the raw path.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | high | tests/unit/test_http1_conn.c, tests/integration/test_http_pipeline.c | New optional `transport` field in `cfw_http1_conn_options_t` is garbage when callers field-initialize without `memset`, causing wild-pointer dereference (segfault in `http1_conn_request_response`) | fixed — all in-tree callers now zero-init; header documents the requirement; caught by existing tests |
| A-002 | medium | src/cfw/server.c | Handshake interest must not arm `POLLOUT` unconditionally — a stalled peer would spin the event loop on a writable socket | fixed — interest derived from `cfw_tls_conn_want_write` only |
| A-003 | low | src/cfw/server.c | `cfw_tls_shutdown` on teardown may attempt a blocking-free `close_notify` write; bounded by nonblocking socket, failure ignored | accepted — best-effort by design, fd closes regardless |
| A-004 | info | include/cfw/http1_conn.h | `cfw_http1_conn_options_t` grew by one pointer; pre-1.0 ABI allows additive changes, callers must recompile and zero-init | accepted — documented in header and changelog |

## Verification

```text
cmake --preset debug-tls && cmake --build --preset debug-tls
ctest --preset debug-tls --output-on-failure
# 6/6 passed (includes server/server_tls_request_response)
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure
# 6/6 passed (cleartext path unchanged)
cmake -S . -B build/san-tls -DCMAKE_BUILD_TYPE=Debug \
    -D'CFW_ENABLE_SANITIZERS=ON' -D'CFW_ENABLE_TLS=ON'
cmake --build build/san-tls && ctest --test-dir build/san-tls
# 5/5 passed under ASan+UBSan
```

## Follow-up

- ALPN selection, session resumption policy, and a TLS example remain
  deferred (ADR 0057/0058 follow-ups).
- Mixed cleartext+TLS listeners on one port (ALPN or port-per-
  listener) are out of scope; `tls_ctx` applies to all accepted
  connections.
- Client-certificate authentication plumbing (verify callback →
  request context) is application-level follow-up work.
