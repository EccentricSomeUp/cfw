<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: portable test suite port (ADR 0076)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: <pending — audit written pre-commit; range covers the
  tests/ portability diff>
- Category: memory safety, API misuse/ownership, platform behavior,
  denial-of-service, test integrity
- Scope: `tests/unit/test_loopback.h` (new), `test_fd.c`,
  `test_net.c`, `test_pollset.c`, `test_http1_conn.c`, `test_ws.c`,
  `test_server.c`, `test_static.c`, `test_tls.c`,
  `tests/integration/test_http_client.c`, `test_http_pipeline.c`,
  `test_ws_client.c`, `tests/security/test_smuggling.c`,
  `.github/workflows/ci.yml`

## Goals

- Verify the ported tests preserve their original assertions and
  timing budgets, introduce no flaky or weakened checks, and that
  enabling full `ctest` on Windows does not mask real failures.

## Method

- Audit-catalog checks:
  - Memory safety: helpers use only stack buffers and `cfw_fd_t`
    ownership; `cfw_test_socketpair` releases partial state on every
    failure path and invalidates wrappers after handing raw handles
    to TLS conns (borrowed-fd contract preserved).
  - Integer/bounds: `ssize_t`→`intptr_t` preserves the -1 sentinel;
    all recv loops keep their original capacity guards
    (`capacity - used - 1`).
  - API misuse/ownership: `cfw_test_close_raw` is used only for
    handles extracted via `cfw_fd_get`/`cfw_fd_release` whose owning
    wrapper was invalidated; no double-close paths introduced.
  - Platform behavior: `cfw_test_socketpair` uses blocking connect +
    restore-nonblocking for determinism (same pattern as the server
    wake channel, ADR 0074); WSAPoll/epoll/kqueue all consume the same
    `cfw_poll_event_t` contract.
  - Denial-of-service: retry budgets unchanged or widened; the
    `test_ws_client` fake server's `SO_RCVTIMEO` was replaced by
    bounded attempt loops (20 000 × 0.5 ms ≈ 10 s, matching the
    original timeout).
  - Test integrity: no assertion was weakened; `static_symlink_
    confinement` and `SO_REUSEPORT` clusters remain gated with
    documented reasons rather than silently passing. (Corpus replay
    was later made portable via a configure-time file list; see the
    follow-up note below.)
- `cmake --preset debug && cmake --build --preset debug` and
  `ctest --preset debug` on Linux: 6/6 pass, including the fully
  ported server/WS/TLS/integration/security suites.
- `cmake --preset debug-tls`: 6/6 pass with `test_tls.c` now driving
  TLS over TCP loopback.
- `tests/windows_smoke.c` still compiles `-Wall -Wextra -Werror`
  against both builds.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| P-001 | medium | Windows CI `windows`/`windows-tls` jobs | Full `ctest` on Windows is enabled without local Windows execution; timing-sensitive tests (sync, server drain, WS soak) may expose platform-timing flakes on first CI run. | deferred — first CI run will show; flakes get individual follow-up |
| P-002 | low | `cfw_test_socketpair` | TCP loopback requires a real handshake vs `AF_UNIX` immediacy; blocking-connect makes it deterministic, and all consumers already retry on WOULDBLOCK. | accepted |
| P-003 | info | `test_tls.c` | TLS conns now run over TCP loopback; coverage identical, transport marginally heavier. | accepted |
| P-004 | info | `test_static.c` | Symlink setup is best-effort on Windows; the only symlink-dependent test stays POSIX-gated. | accepted |

## Verification

```text
cmake --preset debug && cmake --build --preset debug     # clean
ctest --preset debug --output-on-failure                 # 6/6 passed
cmake --preset debug-tls && cmake --build --preset debug-tls  # clean
ctest --preset debug-tls --output-on-failure             # 6/6 passed
# Windows: CI windows + windows-tls jobs now run full ctest
```

## Follow-up

- Triage any Windows-only test failures from the first CI run
  (P-001); each gets a fix or a documented gate.
- ~~Consider `_findfirst`-based corpus replay~~ — done differently:
  the corpus file list is generated at CMake configure time
  (`cfw_test_corpus_files.h`), so replay needs no directory API at
  all and runs on every platform.
- `CreateSymbolicLink` confinement coverage on Windows remains a
  possible follow-up; current coverage documents best-effort
  confinement (ADR 0037).
