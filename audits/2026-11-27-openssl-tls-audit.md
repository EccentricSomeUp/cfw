<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# Audit: optional OpenSSL TLS module (ADR 0057)

- Date: 2026-11-27
- Auditor: Devin (agent)
- Commit: f9dd1a9
- Category: audit catalog item 8 — cryptography and transport
- Scope: `include/cfw/tls.h`, `src/cfw/tls.c`, `CMakeLists.txt`,
  `cmake/cfw-config.cmake.in`, `tests/unit/test_tls.c`,
  `tests/CMakeLists.txt`, `docs/decisions/0057-optional-openssl-tls.md`

## Goals

- Confirm the TLS module delegates all cryptography to OpenSSL 3.x
  and introduces no hand-rolled crypto, parsing of key material, or
  secret logging.
- Verify nonblocking I/O semantics are safe for pollset-driven event
  loops and that error mapping cannot leak handshake internals.
- Confirm certificate/private-key handling fails closed and that
  peer verification is opt-in and correctly scoped per role.
- Verify the module is truly optional: default builds contain no
  OpenSSL dependency and stubs return `CFW_ERROR_UNAVAILABLE`.

## Method

- Code review of `src/cfw/tls.c` against OpenSSL 3.0.13 semantics
  (`SSL_do_handshake` state requirements, `SSL_get_error` mapping,
  `SSL_shutdown` two-phase close).
- Unit tests over real TLS sessions on nonblocking `socketpair`s:
  handshake + bidirectional echo, WOULDBLOCK progression with
  want-read/want-write hints, client verification of the server
  certificate, rejection of an incorrect trust anchor, mandatory
  client-certificate authentication, mutual `close_notify` shutdown
  and EOF reporting.
- Debug (`debug-tls`) and default (`debug`) preset builds and CTest
  runs; install-smoke consumer verifies `find_dependency(OpenSSL)`
  propagation.

## Findings

| ID | Severity | Location | Summary | Disposition |
|----|----------|----------|---------|-------------|
| A-001 | medium | src/cfw/tls.c | `SSL_do_handshake` requires explicit `SSL_set_accept_state`/`SSL_set_connect_state`; missing initially caused `SSL_ERROR_SSL` ("connection type not set") on every handshake | fixed before commit |
| A-002 | low | src/cfw/tls.c | `SSL_shutdown` returns 0 (not an error) while awaiting the peer's `close_notify`; initial mapping reported `CFW_ERROR_IO` prematurely | fixed: ret==0 maps to `CFW_ERROR_WOULDBLOCK` + want-read |
| A-003 | info | include/cfw/tls.h | API contract reviewed: fd ownership transfer on `cfw_tls_conn_create`, `SSL_MODE_ENABLE_PARTIAL_WRITE` always set, `ACCEPT_MOVING_WRITE_BUFFER` opt-in via ctx flag | accepted |
| A-004 | info | CMakeLists.txt | TLS is opt-in (`CFW_ENABLE_TLS`, default OFF); `find_package(OpenSSL 3)` hard-fails when enabled but absent; no vendored or pinned supply chain | accepted per ADR 0057 |
| A-005 | info | tests/CMakeLists.txt | Test certificates (including private keys) are generated into the build tree at configure time; no key material is committed | accepted |

## Verification

```text
cmake --preset debug-tls && cmake --build --preset debug-tls
./build/debug-tls/tests/cfw_unit_tests --filter tls
# 8 executed, 0 failed
ctest --preset debug-tls --output-on-failure
# 6/6 passed
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure
# 6/6 passed (TLS stubs return CFW_ERROR_UNAVAILABLE)
cmake -S . -B build/san-tls -DCMAKE_BUILD_TYPE=Debug \
    -D'CFW_ENABLE_SANITIZERS=ON' -D'CFW_ENABLE_TLS=ON'
cmake --build build/san-tls
./build/san-tls/tests/cfw_unit_tests --filter tls
# 8 executed, 0 failed under ASan+UBSan
```

## Follow-up

- `cfw_server_t` TLS listener integration (accept → handshake →
  decrypted stream into `cfw_http1_conn`) is tracked as follow-up
  work in ADR 0057.
- ALPN protocol selection and session-resumption policy deferred to
  the server-integration change.
- Sanitizer-preset TLS coverage verified via a manual
  `CFW_ENABLE_SANITIZERS=ON` + `CFW_ENABLE_TLS=ON` build; a dedicated
  preset can be added if TLS+sanitizer CI coverage is wanted.
