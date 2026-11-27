<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0057: Optional TLS module backed by OpenSSL

- Status: Accepted
- Date: 2026-11-27

## Context

ADR 0030 took TLS out of scope for the core library and deferred it
"pending a dependency ADR" covering license, version pinning, audit
catalog item 8 (cryptography and transport), and platform support.
Phase 6's boundary is already preserved: `cfw_http1_conn_t` consumes
an established byte stream and `cfw_server_t` binds cleartext
listeners, so a TLS module can slot in below both without changing
the HTTP layers.

Operators today must terminate TLS in a reverse proxy. A first-party
`cfw_tls` module removes that requirement for deployments that want
direct HTTPS serving.

## Decision

Add an **optional** TLS module, `cfw_tls`, backed by **OpenSSL 3.x**:

- New CMake option `CFW_ENABLE_TLS` (default `OFF`). The default
  build remains dependency-free; enabling TLS is an explicit opt-in
  that runs `find_package(OpenSSL 3)` and links `OpenSSL::SSL` +
  `OpenSSL::Crypto`.
- Public API lives in `cfw/tls.h` behind `CFW_HAVE_TLS` (probed via
  `CFW_ENABLE_TLS` and a successful OpenSSL find). When TLS support
  is not compiled in, the API exists but every entry point returns
  `CFW_ERROR_UNAVAILABLE`, matching the existing stub convention used
  for unimplemented platforms.
- The module owns certificate/context configuration
  (`cfw_tls_ctx_t`) and per-connection state (`cfw_tls_conn_t`)
  performing nonblocking `SSL_accept`/`SSL_read`/`SSL_write`/`SSL_shutdown`
  over a `cfw_fd_t` socket. It reports `CFW_ERROR_WOULDBLOCK` with
  explicit want-read/want-write hints so callers can drive it from
  any pollset.
- Server integration (accepting TLS listeners directly) is a
  follow-up; this ADR covers only the transport module.

### License and supply chain

OpenSSL is licensed under the Apache License 2.0, which is
compatible with PolyForm-Noncommercial-1.0.0 (the dependency is
consumed, not relicensed). Version policy: `find_package(OpenSSL 3)`
requires any OpenSSL 3.x; we do not pin a specific patch release —
operators track their platform OpenSSL, consistent with how system
libc/POSIX features are consumed. The OpenSSL build itself is the
distribution's responsibility; cfw performs no vendoring.

### Platform support

OpenSSL 3.x supports all cfw POSIX targets (Linux, macOS, the BSDs,
Android). Windows builds of cfw remain stubs and do not enable TLS.
Where `CFW_ENABLE_TLS` is on but OpenSSL is absent, CMake fails with
a clear message rather than silently disabling the feature.

## Alternatives considered

- **mbedTLS**: pure C, Apache-2.0, smaller surface — attractive, but
  not installed on the development or CI images and would require
  vendoring or a package dependency we cannot verify here. Revisit
  if a minimal-dependency TLS backend is wanted later; the `cfw_tls`
  API is backend-agnostic enough to swap.
- **wolfSSL / BearSSL / Rustls-via-FFI**: smaller or newer, but
  weaker platform coverage or additional FFI/toolchain requirements.
- **Custom TLS implementation**: rejected outright by ADR 0030 and
  unchanged — cryptography is never hand-rolled in this project.
- **Keep TLS permanently out of scope**: rejected; operators
  increasingly expect first-party HTTPS, and the byte-stream boundary
  makes the module cheap to add without touching HTTP parsing.

## Consequences

- `CFW_ENABLE_TLS=ON` builds add a hard link-time dependency on the
  system OpenSSL; documented in README build options.
- New audit-catalog item 8 (cryptography and transport) applies to
  `src/cfw/tls.c`: certificate handling, handshake state machine,
  nonblocking I/O mapping, and shutdown/renegotiation behavior must
  be covered by the change's audit notes.
- The cleartext-only threat model
  (`docs/security/threat-model.md`) remains accurate for default
  builds; TLS builds gain transport confidentiality for data in
  flight but cfw still does not authenticate clients unless the
  application configures peer verification.
- Follow-up work: `cfw_server_t` TLS listener integration (accept →
  handshake → hand decrypted stream to `cfw_http1_conn`), ALPN
  selection, session resumption policy, and a TLS example.
