<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0076: Portable test suite over TCP loopback

## Status

Accepted.

## Context

With the socket, pollset, fd, server, and TLS layers all functional on
Windows (ADRs 0072–0075), the remaining platform gap was the test
suite itself: roughly half of the unit/integration/security files were
wrapped in `#if CFW_PLATFORM_POSIX` because they used `socketpair`,
`pipe`, raw `send`/`recv`/`close`, `fcntl`, `usleep`, `errno`,
`memmem`, `sigaction`/`setitimer`, and raw `sockaddr` plumbing.

## Decision

Port the suite to run on every platform with a socket backend:

- **`tests/unit/test_loopback.h`** provides shared helpers included
  only by translation units that need them (avoiding unused-function
  warnings):
  - `cfw_test_socketpair(cfw_fd_t out[2])` — a connected, nonblocking
    TCP loopback pair built entirely from `cfw_net_*`, the portable
    stand-in for `socketpair(AF_UNIX)`/`pipe`.
  - `cfw_test_sleep_us` — `nanosleep`/`Sleep` wrapper replacing
    `usleep`.
  - `cfw_test_close_raw` — platform-correct close for raw handles
    obtained via `cfw_fd_get`/`cfw_fd_release`.
  - `cfw_test_memmem` — portable `memmem` (GNU-only otherwise).
- **Raw I/O call sites** become `cfw_net_send`/`cfw_net_recv` loops or
  thin `cfw_test_*_recv` shims preserving the original ">0 bytes, 0
  EOF, -1 retry" contract. `ssize_t` becomes `intptr_t`.
- **`test_server_run_and_drain`** replaces the `SIGALRM`+`setitimer`
  stop mechanism with a `cfw_thread` that sleeps then calls
  `cfw_server_stop` — the supported cross-thread shutdown path the
  wake channel already provides.
- **`test_ws_client.c`'s fake server** is rewritten on `cfw_net_*`
  with bounded nonblocking retry loops replacing `SO_RCVTIMEO`.
- **`test_tls.c`** keeps its full case matrix but drives TLS conns
  over `cfw_test_socketpair` TCP loopback instead of `AF_UNIX`
  socketpairs; descriptor tracking uses `intptr_t` +
  `cfw_test_close_raw`.
- **Deliberately still gated**: `static_symlink_confinement` (POSIX
  `O_NOFOLLOW`/`openat` semantics; Windows confinement is documented
  best-effort), the fuzz-corpus replay (`dirent.h`; mutation fuzzer
  runs everywhere), the directory-is-not-a-file `fs` check, and
  `SO_REUSEPORT` cluster tests (Windows has no equivalent; the
  `!CFW_HAVE_SO_REUSEPORT` fallback already asserts `UNAVAILABLE`).

## Consequences

- `ctest` runs the full suite on `windows-latest` in both the `debug`
  and `debug-tls` presets; the `cfw_windows_smoke` target remains as a
  minimal public-API sanity check.
- TCP loopback has marginally different semantics than `AF_UNIX`
  socketpairs (real handshake, possible WOULDBLOCK on connect —
  handled by blocking-connect-then-restore in the helper); all retry
  budgets were kept or widened.
- Any future POSIX-only test must justify the gate in its ADR/change
  description.
