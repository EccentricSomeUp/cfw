<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: POSIX platform parity (macOS/BSD misalignment sweep)

- Date: 2026-11-26
- Auditor: Devin (agent)
- Commit: afdb0aa
- Category: audit catalog §7 (platform-specific behavior), §1 (memory
  safety — resource-handle lifetime on error paths), §6 (API misuse —
  feature-test-macro contract)
- Scope: every translation unit with platform-conditional code:
  `src/cfw/private/net.c`, `src/cfw/private/pollset.c`,
  `src/cfw/private/pollset.h`, `src/cfw/fs.c`, `src/cfw/random.c`,
  `src/cfw/time.c`, `src/cfw/sync.c`, `src/cfw/thread.c`,
  `src/cfw/server.c`, `src/cfw/http1_conn.c`, `src/cfw/fd.c`,
  `src/cfw/platform.c`, `src/cfw/log.c`, plus the POSIX sections of the
  unit/integration/security test suites, `tests/CMakeLists.txt`, and
  `cmake/cfw_compile_options.cmake`.

## Goals

- Find every remaining instance of the failure class that produced the
  macOS CI fixes (Linux/glibc assumptions leaking into shared POSIX
  code) *before* the next CI round-trip, not one error at a time.
- Verify feature-test-macro interactions: `_POSIX_C_SOURCE` hides
  BSD/Darwin extensions on macOS (`O_NOFOLLOW`, `SO_NOSIGPIPE`,
  `MSG_DONTWAIT`, `arc4random_buf`, ...) unless `_DARWIN_C_SOURCE` is
  also defined — confirmed against Apple Libc/XNU headers.
- Verify socket-flag portability: `SOCK_NONBLOCK`/`SOCK_CLOEXEC`/
  `accept4`/`MSG_NOSIGNAL` are Linux-only or conditional; `fcntl`
  fallbacks must cover macOS/BSD.
- Verify the `CFW_POLLF_FORCE_POLL` test override actually reaches the
  `poll()` backend on every POSIX host, not just Linux.

## Method

- Enumerated all `_WIN32`/`__APPLE__`/`__linux__`/`_POSIX_C_SOURCE`/
  `_GNU_SOURCE` conditionals (107 sites) and audited each POSIX branch
  for symbols that are absent or feature-hidden on macOS/BSD.
- Cross-checked suspect symbols against Apple open-source headers
  (xnu `bsd/sys/socket.h`, Libc `include/_time.h`, `stdlib.h` guard
  rules) and glibc `sys/random.h`.
- Reviewed error paths for fd leaks introduced by new `fcntl` calls.
- Linux debug + sanitizers builds and full `ctest` runs.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | high | `src/cfw/server.c` `cfw_server_init` | `socketpair(AF_UNIX, SOCK_STREAM \| SOCK_NONBLOCK, ...)` — `SOCK_NONBLOCK` is undefined on macOS/BSD: compile failure | fixed in afdb0aa: plain `SOCK_STREAM` + `fcntl` `O_NONBLOCK`/`FD_CLOEXEC` on both ends |
| A-002 | high | `src/cfw/random.c` | `arc4random_buf` declaration hidden by `_POSIX_C_SOURCE 200809L` without `_DARWIN_C_SOURCE` on macOS: implicit-declaration error under `-Werror` | fixed in afdb0aa: define `_DARWIN_C_SOURCE` on `__APPLE__` |
| A-003 | high | `src/cfw/private/net.c` | `SO_NOSIGPIPE` hidden the same way, so `cfw_net_set_nosigpipe` silently no-oped on macOS: a client disconnect mid-response could raise `SIGPIPE` and kill the server | fixed in afdb0aa: define `_DARWIN_C_SOURCE` on `__APPLE__` |
| A-004 | medium | `src/cfw/fs.c` | `O_NOFOLLOW` hidden under `_POSIX_C_SOURCE` on macOS: `cfw_file_open_confined` returned `CFW_ERROR_UNAVAILABLE`, breaking static file serving and `fs_file_open_confined`/`static_symlink_confinement` tests on macOS | fixed in afdb0aa: define `_DARWIN_C_SOURCE` on `__APPLE__` |
| A-005 | medium | `src/cfw/server.c` `cfw_server_step` | `recv(..., MSG_DONTWAIT)` — `MSG_DONTWAIT` is hidden on strict-POSIX BSDs; the wakeup fd is nonblocking anyway | fixed in afdb0aa: `MSG_DONTWAIT` where defined, `0` otherwise |
| A-006 | low | `src/cfw/server.c` `cfw_server_init` | wakeup `socketpair` fds lacked `SO_NOSIGPIPE`; `cfw_server_stop`'s `send()` could `SIGPIPE` on macOS if the read end were ever closed first | fixed in afdb0aa: `SO_NOSIGPIPE` applied where `MSG_NOSIGNAL` is absent |
| A-007 | medium | `src/cfw/private/pollset.c` backend selection | `CFW_POLLF_FORCE_POLL` only suppressed the epoll branch, so on macOS/BSD the `cfw_pollset_poll_tests` binary silently still used kqueue — the `poll()` fallback was never compiled or tested there | fixed in afdb0aa: override takes precedence on all non-Windows POSIX |
| A-008 | info | `src/cfw/private/pollset.h` | doc comment predated ADR 0052 ("poll() elsewhere on POSIX") | fixed in afdb0aa |
| A-009 | info | `src/cfw/time.c`, `sync.c`, `thread.c` | `_POSIX_C_SOURCE` users audited: `clock_gettime`/`CLOCK_*`/`pthread_*` are POSIX-visible on macOS; no hidden symbols | verified, no change |
| A-010 | info | `src/cfw/http1_conn.c` | `send`/`recv` paths rely on `SO_NOSIGPIPE` (via A-003) or `MSG_NOSIGNAL`; fd nonblocking makes `recv` flags unnecessary | verified via A-003 fix |
| A-011 | info | `tests/` POSIX sections | `MSG_DONTWAIT` uses in tests are visible on macOS (no feature-test macro defined in test TUs); sockets are nonblocking regardless | accepted: latent strict-BSD hazard only, not a CI target |

## Verification

```text
cmake --preset debug && cmake --build --preset debug   # clean, -Werror
ctest --preset debug --output-on-failure              # 6/6 pass
cmake --preset sanitizers && cmake --build --preset sanitizers
ctest --preset sanitizers --output-on-failure         # 5/5 pass
```

macOS-specific behavior was verified by header inspection (XNU
`socket.h`/`fcntl.h`, Apple Libc `stdlib.h`/`_time.h`), not by a macOS
build; the next `macos-latest` CI run is the end-to-end check.

## Follow-up

- Confirm the `macos-latest` CI matrix leg goes green on the next push.
- If *BSD CI is ever added, re-check `MSG_DONTWAIT` uses in tests (A-011)
  and `__DragonFly__` coverage in `random.c`/`platform.c`.
