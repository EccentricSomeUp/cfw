<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->

# ADR 0063: TLS session exposure to request handlers

- Status: Accepted
- Date: 2026-11-28

## Context

The TLS module can authenticate peers — `verify_peer` enforces
server/client certificate validation and `cfw_tls_conn_peer_cert_der`
exposes the presented identity — but nothing above the transport
layer could reach it: `cfw_request_t` carried peer *address* only.
Mutual-TLS authorization therefore required applications to bypass
`cfw_server_t` or re-implement dispatch, which the roadmap listed as
deferred "authZ policy plumbing".

## Decision

Plumb the connection's TLS session through the existing
transport-metadata seam rather than adding TLS awareness to
dispatch:

- `cfw_dispatch_request_info_t` gains `const cfw_tls_conn_t
  *tls_conn` (appended last per the struct-extension convention);
  `cfw_request_t` gains the same field, copied during dispatch.
- `cfw_server_t` sets `info.tls_conn = conn->tls` — non-NULL only
  after the TLS handshake completed and only when the connection is
  actually TLS (cleartext and pre-detection connections see NULL).
- The pointer is borrowed and valid for the dispatch call only;
  handlers use the existing `cfw_tls_conn_*` accessors
  (`peer_cert_der`, `alpn`, `session_reused`) for authorization
  decisions. Policy itself stays application code — the framework
  transports identity, it does not decide authorization.

`dispatch.h` now includes `cfw/tls.h`; the header is dependency-free
in both TLS and non-TLS builds (stub declarations), so no consumer
breakage results.

## Consequences

- Handlers and middleware can implement mTLS authZ, ALPN-aware
  routing, and session-reuse telemetry without new server options.
- The field is NULL on cleartext builds/connections, so handlers
  must NULL-check — documented on the field.
- `cfw_dispatch_request_info_t` consumers that zero-initialize
  (required for the additive-field convention) need no changes.
