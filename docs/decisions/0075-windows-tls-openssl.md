<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0075: TLS on Windows via OpenSSL 3

## Status

Accepted. Supersedes the "Windows builds remain stubs" note in ADR
0057; the OpenSSL backend choice itself is unchanged.

## Context

ADRs 0072–0074 made the entire cleartext HTTP/WebSocket stack work on
Windows, but `CFW_ENABLE_TLS` was hard-refused on Windows (ADR 0057)
leaving `cfw_tls_*` as `CFW_ERROR_UNAVAILABLE` stubs. Two options
existed:

- **Schannel** (Windows-native SChannel/SSPI): zero external
  dependency, but a completely separate TLS implementation — new
  context/connection lifecycle, certificate store semantics,
  ALPN/session-resumption APIs that do not map onto the existing
  `cfw_tls_*` surface — in effect a second TLS backend to maintain.
- **OpenSSL 3 on Windows**: `src/cfw/tls.c` uses only portable
  OpenSSL API (SSL_CTX/SSL objects, memory managed by OpenSSL); it
  contains no POSIX-only calls. OpenSSL ships official Windows builds.

## Decision

Enable `CFW_ENABLE_TLS` on Windows with the existing OpenSSL 3 backend.
Schannel is rejected: the `cfw_tls_*` API is already OpenSSL-shaped
(ALPN wire lists, session get/set, `SSL_MODE_*` semantics), and a
native Windows backend can be revisited later behind the same public
API if a dependency-free TLS story is ever required.

Implementation notes:

- `cfw_tls_conn_create` guards `fd` before `SSL_set_fd`: Winsock
  `SOCKET` is pointer-sized while `SSL_set_fd` (and OpenSSL's socket
  BIO internally) stores the descriptor in an `int`. Values that would
  truncate return `CFW_ERROR_UNAVAILABLE` rather than handing
  OpenSSL a corrupted descriptor.
- The socket handed to TLS is the same nonblocking Winsock socket used
  by the cleartext path; OpenSSL's socket BIO maps `WSAEWOULDBLOCK` to
  `SSL_ERROR_WANT_READ`/`WANT_WRITE`, preserving the existing
  `cfw_tls_conn_want_*` contract.
- `tests/unit/test_tls.c` remains POSIX-only: it drives TLS over
  `AF_UNIX` socketpairs, which have no Windows equivalent in this
  codebase. Windows TLS coverage lives in `tests/windows_smoke.c`
  (`wsm_tls`), which runs a real TLS handshake + verified request/
  response against `cfw_server` over loopback.
- Windows CI gains a `windows-tls` job (chocolatey OpenSSL 3) running
  the `debug-tls` preset build plus the smoke test. The POSIX TLS unit
  suite is unchanged.

## Consequences

- TLS is no longer POSIX-exclusive; `docs/platform-matrix.md` and the
  `cfw_tls`/`cfw_server` header docs are updated accordingly.
- Windows TLS builds require an OpenSSL 3 installation discoverable
  by `find_package(OpenSSL)`; Schannel remains a possible future
  backend but is not implemented.
- The `fd`-width guard is defensive: in practice Winsock `SOCKET`
  values fit in `int`, matching OpenSSL's own storage width.
