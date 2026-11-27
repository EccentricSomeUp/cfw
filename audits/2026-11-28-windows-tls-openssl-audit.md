<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: TLS on Windows via OpenSSL 3 (ADR 0075)

- Date: 2026-11-28
- Auditor: Devin (agent)
- Commit: 1a716b7
- Category: memory safety, integer/bounds, API misuse/ownership,
  platform behavior, denial-of-service
- Scope: `src/cfw/tls.c` (fd-width guard), `CMakeLists.txt`
  (Windows refusal removal), `tests/unit/test_tls.c` (POSIX gate),
  `tests/windows_smoke.c` (`wsm_tls`), CI `windows-tls` job

## Goals

- Verify enabling the OpenSSL TLS module on Windows introduces no new
  memory-safety or descriptor-handling hazards, and that test
  coverage is not silently weakened.

## Method

- Audit-catalog checks:
  - Memory safety: `tls.c` is unchanged OpenSSL API code; the only new
    logic is a bounds check before `SSL_set_fd`. No new buffers,
    allocations, or lifetime changes.
  - Integer/bounds: Winsock `SOCKET` is `UINT_PTR` (64-bit) while
    `SSL_set_fd` and OpenSSL's socket BIO store the descriptor in an
    `int`. `cfw_tls_conn_create` now rejects `fd` values above
    `INT32_MAX` with `CFW_ERROR_UNAVAILABLE` on Windows, so a
    truncating cast can never reach OpenSSL. POSIX fds are `int` by
    definition and unaffected.
  - API misuse/ownership: `cfw_tls_conn_t` still borrows the
    descriptor; `cfw_fd_t` ownership rules unchanged. The smoke test
    destroys the TLS conn before closing the fd, matching the
    documented order.
  - Platform behavior: OpenSSL's socket BIO maps `WSAEWOULDBLOCK` to
    `SSL_ERROR_WANT_READ`/`WANT_WRITE`; `cfw_tls_conn_want_*` hints
    are preserved. TLS runs over the same nonblocking Winsock sockets
    as cleartext (ADRs 0072–0074).
  - Denial-of-service: no new parsing or allocation surface; TLS
    limits are unchanged (handshake/read/write deadlines are the
    caller's responsibility, same as POSIX).
  - Test coverage: `test_tls.c` is gated to
    `CFW_HAVE_TLS && CFW_PLATFORM_POSIX` because it drives TLS over
    `AF_UNIX` socketpairs; Windows coverage moves to
    `tests/windows_smoke.c::wsm_tls`, which performs a real verified
    handshake and HTTP exchange against `cfw_server` with
    `verify_peer` on. Net coverage is preserved, not reduced.
- `cmake --preset debug-tls && cmake --build --preset debug-tls` and
  `ctest --preset debug-tls` on Linux confirm POSIX TLS behavior is
  unchanged; `tests/windows_smoke.c` was additionally compiled
  `-Wall -Wextra -Werror` against the TLS build and executed
  successfully, exercising `wsm_tls` end-to-end on Linux.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| T-001 | low | `cfw_tls_conn_create` | `SSL_set_fd` stores the descriptor in an `int` inside OpenSSL; SOCKET values above `INT32_MAX` would truncate. | fixed — range guard returns `CFW_ERROR_UNAVAILABLE` on Windows |
| T-002 | medium | Windows CI `windows-tls` job | The choco OpenSSL layout (`C:\Program Files\OpenSSL-Win64`) and PATH/DLL availability for `openssl` CLI and runtime libraries are assumed, not locally verifiable. | deferred — first CI run will confirm; fallback is `OPENSSL_ROOT_DIR`/vcpkg adjustment |
| T-003 | info | `tests/unit/test_tls.c` | Suite remains POSIX-only (AF_UNIX socketpairs); Windows TLS coverage is the single end-to-end smoke path, not the full matrix (resumption, ALPN, client-auth edge cases). | accepted — full port is a separate effort; smoke covers handshake + verified request/response |
| T-004 | info | `cfw_server` TLS paths | Server-side TLS attach/handshake/shutdown code is platform-neutral and compiled on Windows only when `CFW_HAVE_TLS`; exercised by `wsm_tls`. | accepted |

## Verification

```text
cmake --preset debug-tls && cmake --build --preset debug-tls  # clean
ctest --preset debug-tls --output-on-failure                  # 6/6 passed
gcc -std=c11 -Wall -Wextra -Werror ... tests/windows_smoke.c  # clean (TLS build)
./windows_smoke                                              # "windows smoke: ok"
# Windows: CI windows-tls job (choco OpenSSL 3, debug-tls preset,
# ctest -R cfw_windows_smoke)
```

## Follow-up

- If the choco OpenSSL layout differs on `windows-latest`, adjust
  `OPENSSL_ROOT_DIR` or switch the job to vcpkg (T-002).
- Consider porting `test_tls.c` to TCP loopback so the full TLS suite
  runs on Windows (T-003).
