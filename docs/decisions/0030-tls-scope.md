<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0030: TLS scope for the core library

- Status: Accepted
- Date: 2026-11-26

## Context

Phase 6 asks whether TLS is in scope. The project constraint forbids
third-party runtime dependencies without an approved ADR, and no TLS
dependency (OpenSSL, mbedTLS, BoringSSL, wolfSSL) has been approved.
Every candidate is a large supply-chain and maintenance surface; none
can be vetted to the project's audit bar within this phase. Rolling a
custom TLS stack is explicitly out of the question for a
security-focused framework.

## Decision

TLS is **out of scope for the core `cfw` library** in the current
release line. The HTTP/1.x stack operates on cleartext transport only;
operators terminate TLS in a reverse proxy (nginx, haproxy, a CDN) or a
future optional module.

The boundary is preserved for a future decision:

- `cfw_http1_conn_t` consumes an already-established byte stream; a TLS
  backend would slot in below it (decrypting into the same feed path)
  without changing the HTTP layers.
- `cfw_server_t` binds cleartext listeners only; a future `cfw_tls`
  module would own certificate loading, handshake state, and ALPN,
  exposing a transport that `cfw_server` can accept.
- Any future TLS dependency requires its own ADR covering license,
  version pinning, audit catalog item 8 (cryptography and transport),
  and platform support before merge.

## Consequences

- Phase 6's "decide whether TLS is in scope" criterion is met by this
  record; implementation is deferred indefinitely pending a dependency
  approval. (Update: that approval was granted — ADR 0057 added the
  optional OpenSSL-backed `cfw_tls` module. The core `cfw` library
  remains cleartext-only; TLS lives in the opt-in module.)
- The threat model (`docs/security/threat-model.md`) treats the server
  as cleartext-only and must not claim transport confidentiality.
- HTTP request parsing already rejects ambiguous framing, which
  matters for TLS-terminating proxies that forward to cfw.
