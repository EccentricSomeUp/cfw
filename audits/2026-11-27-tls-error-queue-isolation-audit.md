<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: TLS error-queue isolation fix

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: pending (see changelog)
- Category: 4 API misuse/lifetime, 5 concurrency/cancellation,
  8 cryptography and transport
- Scope: `src/cfw/tls.c` (`cfw_tls_handshake`, `cfw_tls_read`,
  `cfw_tls_write`, `cfw_tls_shutdown`),
  `tests/unit/test_tls.c` (`tls_error_queue_isolation`),
  `tests/bench/bench_server.c` (TLS bench mode used as reproducer)

## Goals

- Diagnose and fix a multi-threaded TLS failure where healthy
  connections were killed mid-run under `cfw_bench ... tls` with
  4 reuse-port workers (~75-95% failure rate per run).
- Verify the fix under stress and lock in regression coverage.

## Method

- Reproduced with the HTTPS benchmark (`cfw_bench 3000 200 4 tls`)
  against generated test certificates.
- A/B instrumentation: debug prints and `ERR_get_error()`/
  `ERR_peek_error()` probes masked the failure; recording diagnostics
  without draining the queue reproduced it. This identified OpenSSL's
  per-thread error queue as the mechanism.
- Root cause: OpenSSL's error queue is per-thread, not per-`SSL`.
  Errors left on a worker thread's queue by one connection's teardown
  (e.g. `unexpected eof` / `shutdown while in init` after an abrupt
  peer close) make `SSL_get_error` return `SSL_ERROR_SSL` for
  unrelated transient results (`ret <= 0` with `errno == 0`, e.g.
  TLS 1.3 post-handshake processing). `cfw_tls_map_error` then
  reported `CFW_ERROR_IO`, and the server marked the healthy
  connection dead.
- Fix: `ERR_clear_error()` before every `SSL_*` operation in
  `cfw_tls_handshake`, `cfw_tls_read`, `cfw_tls_write`, and
  `cfw_tls_shutdown`, so each call classifies only its own outcome.
  This matches OpenSSL's documented guidance for reusing threads
  across `SSL` objects.
- Stress verification: 20/20 consecutive 4-worker TLS bench runs
  pass after the fix (previously ~1/20).

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | high | src/cfw/tls.c | Stale per-thread OpenSSL error-queue entries poisoned `SSL_get_error`, killing unrelated TLS connections on the same thread | fixed |
| A-002 | low | tests/unit/test_tls.c | `tls_error_queue_isolation` exercises cross-connection isolation at the API level; the `errno == 0` poison path is timing-dependent and not deterministically reproduced in a unit test | accepted (bench stress is the reproducer) |

## Verification

```text
cmake --build --preset debug-tls
ctest --preset debug-tls --output-on-failure   # 6/6 pass
for i in $(seq 1 20); do cfw_bench 3000 200 4 tls; done
# 20/20 pass (was ~1/20 before the fix)
```

## Follow-up

- Consider TLS 1.3 session-ticket pumping notes already covered by
  ADR 0062; no further action.
